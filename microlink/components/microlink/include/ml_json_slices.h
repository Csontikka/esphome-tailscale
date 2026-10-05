#ifndef ML_JSON_SLICES_H
#define ML_JSON_SLICES_H

/* Contributed by gbertsch as part of the memory-optimisation reference patch
 * shared in Csontikka/esphome-tailscale#48 (MIT, like the rest of microlink).
 * The array iterator and the key comparison at the end were added here. */

/* Allocation-free, bounded JSON validation and object iteration. Large control
 * responses can then be decoded one member/DERP region at a time with cJSON. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct { const char *begin; const char *end; } ml_json_slice;
typedef struct { const char *p; const char *end; bool first; } ml_json_object;

static inline void ml_json_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) ++*p;
}

static inline bool ml_json_string(const char **p, const char *end) {
    if (*p == end || *(*p)++ != '"') return false;
    while (*p < end) {
        unsigned char c = (unsigned char)*(*p)++;
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
            if (*p == end) return false;
            c = (unsigned char)*(*p)++;
            if (c == 'u') {
                for (int i = 0; i < 4; ++i) {
                    if (*p == end) return false;
                    c = (unsigned char)*(*p)++;
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) return false;
                }
            } else if (c == 0 || !strchr("\"\\/bfnrt", c)) return false;   /* strchr matches the NUL */
        }
    }
    return false;
}

static inline bool ml_json_value(const char **p, const char *end, unsigned depth) {
    ml_json_ws(p, end);
    if (*p == end || depth > 24) return false;   /* netmaps nest ~8 deep; cJSON then recurses on the same slice */
    if (**p == '"') return ml_json_string(p, end);
    if (**p == '{' || **p == '[') {
        bool object = *(*p)++ == '{';
        char close = object ? '}' : ']';
        ml_json_ws(p, end);
        if (*p < end && **p == close) { ++*p; return true; }
        while (*p < end) {
            if (object) {
                if (!ml_json_string(p, end)) return false;
                ml_json_ws(p, end);
                if (*p == end || *(*p)++ != ':') return false;
            }
            if (!ml_json_value(p, end, depth + 1)) return false;
            ml_json_ws(p, end);
            if (*p == end) return false;
            if (**p == close) { ++*p; return true; }
            if (*(*p)++ != ',') return false;
            ml_json_ws(p, end);
        }
        return false;
    }
    const char *literal = **p == 't' ? "true" : **p == 'f' ? "false" : **p == 'n' ? "null" : NULL;
    if (literal) {
        size_t n = strlen(literal);
        if ((size_t)(end - *p) < n || memcmp(*p, literal, n)) return false;
        *p += n;
        return true;
    }
    if (**p == '-') ++*p;
    if (*p == end) return false;
    if (**p == '0') ++*p;
    else {
        if (**p < '1' || **p > '9') return false;
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    }
    if (*p < end && **p == '.') {
        ++*p;
        if (*p == end || **p < '0' || **p > '9') return false;
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    }
    if (*p < end && (**p == 'e' || **p == 'E')) {
        ++*p;
        if (*p < end && (**p == '+' || **p == '-')) ++*p;
        if (*p == end || **p < '0' || **p > '9') return false;
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    }
    return true;
}

static inline bool ml_json_object_init(ml_json_object *it, ml_json_slice s) {
    const char *p = s.begin;
    ml_json_ws(&p, s.end);
    if (p == s.end || *p != '{') return false;
    const char *start = p++;
    it->p = p; it->end = s.end; it->first = true;
    p = start;
    if (!ml_json_value(&p, s.end, 0)) return false;
    ml_json_ws(&p, s.end);
    return p == s.end;
}

/* Only called after object_init successfully validates the complete object. */
static inline bool ml_json_object_next(ml_json_object *it, ml_json_slice *key, ml_json_slice *value) {
    ml_json_ws(&it->p, it->end);
    if (it->p == it->end || *it->p == '}') return false;
    if (!it->first) { ++it->p; ml_json_ws(&it->p, it->end); }
    it->first = false;
    key->begin = it->p;
    if (!ml_json_string(&it->p, it->end)) return false;
    key->end = it->p;
    ml_json_ws(&it->p, it->end);
    ++it->p;
    ml_json_ws(&it->p, it->end);
    value->begin = it->p;
    if (!ml_json_value(&it->p, it->end, 0)) return false;
    value->end = it->p;
    return true;
}

/* --- additions for esphome-tailscale (#48) -------------------------------- */

typedef struct { const char *p; const char *end; bool first; } ml_json_array;

/* Iterate the elements of an array slice that an enclosing ml_json_object_init
 * has already validated. Returns false if the slice is not an array. */
static inline bool ml_json_array_init(ml_json_array *it, ml_json_slice s) {
    const char *p = s.begin;
    ml_json_ws(&p, s.end);
    if (p == s.end || *p != '[') return false;
    it->p = p + 1; it->end = s.end; it->first = true;
    return true;
}

static inline bool ml_json_array_next(ml_json_array *it, ml_json_slice *value) {
    ml_json_ws(&it->p, it->end);
    if (it->p >= it->end || *it->p == ']') return false;
    if (!it->first) { ++it->p; ml_json_ws(&it->p, it->end); }
    it->first = false;
    value->begin = it->p;
    if (!ml_json_value(&it->p, it->end, 0)) return false;
    value->end = it->p;
    return true;
}

/* True if the key slice (a JSON string, quotes included) is exactly "name". */
static inline bool ml_json_key_is(ml_json_slice key, const char *name) {
    size_t n = strlen(name);
    return (size_t)(key.end - key.begin) == n + 2 && key.begin[0] == '"' &&
           memcmp(key.begin + 1, name, n) == 0;
}

#endif
