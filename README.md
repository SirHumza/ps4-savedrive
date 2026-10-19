# ps4-savedrive

A PS4 payload that backs up your savedata to Google Drive on every jailbreak.
Runs on the console itself — no PC, no FTP bridge, no resident daemon. Each
jailbreak it walks both users' `/user/home/*/savedata/*`, zips every title, and
uploads it to a Drive folder you own. Unchanged titles are skipped, so the
first run is full and later runs are near-instant.

Status: host-tested end to end (zip, manifest, resumable-upload framing, config
loading). Not yet run on a console — the hardware steps are in
`docs/plans/2026-10-10-save-drive-daemon.md`.

## How it works

1. `tools/provision.py` (Mac, one time) runs the Google OAuth flow and prints a
   `config.json` you copy to `/data/savedrive/config.json`.
2. On every jailbreak, `savedrive.bin` runs from the GoldHEN payload autorun
   list: it loads the config, refreshes an access token, walks the saves,
   skips titles whose size+mtime match `manifest.txt`, zips the rest, and
   uploads each zip as `<user>+<title>+<date>.zip`.
3. Failures are logged and retried next boot. The payload always exits 0, so
   a network problem can never block boot.

## Layout

```
src/            payload sources (main, drive, zip, manifest, sd_cfg)
src/probe/      standalone FS probe (proves savedata is readable)
src/third_party/mbedtls/  TLS backend
tools/          provision.py + tests
tests/          host tests (run on the Mac, never on the console)
scripts/        deploy.sh (anonymous FTP helpers)
docs/plans/     implementation plan
```

## Build

Requires the ps4-payload-dev SDK built from git (never the release ZIP — its
CRT is gated on a per-firmware table and dies silently on newer firmware) and
an `orbis-clang` toolchain.

```
make -C src            # -> build/savedrive.elf
make -C src check      # fails if app-world libkernel.so leaks into NEEDED
make -C src/probe      # -> src/probe/probe.elf
make -C tests test      # host tests: config, zip/manifest, drive framing
python3 tools/test_provision.py
```

The linked payload must import only `libkernel_web.sprx`,
`libSceLibcInternal.sprx` and `libSceNet.sprx`. Anything from the app world
(notably `libkernel.so`) makes it die before `main()` with no log.

## Provision

```
python3 tools/provision.py
```

Follow the four steps it prints (Cloud Console → OAuth client → folder → auth
URL), then copy the printed JSON to `/data/savedrive/config.json` on the PS4:

```
scripts/deploy.sh deploy   # build/savedrive.elf -> /data/payloads/savedrive.bin
scripts/deploy.sh probe    # src/probe/probe.elf -> /data/payloads/sdprobe.bin
scripts/deploy.sh fetch    # pull run.log / probe.txt back for inspection
```

Add `savedrive.bin` to `/data/payloads/payloads.ini` to run it on every
jailbreak. Logs land in `/data/savedrive/run.log` and on klog (TCP 3232).

## Config and secrets

`config.json` holds a long-lived OAuth refresh token; treat it like a
password. It is git-ignored and never committed. The payload only reads it and
never writes secrets to any log. The provisioner uses the full Drive scope
because a pre-created `folder_id` cannot be addressed with `drive.file`.

## Limitations

- Raw (still encrypted) saves: a backup restores to the same console, not
  another account. Decryption is out of scope.
- Torn-read detection is size+mtime; a same-size rewrite inside one mtime
  second is not detected.
- Empty titles are reported `skip`, not backed up.
- First run over Wi-Fi can take a while for multi-GB save sets. Uploads are
  resumable: a dropped connection or partially persisted chunk resumes from
  the byte position the server reports, and an interrupted title retries on
  the next jailbreak.

## Credits

`miniz` is public domain. The TLS client, log, compat and CA-bundle modules
are vendored from [SirHumza/orbisRPC](https://github.com/SirHumza/orbisRPC),
which also documents the daemon-world build and payload-loading pitfalls this
project follows. mbedTLS is Apache-2.0.
