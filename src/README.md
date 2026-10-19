# savedrive payload

## Build

    make -C src          # -> build/savedrive.elf (raw ELF; SDK has no .fself rule)
    make -C src check    # fails if libkernel.so (app world) leaked into NEEDED

## Deploy

    scripts/deploy.sh deploy   # -> /data/payloads/savedrive.bin
    scripts/deploy.sh probe    # -> /data/payloads/sdprobe.bin
    scripts/deploy.sh fetch    # -> build/run.log, build/probe.txt

Override the console with `PS4_HOST` / `PS4_PORT` (default 192.168.1.136:2121).

## Autorun

Add `savedrive.bin` to `/data/payloads/payloads.ini`. It then runs once on
every jailbreak and exits; nothing stays resident.

## Config

`/data/savedrive/config.json` (client_id, client_secret, refresh_token,
folder_id) is produced by `tools/provision.py` and copied to the console.
Missing or bad config logs one line and exits without blocking boot.

## Logs

`/data/savedrive/run.log`, mirrored to klog (TCP 3232). One line per title:
`TITLE <user>+<title> zip|skip|fail|retry <bytes>`, then a `SUMMARY` line.

## Steady state

Each jailbreak is an incremental backup: unchanged titles are skipped using
size+mtime from `manifest.txt`. Delete `/data/savedrive/manifest.txt` to force
a full re-upload. A failed upload is retried on the next jailbreak.
