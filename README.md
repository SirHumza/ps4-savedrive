# ps4-savedrive

PS4 payload daemon that zips your savedata and uploads it to Google Drive on every jailbreak. Incremental after the first run.

Status: plan stage. Implementation follows `docs/plans/2026-10-10-save-drive-daemon.md`, task by task.

## Layout

- `src/` — payload sources (`probe/`, `sd_cfg`, `zip`, `manifest`, `drive`, `main`)
- `tools/provision.py` — one-time Mac helper: Google OAuth → refresh token → `config.json` you FTP to `/data/savedrive/`
- `tests/` — host tests (run on Mac, never on console)
- `docs/plans/` — implementation plan

## Build

Requires the OpenOrbis SDK built from git (never the release ZIP — CRT firmware gate). Details in the plan.

## Credits

Common modules (`tls.c` + CA bundle, `jsonlite.c`, `log.c`, BearSSL objects, Makefile patterns) vendored from [SirHumza/orbisRPC](https://github.com/SirHumza/orbisRPC) with thanks. `miniz` is public domain.
