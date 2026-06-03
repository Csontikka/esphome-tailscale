# Handoff — Excel-style filter + sort + row-limit for admin tables

**What this gives you:** any server-rendered HTML `<table>` (e.g. a telemetry
admin dashboard) gets, with zero extra requests:
- a **row-limit `<select>`** (50 / 100 / 250 / 500 / 1000), default **50**, while
  the server is allowed to ship up to ~1000 rows;
- a **per-column filter** input under every header (Excel "AutoFilter" style,
  case-insensitive *contains*, multiple columns AND-combine);
- **click-a-header sorting** (toggle asc/desc, `▲`/`▼` indicator), numeric-aware;
- a **"showing X of Y (filtered from Z)"** counter.

It is **100% client-side** over the already-rendered rows — no API, no JSON
embed, no framework. The server just renders more rows (LIMIT 1000) and the
browser filters/sorts/limits them.

This was implemented for two tables on a Cloudflare-Worker dashboard whose whole
HTML is a JS **template literal** (backtick string). That constraint matters:
**the injected `<script>` must contain NO backticks and no `${...}`** or it will
terminate the server-side template literal. Use single/double quotes + `+`
concatenation only. (If your dashboard is a static `.html` file, this caveat
doesn't apply, but the no-backtick version still works fine.)

---

## 1. Server: raise the row cap

Change the query that feeds the table from a small `LIMIT` to `LIMIT 1000`:

```sql
... ORDER BY ts DESC LIMIT 1000      -- was LIMIT 50
```

The client defaults to showing 50, so the page isn't visually flooded; the rest
are present in the DOM (hidden) for filtering/sorting.

---

## 2. HTML: give the table an id, add controls + a filter row

Per table you need three ids: `<x>-table`, `<x>-limit`, `<x>-count`. The header
gets a **second `<thead>` row** (`class="filter-row"`) with one `<input data-col="N">`
per column, where `N` is the 0-based column index (must match the data columns
left-to-right).

```html
<div class="panel" style="margin-bottom:24px">
  <h2>Recent activity <span id="rec-count" class="rec-count"></span></h2>
  <div class="rec-controls">Show
    <select id="rec-limit"><option>50</option><option>100</option><option>250</option><option>500</option><option>1000</option></select>
    rows · type under a header to filter (Excel-style) · click a header to sort</div>
  <div class="scroll"><table id="rec-table">
    <thead>
      <tr><th>Time</th><th>Device</th><th>Type</th>... (your real headers) ...</tr>
      <tr class="filter-row">
        <th><input data-col="0" placeholder="filter"></th>
        <th><input data-col="1" placeholder="filter"></th>
        <th><input data-col="2" placeholder="filter"></th>
        ... one <th><input data-col="N"> per column ...
      </tr>
    </thead>
    <tbody>${recRows || '<tr><td colspan="13">no data</td></tr>'}</tbody>
  </table></div>
</div>
```

Notes:
- The **first** `<thead>` row = the real (sortable) headers.
- The **second** row (`.filter-row`) = the filter inputs; one per column, in
  order. `data-col` is the column index (0-based) but the script actually keys
  off the input's DOM order, so just keep them aligned with the columns.
- Keep the `colspan` on the "no data" row equal to the column count.

---

## 3. CSS (add to your stylesheet)

```css
.rec-count{font-size:12px;font-weight:400;color:var(--mut);margin-left:8px;text-transform:none;letter-spacing:0}
.rec-controls{padding:8px 16px;color:var(--mut);font-size:12px;border-bottom:1px solid var(--border)}
.rec-controls select{background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:2px 6px;font:12px inherit}
.filter-row th{padding:4px 8px;background:rgba(255,255,255,.02)}
.filter-row input{width:100%;box-sizing:border-box;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:3px 6px;font:11px ui-monospace,Menlo,Consolas,monospace}
.filter-row input::placeholder{color:var(--mut);opacity:.5}
thead tr:first-child th{cursor:pointer;user-select:none}
thead tr:first-child th:hover{color:var(--fg)}
```

(`--bg`/`--fg`/`--mut`/`--border` are this dashboard's CSS variables — swap for
your own colors.)

---

## 4. JavaScript: one reusable `enhance()`, called per table

Put this `<script>` just before `</body>`. **No backticks / no `${}`** inside —
quotes + `+` only (see the template-literal caveat above).

```html
<script>
(function(){
  function enhance(tableId, limitId, countId, defCol, defDir){
    var table=document.getElementById(tableId);
    if(!table||!table.tBodies[0])return;
    var tbody=table.tBodies[0];
    var allRows=Array.prototype.slice.call(tbody.rows);
    var limitSel=document.getElementById(limitId);
    var counter=document.getElementById(countId);
    var filters=Array.prototype.slice.call(table.querySelectorAll('.filter-row input'));
    var headers=Array.prototype.slice.call(table.querySelectorAll('thead tr:first-child th'));
    var labels=headers.map(function(th){return th.textContent;});
    var sortCol=defCol, sortDir=defDir;
    function cellText(row,i){var c=row.cells[i];return c?(c.textContent||'').replace(/\s+/g,' ').trim():'';}
    function num(s){s=(s||'').trim();return /^-?\d+(\.\d+)?$/.test(s)?parseFloat(s):NaN;}
    function apply(){
      var fv=filters.map(function(i){return (i.value||'').toLowerCase();});
      var matched=allRows.filter(function(row){
        for(var i=0;i<fv.length;i++){if(fv[i]&&cellText(row,i).toLowerCase().indexOf(fv[i])<0)return false;}
        return true;
      });
      matched.sort(function(a,b){
        var x=cellText(a,sortCol),y=cellText(b,sortCol),nx=num(x),ny=num(y),c;
        if(!isNaN(nx)&&!isNaN(ny))c=nx-ny; else c=(x<y?-1:(x>y?1:0));
        return c*sortDir;
      });
      var limit=parseInt(limitSel.value,10)||50, shown=0;
      matched.forEach(function(row){if(shown<limit){tbody.appendChild(row);row.style.display='';shown++;}else{row.style.display='none';}});
      allRows.forEach(function(row){if(matched.indexOf(row)<0)row.style.display='none';});
      var anyF=fv.some(function(v){return v;});
      counter.textContent='showing '+shown+' of '+matched.length+((anyF&&matched.length!==allRows.length)?(' (filtered from '+allRows.length+')'):'');
      headers.forEach(function(th,i){th.textContent=labels[i]+(i===sortCol?(sortDir>0?' ▲':' ▼'):'');});
    }
    limitSel.addEventListener('change',apply);
    filters.forEach(function(i){i.addEventListener('input',apply);});
    headers.forEach(function(th,i){th.addEventListener('click',function(){if(sortCol===i){sortDir=-sortDir;}else{sortCol=i;sortDir=1;}apply();});});
    apply();
  }
  enhance('rec-table','rec-limit','rec-count',0,-1);  /* default: column 0 (Time) descending */
  enhance('top-table','top-limit','top-count',5,-1);  /* default: column 5 (Events) descending */
})();
</script>
```

### How it works
- On load it snapshots all `<tbody>` rows (`allRows`).
- `apply()` is the single pipeline: **filter** (every non-empty column filter
  must be a substring of that column's cell text) → **sort** (by the active
  column; numeric if both cells are pure numbers, else lexicographic) →
  **limit** (re-append the first N matched rows in sorted order, hide the rest
  and all non-matched) → update the **counter** and the header `▲/▼`.
- Re-running `apply()` always re-derives from the original `allRows`, so
  filtering/sorting/limit changes compose correctly.

### To add it to a NEW table
1. Give the table `id`, add the controls bar (`<select id="x-limit">` +
   `<span id="x-count">`), and the `.filter-row` with one `<input>` per column.
2. Bump that table's server query to `LIMIT 1000`.
3. Add one line: `enhance('x-table','x-limit','x-count', <defaultSortCol>, <defaultSortDir>);`
   (`defaultSortDir`: `-1` = descending, `1` = ascending).

---

## 5. Caveats / gotchas
- **Backticks:** if the page HTML is a JS template literal (Cloudflare Worker,
  some SSR), the `<script>` must have NO backticks and NO `${}`. The code above
  already obeys this. The `▲/▼` arrows are written as `▲`/`▼` for the
  same reason (avoid any literal that could confuse the outer template).
- **Column index = input order.** The Nth `.filter-row input` filters the Nth
  `<td>`. If a cell uses `colspan`, the indexing shifts — keep one cell per
  column.
- **Sort type:** a cell sorts numerically only if its *whole* trimmed text is a
  plain number (e.g. `447`). `"1d"`, `"57.0K"`, hex hashes → lexicographic. ISO
  timestamps (`2026-06-03 14:22:55`) sort correctly lexicographically, so the
  Time column works without special handling.
- **Payload size:** shipping 1000 rows of HTML is fine for an admin page
  (~hundreds of KB). If a table could have *many thousands* of rows, switch to
  server-side pagination instead.
- **`<details>` / hidden tables:** `enhance()` runs once on load; if the table
  starts hidden, that's fine (it operates on the DOM regardless of visibility).

---

## 6. Exact diff applied here (for reference)
Two tables on `telemetry/worker.js`:
- **Recent activity** — query `LIMIT 50 → 1000`; default sort = Time desc (col 0).
- **Top devices** — query `LIMIT 20 → 1000`; default sort = Events desc (col 5).
Plus the shared CSS (§3) and the single `enhance()` script (§4). Deploys as a
normal Worker script update; no schema or binding change.
