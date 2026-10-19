# ps4-savedrive

A PS4 payload that backs up your savedata to Google Drive on every jailbreak.
Runs on the console itself — no PC, no FTP bridge, no resident daemon. Each
boot it walks both users' `/user/home/*/savedata/*`, zips every title, and
uploads it to a Drive folder you own. Unchanged titles are skipped, and
interrupted uploads resume where they stopped instead of starting over.

Status: host-tested end to end (zip, manifest, resumable-upload framing,
cross-boot resume, chunked responses, config). Not yet run on a console.

## Quick start

**1. Provision (once, on your Mac).**

```
python3 tools/provision.py
```

Follow the four steps it prints (Cloud Console → OAuth client → folder → auth
URL), then copy the printed JSON to `/data/savedrive/config.json` on the PS4.

**2. Build and deploy.**

```
make -C src
scripts/deploy.sh deploy   # build/savedrive.elf -> /data/payloads/savedrive.elf
```

**3. Run it after each jailbreak.** With GoldHEN's PayLoader server enabled,
send the ELF from your Mac:

```
nc <ps4-ip> 9090 < build/savedrive.elf
```

GoldHEN 2.4b18.8+ loads both ELF and BIN payloads. There is no payload autorun
in public GoldHEN builds (no `payloads.ini`); GoldHEN 2.4b18.10+ adds an
in-menu PayLoader LaunchPad/AutoRun queue, which you configure from its menu.
Payload Guest cannot run this ELF (it executes flat shellcode). Progress lands
in `/data/savedrive/run.log` and on klog (TCP 3232); `scripts/deploy.sh fetch`
pulls the log back to the Mac.

## How it works

1. Loads `config.json` and refreshes a short-lived access token from your
   long-lived refresh token.
2. For each user and title, sums file sizes and the newest mtime. When
   `<user>,<title>,<bytes>,<mtime>` matches the manifest, the title is skipped
   — so the first run is full and later runs are near-instant.
3. Everything else is zipped to `/data/savedrive/<user>+<title>.zip` and
   uploaded as `<user>+<title>+<date>.zip` via a Drive resumable session.
4. Success records the manifest entry and deletes the zip. Failures are logged
   and retried on the next jailbreak. The payload always exits 0, so a network
   problem can never block boot.

The loader hands the payload the standard homebrew world — uid 0 with the real
filesystem root (`ROOTVNODE`) and PRISON0 (verified in the ps4-payload-dev
elfldr sources, whose loader model GoldHEN's PayLoader follows) — so
`/user/home` and `/data` are directly readable.

## Stability

- **Resumable chunks.** Each zip goes up in 8 MiB chunks. If the server
  persists only part of a chunk, the next request starts at the exact byte the
  server reports — never at a guessed offset.
- **Cross-boot resume.** The session URL, zip size and savedata signature are
  kept in `/data/savedrive/state/<user>+<title>.state` while an upload is
  unfinished. If the console reboots or the network dies mid-upload, the next
  run asks Drive where the session stopped and continues from there; the zip
  is not rebuilt. If the session has expired (Drive answers 404), a fresh
  session starts from zero without re-zipping. State is dropped on success and
  on fatal errors.
- **Retry classification.** Network errors, HTTP 5xx and 429 are retryable;
  bad credentials and other 4xx are fatal for that title. Either way the title
  retries on the next jailbreak.
- **Chunked responses.** Server responses using `Transfer-Encoding: chunked`
  (Drive occasionally does this) are handled.
- **No double runs.** A lock at `/data/savedrive/run.lock` stops a second
  payload instance (an accidental double send) from running a parallel backup
  over the same zip and state files. A lock whose process is gone, whose
  timestamp is unusable (missing, zero, or far in the future after a clock
  rollback), or is older than 24 hours is taken over automatically, so a crash
  or power loss cannot wedge future runs; release only removes a lock this
  process still owns.
- **Log rotation.** `run.log` is capped at 512 KB.

## Build

Requires the ps4-payload-dev SDK built from git (never the release ZIP — its
CRT is gated on a per-firmware table and dies silently on newer firmware) and
an `orbis-clang` toolchain.

```
make -C src            # -> build/savedrive.elf
make -C src check      # fails if app-world libkernel.so leaks into NEEDED
make -C src/probe      # -> src/probe/probe.elf
make -C tests test     # host tests: config, zip/manifest, drive framing/resume
python3 tools/test_provision.py
```

The linked payload must import only `libkernel_web.sprx`,
`libSceLibcInternal.sprx` and `libSceNet.sprx`. Anything from the app world
(notably `libkernel.so`) makes it die before `main()` with no log.

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

## Config and secrets

`config.json` holds a long-lived OAuth refresh token; treat it like a
password. It is git-ignored and never committed. The payload only reads it and
never writes secrets to any log. The provisioner uses the full Drive scope
because a pre-created `folder_id` cannot be addressed with `drive.file`.

## Troubleshooting

- **Nothing appears on Drive.** Check `/data/savedrive/run.log`. No file at
  all means the payload did not run — confirm you sent `build/savedrive.elf`
  to port 9090 and that `make -C src check` passes. `config ... unavailable`
  means step 1 was skipped or the JSON landed in the wrong path.
- **A title keeps failing.** Look for `TITLE <user>+<title> fail|retry <bytes>`
  in the log. `retry` is network/5xx (it resumes next boot); `fail` is
  auth/4xx. Re-run `tools/provision.py` if credentials were revoked.
- **TLS or certificate errors.** The console clock must be roughly right —
  fix via Settings → Date and Time (Internet). There is no proxy support.
- **Upload speed.** First run over Wi-Fi can take a while for multi-GB save
  sets; later runs only touch changed titles.

## Limitations

- Raw (still encrypted) saves: a backup restores to the same console, not
  another account. Decryption is out of scope.
- Google Drive is the only backend by design.
- Torn-read detection is size+mtime; a same-size rewrite inside one mtime
  second is not detected.
- Empty titles are reported `skip`, not backed up.

## Credits

`miniz` is public domain. The TLS client, log, compat and CA-bundle modules
are vendored from [SirHumza/orbisRPC](https://github.com/SirHumza/orbisRPC),
which also documents the daemon-world build and payload-loading pitfalls this
project follows. mbedTLS is Apache-2.0.
