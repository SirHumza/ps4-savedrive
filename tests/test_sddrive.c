/* test_sddrive.c - host framing tests for sd_drive_put_io().
 *
 * Rather than a Python fake server (the plan's ruling), this runs a tiny
 * HTTP/1.1 server in a pthread over an ephemeral loopback port. It records
 * every request line, header and body length so the protocol can be asserted
 * without any external dependency. Compiled with -DSD_DRIVE_CHUNK=64 so a
 * 200-byte fixture forces 4 resumable PUTs (resume exercised).
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
  int token_seen;
  int session_seen;
  int refresh_body_ok;
  int session_path_ok;
  int session_body_ok;
  int put_auth_ok;

  unsigned long put_start[MAXREQ];
  unsigned long put_end[MAXREQ];
  unsigned long put_total[MAXREQ];
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

static void *server_thread(void *arg) {
  srv_t *s = (srv_t *)arg;
  struct sockaddr_in ca;
  socklen_t cl = sizeof ca;
  int fd = accept(s->listen_fd, (struct sockaddr *)&ca, &cl);
  char hbuf[8192], body[8192];

  if (fd < 0)
    return NULL;

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
      s->refresh_body_ok = strstr(body, "grant_type=refresh_token") != NULL;
      if (s->mode == 1) {
        send_response(fd, 401, "Unauthorized", "", NULL);
      } else {
        send_response(fd, 200, "OK", "",
                      "{\"access_token\":\"test-token-123\",\"expires_in\":3600}");
      }
    } else if (strcmp(method, "POST") == 0 &&
               strstr(path, "uploadType=resumable") != NULL) {
      s->session_seen = 1;
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
      if (!(az && strncmp(az, "Bearer test-token-123", 21) == 0))
        s->put_auth_ok = 0;
      s->put_count++;

      if (en + 1 < tot) {
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
  int one = 1;

  memset(s, 0, sizeof *s);
  s->mode = mode;
  s->put_auth_ok = 1;

  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(s->listen_fd >= 0);
  assert(setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ==
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

static int run_scenario(int mode, const char *fixture, const char *drive_name,
                        srv_t *s) {
  struct sockaddr_in a;
  pthread_t th;
  cli_t cli;
  sd_io_t io;
  sd_cfg_t c;
  int rc;

  make_server(s, mode);
  assert(pthread_create(&th, NULL, server_thread, s) == 0);

  cli.fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(cli.fd >= 0);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(s->port);
  assert(connect(cli.fd, (struct sockaddr *)&a, sizeof a) == 0);

  io.ctx = &cli;
  io.write = cli_write;
  io.read = cli_read;

  memset(&c, 0, sizeof c);
  strcpy(c.client_id, "test-client-id");
  strcpy(c.client_secret, "test-client-secret");
  strcpy(c.refresh_token, "test-refresh-token-xyz");
  strcpy(c.folder_id, "test-folder-id");

  rc = sd_drive_put_io(&c, fixture, drive_name, &io);

  shutdown(cli.fd, SHUT_WR);
  close(cli.fd);
  assert(pthread_join(th, NULL) == 0);
  close(s->listen_fd);
  return rc;
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

  unlink(fixture);
  free(fixture);
  log_close();
  unlink(logpath);

  puts("PASS test_sddrive");
  return 0;
}
