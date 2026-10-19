/* test_sddrive.c - host framing tests for sd_drive_put_io().
 *
 * Rather than a Python fake server (the plan's ruling), this runs a tiny
 * HTTP/1.1 server in a pthread over an ephemeral loopback port. It records
 * every request line, header and body length so the protocol can be asserted
 * without any external dependency. Compiled with -DSD_DRIVE_CHUNK=64 so a
 * 200-byte fixture forces 4 resumable PUTs (resume exercised).
 *
 * Modes: 0 normal, 1 token 401, 2 session 503, 3 token 500, 4 partial chunk
 * receipt (server keeps only 32 bytes of the first 64), 5 PUT 403, 6 server
 * persists everything but never completes (status query path), 9 chunked
 * token response, 10 cross-boot resume (connection dropped mid-upload, state
 * file written, second call resumes), 11 expired session (query 404, new
 * session, upload restarts from zero without re-zipping).
 */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../src/drive.h"
#include "../src/log.h"

#define MAXREQ 32

typedef struct {
  char method[16];
  char path[256];
} req_line_t;

typedef struct {
  int listen_fd;
  int port;
  int mode; /* 0 normal, 1 token 401, 2 session 503 */

  int req_count;
  int put_count;
  int token_count;
  int session_count;
  int token_seen;
  int session_seen;
  int refresh_body_ok;
  int session_path_ok;
  int session_body_ok;
  int put_auth_ok;
  int finalize_seen;
  int dropped;
  unsigned long progress;

  unsigned long put_start[MAXREQ];
  unsigned long put_end[MAXREQ];
  unsigned long put_total[MAXREQ];
  unsigned char put_first[MAXREQ];
  int put_query[MAXREQ];
  req_line_t req[MAXREQ];
} srv_t;

static int send_all(int fd, const char *b, size_t n) {
  size_t o = 0;
  while (o < n) {
    ssize_t r = send(fd, b + o, n - o, 0);
    if (r <= 0)
      return -1;
    o += (size_t)r;
  }
  return 0;
}

static void send_response(int fd, int code, const char *reason,
                          const char *extra, const char *body) {
  char resp[2048];
  int bl = body ? (int)strlen(body) : 0;
  int n = snprintf(resp, sizeof resp,
                   "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n%s\r\n%s", code,
                   reason, bl, extra ? extra : "", body ? body : "");
  assert(n > 0 && (size_t)n < sizeof resp);
  assert(send_all(fd, resp, (size_t)n) == 0);
}

/* Reads until CRLFCRLF, storing the raw header block (NUL-terminated). */
static int read_headers(int fd, char *buf, size_t cap) {
  size_t n = 0;
  while (n < cap - 1) {
    char c;
    ssize_t r = recv(fd, &c, 1, 0);
    if (r <= 0)
      return -1;
    buf[n++] = c;
    if (n >= 4 && buf[n - 4] == '\r' && buf[n - 3] == '\n' &&
        buf[n - 2] == '\r' && buf[n - 1] == '\n') {
      buf[n] = 0;
      return (int)n;
    }
  }
  return -1;
}

static const char *find_hdr(const char *hdr, const char *name) {
  size_t nlen = strlen(name);
  const char *p = hdr;
  while ((p = strchr(p, '\n')) != NULL) {
    p++;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
      p += nlen + 1;
      while (*p == ' ' || *p == '\t')
        p++;
      return p;
    }
  }
  return NULL;
}

static int read_body(int fd, char *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, buf + got, n - got, 0);
    if (r <= 0)
      return -1;
    got += (size_t)r;
  }
  return 0;
}

/* A chunked 200 with the token JSON split into two chunks and the split
 * forced by a short pause, so the client must scan for the terminator across
 * multiple reads. */
static void send_chunked_token(int fd) {
  static const char json[] =
      "{\"access_token\":\"test-token-123\",\"expires_in\":3600}";
  char head[128];
  size_t n = sizeof json - 1, half = n / 2;
  int hn;

  hn = snprintf(head, sizeof head,
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%zx\r\n",
                half);
  assert(hn > 0 && (size_t)hn < sizeof head);
  assert(send_all(fd, head, (size_t)hn) == 0);
  assert(send_all(fd, json, half) == 0);
  assert(send_all(fd, "\r\n", 2) == 0);
  usleep(20000);
  hn = snprintf(head, sizeof head, "%zx\r\n", n - half);
  assert(hn > 0 && (size_t)hn < sizeof head);
  assert(send_all(fd, head, (size_t)hn) == 0);
  assert(send_all(fd, json + half, n - half) == 0);
  assert(send_all(fd, "\r\n0\r\n\r\n", 7) == 0);
}

static void *server_thread(void *arg) {
  srv_t *s = (srv_t *)arg;
  /* Modes 10/11 drop the connection mid-upload, so they need two accept
   * rounds: one per client connection (per "boot"). */
  int rounds = (s->mode == 10 || s->mode == 11) ? 2 : 1;
  int round;

  for (round = 0; round < rounds; round++) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof ca;
    struct timeval tv = {15, 0};
    int fd = accept(s->listen_fd, (struct sockaddr *)&ca, &cl);
    char hbuf[8192], body[8192];

    if (fd < 0)
      break;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);

    for (;;) {
      int hn = read_headers(fd, hbuf, sizeof hbuf);
      char method[16] = {0}, path[1024] = {0};
      const char *clh;
      long blen = 0;
      int idx;

      if (hn < 0)
        break;
      assert(sscanf(hbuf, "%15s %1023s", method, path) == 2);

      clh = find_hdr(hbuf, "Content-Length");
      if (clh)
        blen = strtol(clh, NULL, 10);
      if (blen < 0 || (size_t)blen >= sizeof body)
        break;
      if (blen > 0 && read_body(fd, body, (size_t)blen) < 0)
        break;
      body[blen] = 0;

      idx = s->req_count < MAXREQ ? s->req_count : MAXREQ - 1;
      strncpy(s->req[idx].method, method, sizeof s->req[idx].method - 1);
      strncpy(s->req[idx].path, path, sizeof s->req[idx].path - 1);
      s->req_count++;

      if (strcmp(method, "POST") == 0 && strcmp(path, "/token") == 0) {
        s->token_seen = 1;
        s->token_count++;
        s->refresh_body_ok = strstr(body, "grant_type=refresh_token") != NULL;
        if (s->mode == 1) {
          send_response(fd, 401, "Unauthorized", "", NULL);
        } else if (s->mode == 3) {
          send_response(fd, 500, "Internal Server Error", "", NULL);
        } else if (s->mode == 9) {
          send_chunked_token(fd);
        } else {
          send_response(fd, 200, "OK", "",
                        "{\"access_token\":\"test-token-123\",\"expires_in\":3600}");
        }
      } else if (strcmp(method, "POST") == 0 &&
                 strstr(path, "uploadType=resumable") != NULL) {
        s->session_seen = 1;
        s->session_count++;
        s->progress = 0; /* a new session holds no bytes yet */
        s->session_path_ok = 1;
        s->session_body_ok = strstr(body, "test-drive-name") != NULL &&
                             strstr(body, "test-folder-id") != NULL;
        if (s->mode == 2) {
          send_response(fd, 503, "Service Unavailable", "", NULL);
        } else {
          char extra[256];
          snprintf(extra, sizeof extra,
                   "Location: http://127.0.0.1:%d/upload/session/abc\r\n",
                   s->port);
          send_response(fd, 200, "OK", extra, NULL);
        }
      } else if (strcmp(method, "PUT") == 0) {
        const char *cr = find_hdr(hbuf, "Content-Range");
        const char *az = find_hdr(hbuf, "Authorization");
        unsigned long st = 0, en = 0, tot = 0;
        int i = s->put_count < MAXREQ ? s->put_count : MAXREQ - 1;

        if (cr)
          sscanf(cr, "bytes %lu-%lu/%lu", &st, &en, &tot);
        s->put_start[i] = st;
        s->put_end[i] = en;
        s->put_total[i] = tot;
        s->put_first[i] = blen > 0 ? (unsigned char)body[0] : 0;
        if (!(az && strncmp(az, "Bearer test-token-123", 21) == 0))
          s->put_auth_ok = 0;
        s->put_count++;

        if ((s->mode == 10 || s->mode == 11) && s->put_count == 2) {
          /* Simulate the network dying mid-upload: read the request, then
           * close without persisting or answering. */
          s->dropped = 1;
          break;
        } else if (cr && strstr(cr, "*/")) {
          s->put_query[i] = 1;
          if (s->mode == 10) {
            /* The session survived and still holds the persisted bytes. */
            char extra[64];
            if (s->progress > 0) {
              snprintf(extra, sizeof extra, "Range: bytes=0-%lu\r\n",
                       s->progress - 1);
              send_response(fd, 308, "Resume Incomplete", extra, NULL);
            } else {
              send_response(fd, 308, "Resume Incomplete", "", NULL);
            }
          } else if (s->mode == 11) {
            /* The session expired: Drive answers 404. */
            send_response(fd, 404, "Not Found", "", NULL);
          } else {
            /* Session-status query: report the upload complete. */
            s->finalize_seen = 1;
            send_response(fd, 200, "OK", "", "{\"id\":\"file-id\"}");
          }
        } else if (s->mode == 5) {
          send_response(fd, 403, "Forbidden", "", NULL);
        } else if (s->mode == 4 && s->put_count == 1) {
          /* Persist only half of the first chunk. */
          send_response(fd, 308, "Resume Incomplete", "Range: bytes=0-31\r\n",
                        NULL);
        } else if (s->mode == 10 || s->mode == 11) {
          char extra[64];
          if (en + 1 > s->progress)
            s->progress = en + 1;
          if (s->progress >= tot) {
            send_response(fd, 201, "Created", "", "{\"id\":\"file-id\"}");
          } else {
            snprintf(extra, sizeof extra, "Range: bytes=0-%lu\r\n",
                     s->progress - 1);
            send_response(fd, 308, "Resume Incomplete", extra, NULL);
          }
        } else if (s->mode == 6 && en + 1 >= tot) {
          /* Every byte persisted, but never the completion status. */
          char extra[64];
          snprintf(extra, sizeof extra, "Range: bytes=0-%lu\r\n", en);
          send_response(fd, 308, "Resume Incomplete", extra, NULL);
        } else if (en + 1 < tot) {
          char extra[64];
          snprintf(extra, sizeof extra, "Range: bytes=0-%lu\r\n", en);
          send_response(fd, 308, "Resume Incomplete", extra, NULL);
        } else {
          send_response(fd, 201, "Created", "", "{\"id\":\"file-id\"}");
        }
      } else {
        send_response(fd, 404, "Not Found", "", NULL);
      }
    }

    close(fd);
  }

  return NULL;
}

typedef struct {
  int fd;
} cli_t;

static int cli_write(void *ctx, const void *b, size_t n) {
  cli_t *c = (cli_t *)ctx;
  size_t o = 0;
  while (o < n) {
    ssize_t r = send(c->fd, (const char *)b + o, n - o, 0);
    if (r <= 0)
      return -1;
    o += (size_t)r;
  }
  return (int)n;
}

static int cli_read(void *ctx, void *b, size_t cap) {
  cli_t *c = (cli_t *)ctx;
  ssize_t r = recv(c->fd, b, cap, 0);
  return (int)r;
}

static void make_server(srv_t *s, int mode) {
  struct sockaddr_in a;
  socklen_t al = sizeof a;
  struct timeval tv = {15, 0};
  int one = 1;

  memset(s, 0, sizeof *s);
  s->mode = mode;
  s->put_auth_ok = 1;

  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(s->listen_fd >= 0);
  assert(setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ==
         0);
  /* So a failed test cannot hang the server thread in accept() forever. */
  assert(setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) ==
         0);

  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  assert(bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) == 0);
  assert(listen(s->listen_fd, 1) == 0);
  assert(getsockname(s->listen_fd, (struct sockaddr *)&a, &al) == 0);
  s->port = ntohs(a.sin_port);
}

static void client_open(srv_t *s, cli_t *cli) {
  struct sockaddr_in a;
  cli->fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(cli->fd >= 0);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(s->port);
  assert(connect(cli->fd, (struct sockaddr *)&a, sizeof a) == 0);
}

static void client_close(cli_t *cli) {
  shutdown(cli->fd, SHUT_WR);
  close(cli->fd);
}

static void test_cfg(sd_cfg_t *c) {
  memset(c, 0, sizeof *c);
  strcpy(c->client_id, "test-client-id");
  strcpy(c->client_secret, "test-client-secret");
  strcpy(c->refresh_token, "test-refresh-token-xyz");
  strcpy(c->folder_id, "test-folder-id");
}

static int run_scenario(int mode, const char *fixture, const char *drive_name,
                        srv_t *s) {
  pthread_t th;
  cli_t cli;
  sd_io_t io;
  sd_cfg_t c;
  int rc;

  make_server(s, mode);
  assert(pthread_create(&th, NULL, server_thread, s) == 0);

  client_open(s, &cli);
  io.ctx = &cli;
  io.write = cli_write;
  io.read = cli_read;
  test_cfg(&c);

  rc = sd_drive_put_io(&c, fixture, drive_name, &io);

  client_close(&cli);
  assert(pthread_join(th, NULL) == 0);
  close(s->listen_fd);
  return rc;
}

/* One sd_drive_put_io_state call against an already-running server. */
static int run_call(srv_t *s, const char *state_path, const char *fixture,
                    const char *drive_name, unsigned long long sig_bytes,
                    long sig_mtime) {
  cli_t cli;
  sd_io_t io;
  sd_cfg_t c;
  int rc;

  client_open(s, &cli);
  io.ctx = &cli;
  io.write = cli_write;
  io.read = cli_read;
  test_cfg(&c);

  rc = sd_drive_put_io_state(&c, fixture, drive_name, &io, state_path,
                             sig_bytes, sig_mtime);

  client_close(&cli);
  return rc;
}

static void write_state(const char *path, const char *content) {
  FILE *f = fopen(path, "wb");
  size_t n = strlen(content);
  assert(f);
  assert(fwrite(content, 1, n, f) == n);
  fclose(f);
}

static char *make_fixture(size_t n) {
  char *path = strdup("/tmp/sddriveXXXXXX");
  int fd = mkstemp(path);
  size_t i;
  assert(fd >= 0);
  for (i = 0; i < n; i++) {
    char b = (char)('A' + (i % 26));
    assert(write(fd, &b, 1) == 1);
  }
  close(fd);
  return path;
}

int main(void) {
  char logpath[] = "/tmp/sddrive_logXXXXXX";
  char *fixture;
  srv_t s;
  int rc, i;
  FILE *lf;
  char lb[65536];
  size_t ln;

  {
    int lfd = mkstemp(logpath);
    assert(lfd >= 0);
    close(lfd);
  }
  log_init(logpath);

  fixture = make_fixture(200);

  /* --- normal path ----------------------------------------------------- */
  rc = run_scenario(0, fixture, "test-drive-name.zip", &s);
  assert(rc == 0);
  assert(s.token_seen && s.refresh_body_ok);
  assert(s.session_seen && s.session_path_ok && s.session_body_ok);
  assert(s.put_count >= 2);
  assert(s.put_auth_ok);
  assert(s.req_count == s.put_count + 2);
  assert(strcmp(s.req[0].method, "POST") == 0 &&
         strcmp(s.req[0].path, "/token") == 0);
  assert(strcmp(s.req[1].method, "POST") == 0 &&
         strstr(s.req[1].path, "uploadType=resumable") != NULL);
  for (i = 0; i < s.put_count; i++) {
    unsigned long exp_start = (unsigned long)i * 64UL;
    unsigned long exp_end = exp_start + 63UL;
    assert(strcmp(s.req[i + 2].method, "PUT") == 0);
    assert(s.put_total[i] == 200);
    assert(s.put_start[i] == exp_start);
    assert(s.put_end[i] == (exp_end < 199UL ? exp_end : 199UL));
    assert(s.put_first[i] == (unsigned char)('A' + (exp_start % 26)));
  }
  assert(s.put_end[s.put_count - 1] == 199);

  /* --- log must exist and must not carry secrets ----------------------- */
  lf = fopen(logpath, "rb");
  assert(lf);
  ln = fread(lb, 1, sizeof lb - 1, lf);
  lb[ln] = 0;
  fclose(lf);
  assert(ln > 0);
  assert(strchr(lb, '\n') != NULL);
  assert(strstr(lb, "test-token-123") == NULL);
  assert(strstr(lb, "test-client-secret") == NULL);
  assert(strstr(lb, "test-refresh-token-xyz") == NULL);

  /* --- token 401 is fatal ---------------------------------------------- */
  rc = run_scenario(1, fixture, "test-drive-name.zip", &s);
  assert(rc == -1);
  assert(s.token_seen && s.put_count == 0);

  /* --- session 503 is retryable ---------------------------------------- */
  rc = run_scenario(2, fixture, "test-drive-name.zip", &s);
  assert(rc == -2);
  assert(s.token_seen && s.session_seen && s.put_count == 0);

  /* --- token 500 is retryable ------------------------------------------ */
  rc = run_scenario(3, fixture, "test-drive-name.zip", &s);
  assert(rc == -2);
  assert(s.token_seen && s.put_count == 0);

  /* --- partial chunk receipt: resend from the server's position -------- */
  rc = run_scenario(4, fixture, "test-drive-name.zip", &s);
  assert(rc == 0);
  assert(s.put_count == 4);
  assert(s.put_start[0] == 0 && s.put_end[0] == 63);
  assert(s.put_start[1] == 32 && s.put_end[1] == 95);
  assert(s.put_start[2] == 96 && s.put_end[2] == 159);
  assert(s.put_start[3] == 160 && s.put_end[3] == 199);
  for (i = 0; i < s.put_count; i++) {
    unsigned long st = s.put_start[i];
    assert(s.put_total[i] == 200);
    assert(s.put_first[i] == (unsigned char)('A' + (st % 26)));
  }

  /* --- PUT 403 is fatal ------------------------------------------------ */
  rc = run_scenario(5, fixture, "test-drive-name.zip", &s);
  assert(rc == -1);
  assert(s.put_count == 1);

  /* --- 308 covering the last byte: status query completes the upload --- */
  rc = run_scenario(6, fixture, "test-drive-name.zip", &s);
  assert(rc == 0);
  assert(s.finalize_seen);
  assert(s.put_count == 5);

  /* --- chunked token response ------------------------------------------ */
  rc = run_scenario(9, fixture, "test-drive-name.zip", &s);
  assert(rc == 0);
  assert(s.token_count == 1 && s.put_count == 4);

  /* --- state file parsing ---------------------------------------------- */
  {
    char stpath[] = "/tmp/sdstateXXXXXX";
    int sfd = mkstemp(stpath);
    assert(sfd >= 0);
    close(sfd);

    write_state(stpath, "200 1700000000 200\n/upload/session/abc\nname.zip\n");
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 1);
    assert(sd_drive_state_resumable(stpath, fixture, 201, 1700000000) == 0);
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000001) == 0);
    assert(sd_drive_state_resumable(stpath, "/nonexistent/zip", 200,
                                    1700000000) == 0);
    assert(sd_drive_state_resumable(NULL, fixture, 200, 1700000000) == 0);

    write_state(stpath, "200 1700000000 999\n/upload/session/abc\nname.zip\n");
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 0);

    write_state(stpath, "junk\n/upload/session/abc\nname.zip\n");
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 0);

    write_state(stpath, "200 1700000000 200\nrelative/path\nname.zip\n");
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 0);

    write_state(stpath, "200 1700000000 200\n/upload/session\ntest\x01" "x\n");
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 0);

    unlink(stpath);
  }

  /* --- cross-boot resume: the second call continues the first ---------- */
  {
    char stpath[] = "/tmp/sdstateXXXXXX";
    char sbuf[256];
    pthread_t th;
    FILE *sf;
    size_t sn;
    int sfd = mkstemp(stpath);
    assert(sfd >= 0);
    close(sfd);

    make_server(&s, 10);
    assert(pthread_create(&th, NULL, server_thread, &s) == 0);

    /* First boot: the network dies on the second chunk. */
    rc = run_call(&s, stpath, fixture, "test-drive-name.zip", 200,
                  1700000000);
    assert(rc == -2);
    assert(s.token_count == 1 && s.session_count == 1);
    assert(s.put_count == 2 && s.dropped == 1 && s.progress == 64);

    sf = fopen(stpath, "rb");
    assert(sf);
    sn = fread(sbuf, 1, sizeof sbuf - 1, sf);
    sbuf[sn] = 0;
    fclose(sf);
    assert(strncmp(sbuf, "200 1700000000 200\n", 19) == 0);
    assert(strstr(sbuf, "/upload/session/abc") != NULL);
    assert(strstr(sbuf, "test-drive-name.zip") != NULL);
    assert(sd_drive_state_resumable(stpath, fixture, 200, 1700000000) == 1);

    /* Next boot: same zip and signature, so it resumes the session. */
    rc = run_call(&s, stpath, fixture, "test-drive-name.zip", 200,
                  1700000000);
    assert(rc == 0);
    assert(s.token_count == 2 && s.session_count == 1);
    assert(s.put_count == 6);
    assert(s.put_query[2] == 1);
    assert(s.put_start[3] == 64 && s.put_end[5] == 199);
    assert(s.put_first[3] == (unsigned char)('A' + (64 % 26)));
    assert(access(stpath, F_OK) != 0);

    assert(pthread_join(th, NULL) == 0);
    close(s.listen_fd);
    unlink(stpath);
  }

  /* --- expired session: query 404, new session, upload from zero ------- */
  {
    char stpath[] = "/tmp/sdstateXXXXXX";
    pthread_t th;
    int sfd = mkstemp(stpath);
    assert(sfd >= 0);
    close(sfd);

    make_server(&s, 11);
    assert(pthread_create(&th, NULL, server_thread, &s) == 0);

    rc = run_call(&s, stpath, fixture, "test-drive-name.zip", 200,
                  1700000000);
    assert(rc == -2);
    assert(s.dropped == 1 && s.progress == 64 && s.session_count == 1);

    rc = run_call(&s, stpath, fixture, "test-drive-name.zip", 200,
                  1700000000);
    assert(rc == 0);
    assert(s.token_count == 2 && s.session_count == 2);
    assert(s.put_count == 7);
    assert(s.put_query[2] == 1);
    assert(s.put_start[3] == 0 && s.put_first[3] == (unsigned char)'A');
    assert(s.put_end[6] == 199);
    assert(access(stpath, F_OK) != 0);

    assert(pthread_join(th, NULL) == 0);
    close(s.listen_fd);
    unlink(stpath);
  }

  unlink(fixture);
  free(fixture);
  log_close();
  unlink(logpath);

  puts("PASS test_sddrive");
  return 0;
}
