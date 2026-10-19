# savedrive payload

## Build

    make -C src          # -> build/savedrive.elf (raw ELF; SDK has no .fself rule)
    make -C src check    # fails if libkernel.so (app world) leaked into NEEDED

## Deploy

    scripts/deploy.sh deploy   # -> /data/payloads/savedrive.elf
    scripts/deploy.sh probe    # -> /data/payloads/sdprobe.elf
    scripts/deploy.sh fetch    # -> build/run.log, build/probe.txt

Override the console with `PS4_HOST` / `PS4_PORT` (default 192.168.1.136:2121).

## Running

There is no payload autorun in public GoldHEN builds (and no `payloads.ini`).
After each jailbreak, send the ELF to GoldHEN's PayLoader server:

    nc <ps4-ip> 9090 < build/savedrive.elf

GoldHEN 2.4b18.8+ accepts ELF and BIN payloads. The payload runs once and
exits; nothing stays resident. Payload Guest cannot run this ELF because it
executes flat shellcode.

## Config

`/data/savedrive/config.json` (client_id, client_secret, refresh_token,
folder_id) is produced by `tools/provision.py` and copied to the console.
Missing or bad config logs one line and exits without blocking boot.

## Logs

`/data/savedrive/run.log`, mirrored to klog (TCP 3232). One line per title:
`TITLE <user>+<title> zip|skip|fail|retry <bytes>`, then a `SUMMARY` line.

A second instance started while one is running logs `already running, exit`
and stops; its lock (`/data/savedrive/run.lock`) is taken over automatically
when the previous process is gone or the lock is older than 24 hours.

## Steady state

Each jailbreak is an incremental backup: unchanged titles are skipped using
size+mtime from `manifest.txt`. Delete `/data/savedrive/manifest.txt` to force
a full re-upload. A failed upload is retried on the next jailbreak.
