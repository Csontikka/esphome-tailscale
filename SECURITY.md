# Security Policy

## Reporting a Vulnerability

If you think you've found a security issue in this component — for example a memory-corruption bug in the C++ glue, a way to leak the auth key out of NVS, or any other concern that affects the safety of a device running this code — please report it privately rather than opening a public issue.

**Preferred channel:** use GitHub's [private vulnerability reporting](https://github.com/Csontikka/esphome-tailscale/security/advisories/new) on this repository. This creates a private security advisory that only the maintainer and invited collaborators can see.

If that isn't available to you, you can also email the maintainer directly (see the GitHub profile at [@Csontikka](https://github.com/Csontikka)).

Please include:

- A description of the issue and why you think it's a security problem.
- The exact commit / version of `esphome-tailscale` you observed it on.
- Steps to reproduce, if possible — ideally a minimal YAML config and a description of the runtime behavior.
- Any logs, crash dumps, or serial output that illustrates the problem.

I will acknowledge the report within a few days, work with you to confirm the issue, and coordinate a fix and disclosure timeline.

## Scope

This repository is only the **ESPHome wrapper** around [microlink](https://github.com/CamM2325/microlink). If the vulnerability is in the underlying Tailscale/WireGuard protocol stack itself, please consider reporting it upstream to the microlink project as well — the fix will likely need to land there first.

## Supported Versions

This project is under **heavy development**. Only the current `main` branch is supported for security fixes. Older commits and unreleased snapshots are not maintained.

## Non-affiliation notice

This project is **not** affiliated with, sponsored by, or endorsed by Tailscale Inc., Jason A. Donenfeld, or the WireGuard project. Please do not report Tailscale-service or WireGuard-protocol vulnerabilities here — report those to the respective upstream projects.
