# PS4 Save-to-Drive Daemon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A PS4 payload that zips both users' raw savedata and uploads it to Google Drive, running on every jailbreak.

**Architecture:** Standalone repo. Common modules (`tls.c` + CA bundle, `jsonlite.c`, `log.c`, mbedTLS objects, Makefile patterns) vendored from SirHumza/orbisRPC with attribution in the README. One-time Mac-side Python helper provisions a Google OAuth refresh token into `/data/savedrive/config.json`; the payload swaps it for an access token at runtime and does Drive resumable uploads, one zip per title, skipping unchanged titles via a manifest.

**Tech Stack:** C (OpenOrbis SDK, daemon-world libs only), miniz (public domain zip), Google Drive resumable upload API, Python 3 (provisioning helper + host tests).

**Spec:** Inline — user answers: PS4 payload daemon (not Mac-side), raw encrypted saves (restorable to this PS4). Measured: `/user/home/1898cd02/savedata` = 2.07GB/302 files, `1898cd03/savedata` = 118MB/38 files; `savedata_meta` + `trophy` are small and included.

## Global Constraints

- Daemon-world only: `libkernel_web.sprx`, `libSceLibcInternal.sprx`, `libSceNet.sprx`. Never link `libkernel.so` or any app-world `.so` (dies before `main()`, no log).
- SDK built from git, never the release ZIP (CRT firmware gate).
- No GPL code (license undecided); miniz is public domain, OK. No LICENSE file.
- Never commit secrets (Drive refresh token, client secret). CI-style secret scan on every task.
- Host-test every change (`make -C tests`-style); on-console runs are verified by the user, never assumed.
- No `malloc`-heavy unbounded designs; fixed-size tables where the codebase does (`sd_cfg.h` pattern).
- Never log secret material (tokens, auth headers). Debug logs metadata only.

## Review Focus

- Refresh token ending up in `log.txt` or a crash dump: payload must scrub secrets before any log write; add a test that feeds a fake token through every log path and asserts absence.
- Upload dying mid-zip (rest mode, network drop) leaving a corrupt partial on Drive: resumable sessions must delete-or-complete, never leave partials advertised as backups.
- A game writing its save mid-zip producing a torn archive: locked/changed-during-read files must abort that title with a retry-next-boot marker, not a bad zip.
- First-run 2.2GB upload over PS4 WiFi timing out the jailbreak session: chunked PUTs with progress log lines; user can stop and resume next boot.
- Two users' saves colliding on Drive: zip names must namespace user id + title id (`1898cd02+CUSA12345+2026-10-10.zip`).

---

### Task 1: FS probe payload (prove saves are readable)

**Files:**
- Create: `src/probe/main.c`, `src/probe/Makefile` (daemon-world libs only)
- Test: user-run on console; evidence = `/data/savedrive/probe.txt`

**Interfaces:**
- Consumes: nothing (standalone, ~80 lines + vendored `log.c` if trivial, else `sceKernelDebugOutText`)
- Produces: `probe.txt` line format `USER/TITLE/FILES/BYTES/READ_OK` — Task 3's zip walker must accept exactly these paths

- [ ] **Step 1: Write `src/probe/main.c`** that walks `/user/home/*/savedata/*`, opens every file read-only, sums sizes, writes one line per title to `/data/savedrive/probe.txt`
- [ ] **Step 2: Build** with the OpenOrbis SDK

Run: `make -C src/probe`
Expected: clean build, `probe.elf`, no `libkernel.so` in link

- [ ] **Step 3: Deploy via FTP** to `/data/payloads/` and have the user launch it once via BinLoader

Run (Mac): FTP STOR `probe.elf` → `/data/payloads/sdprobe.elf`
Expected: remote size matches local

- [ ] **Step 4: User pastes back `probe.txt`; verify** every title dir shows `READ_OK` and byte totals ≈ FTP-measured 2.07GB + 118MB
- [ ] **Step 5: Commit**

```bash
git add src/probe
git commit -m "feat: FS probe proves savedata readable"
```

Ruling (pre-recorded): if any title is unreadable from payload context, the whole project pivots to Mac-side FTP pull; do not build further until Task 1 passes.

### Task 2: Drive auth provisioning (one-time Mac helper + PS4 config)

**Files:**
- Create: `tools/provision.py`, `src/sd_cfg.h` (config schema)
- Test: `tools/test_provision.py`

**Interfaces:**
- Consumes: nothing
- Produces: `/data/savedrive/config.json` with fields `client_id`, `client_secret`, `refresh_token`, `folder_id`; loader `int sd_cfg_load(const char *path, sd_cfg_t *c)` in `src/sd_cfg.c` — Task 4 consumes this exact signature

- [ ] **Step 1: Write failing test** asserting `provision.py --help` prints the 4 setup steps (Cloud Console → OAuth client → auth URL → paste code) and that `sd_cfg_load` rejects a config missing `refresh_token`

Run: `python3 tools/test_provision.py -v`
Expected: FAIL (files don't exist)

- [ ] **Step 2: Write `provision.py`** (builds auth URL, exchanges code for refresh token via `oauth2.googleapis.com/token`, writes nothing to disk except printing the JSON block the user FTPs over) and `sd_cfg.h/.c` (fixed-size char arrays, atomic save)
- [ ] **Step 3: Run host tests**

Run: `python3 tools/test_provision.py -v` + `make -C tests test_sd_cfg`
Expected: PASS; token-exchange HTTP mocked, asserts refresh_token parsed and persisted

- [ ] **Step 4: User runs the real provisioning** (creates own free Google Cloud OAuth client, pastes code) and FTPs `config.json` to `/data/savedrive/`; verify by downloading it back and checking the 4 fields exist (values never printed)
- [ ] **Step 5: Commit**

```bash
git add tools/provision.py tools/test_provision.py src/sd_cfg.*
git commit -m "feat: OAuth provisioning + config schema"
```

### Task 3: Per-title zip + unchanged-skip manifest (host-tested)

**Files:**
- Create: `src/zip.c`, `src/zip.h`, `src/manifest.c`, `src/manifest.h` (+ vendored `miniz.c/.h`, public domain)
- Test: `tests/test_sdzip.c`

**Interfaces:**
- Consumes: probe.txt path format from Task 1
- Produces: `int sd_zip_title(const char *user, const char *title, const char *out_zip)` (0 ok, -1 changed-mid-read/locked) and `int sd_manifest_skip(const char *user, const char *title, size_t bytes, long mtime)` (1 = skip, 0 = upload) — Task 5 consumes both

- [ ] **Step 1: Write failing host test** with a fixture dir (3 files, known bytes): zip it, assert `unzip -l` lists all 3 with exact sizes; assert manifest skips on second identical call and re-uploads after mtime bump

Run: `cc tests/test_sdzip.c src/miniz.c -o /tmp/t && /tmp/t`
Expected: FAIL (no sources)

- [ ] **Step 2: Implement `zip.c`/`manifest.c`**; torn-read detection = re-stat size+mtime after zipping, mismatch → delete zip, return -1
- [ ] **Step 3: Run host test green**, plus a 2GB-sparse-file perf sanity (zip streams, no whole-file malloc)

Run: same as Step 1 + `ls -l` fixture zips
Expected: PASS, memory flat

- [ ] **Step 4: Commit**

```bash
git add src/zip.* src/manifest.* src/miniz.* tests/test_sdzip.c
git commit -m "feat: per-title zip + skip manifest"
```

### Task 4: Drive resumable upload over `tls.c` (host-test framing, console-test bytes)

**Files:**
- Create: `src/drive.c`, `src/drive.h`
- Test: `tests/test_sddrive.c` (framing only, against a local Python fake-`upload.googleapis.com`)

**Interfaces:**
- Consumes: `tls_start/tls_write/tls_read` (vendored `tls.h`), `sd_cfg_load` (Task 2)
- Produces: `int sd_drive_put(const sd_cfg_t *c, const char *zip_path, const char *drive_name)` (0 uploaded, -1 fatal, -2 retryable) — Task 5 consumes this

- [ ] **Step 1: Write failing framing test**: fake server expects `POST /upload/drive/v3/files?uploadType=resumable` with JSON metadata, then `PUT <session-uri>` with `Content-Range: bytes 0-N/T`; assert client sends both and parses `201`/`200` resume states

Run: `make -C tests test_sddrive`
Expected: FAIL

- [ ] **Step 2: Implement `drive.c`**: refresh→access token POST to `oauth2.googleapis.com` (vendored CA bundle, verify REQUIRED), session init, 8MB-chunk PUTs with progress log every chunk, 308-resume handling, secret scrubbing before every log call
- [ ] **Step 3: Framing tests green** + secret-absence test (fake refresh token string asserted absent from all captured log output)
- [ ] **Step 4: Commit**

```bash
git add src/drive.* tests/test_sddrive.c
git commit -m "feat: Drive resumable upload over tls"
```

### Task 5: Main payload, autorun, deploy

**Files:**
- Create: `src/main.c`, `src/Makefile`, `src/README.md` (user runbook)
- Modify: none on the PS4 — no `payloads.ini` exists in GoldHEN or Payload Guest; the payload is sent to GoldHEN's PayLoader (port 9090) after each jailbreak
- Test: console run, evidence = `/data/savedrive/run.log` + Drive folder listing

**Interfaces:**
- Consumes: `sd_zip_title`, `sd_manifest_skip`, `sd_drive_put`, `sd_cfg_load`
- Produces: `/data/payloads/savedrive.elf` staged; `run.log` lines `TITLE <id> <zip|skip|fail> <bytes>`

- [ ] **Step 1: Write `main.c`**: load config → refresh access token → walk both users (probe paths) → manifest check → zip → upload → log line per title → summary line; exit 0 always (never block boot on failure)
- [ ] **Step 2: Build, link-check**

Run: `make -C src`
Expected: clean, no `libkernel.so`, raw ELF produced

- [ ] **Step 3: Stage via FTP** to `/data/payloads/savedrive.elf`, verify remote size (staging only; the user sends `build/savedrive.elf` to port 9090 after each jailbreak)
- [ ] **Step 4: User runs one jailbreak cycle**, pastes `run.log`; verify first title zips appear in the Drive folder (user confirms visually), failures show `fail` not silent skip
- [ ] **Step 5: Fix pass on real failures only** (one round, each fix host-tested first), then commit

```bash
git add src/main.c src/Makefile src/README.md
git commit -m "feat: backup payload + autorun"
```

### Task 6: First full backup + steady state

**Files:** none (operations)

- [ ] **Step 1: User runs 1–2 more jailbreak cycles** until `run.log` shows all titles `zip` then `skip`
- [ ] **Step 2: Verify**: Drive folder holds one zip per `user+title` (`1898cd02+CUSA…` naming), count matches probe title count; download one zip on Mac, `unzip -l` lists save files
- [ ] **Step 3: Write the steady-state note** into `src/README.md` (each jailbreak = incremental backup; full re-upload if manifest deleted; rest-mode mid-run resumes next boot) and commit
- [ ] **Step 4: Delete staged build artifacts** from `/data/payloads` that aren't needed by your loader (`sdprobe.elf`)

## Self-review

1. Spec coverage: payload-on-PS4 ✓ (T1/T5), raw saves ✓ (T1/T3, no decrypt anywhere), Drive destination ✓ (T4), daemon cadence ✓ (autorun each jailbreak, T5 — no 24/7 resident, stated openly).
2. Step scan: each step names one artifact + command + expected output; fixed.
3. Type consistency: `sd_cfg_load`/`sd_zip_title`/`sd_manifest_skip`/`sd_drive_put` signatures identical between producer and consumer blocks; `run.log`/`probe.txt` formats defined once.
4. Review Focus: 5 lines above, each with a pinning test (secret-absence T4S3, partial-upload T4 design + T6S2, torn-read T3S2, 2.2GB first run T4S2 chunking + T6, user+title namespacing in header).
5. Proportion: plan ≈ spec size; no function bodies, only signatures and commands.
