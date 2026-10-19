/* drive.c - uploads a zip to Google Drive over a resumable session.
 *
 * Two responsibilities, deliberately split by the transport seam:
 *   sd_drive_put_io() - the whole HTTP/1.1 protocol over an injected sd_io_t.
 *                       This is what host tests exercise byte for byte.
 *   sd_drive_put()    - the PS4 entry: resolves + connects to the two Google
 *                       hosts over TLS and reuses sd_drive_put_io via an
 *                       sd_io_t that switches connection when the request's
 *                       Host header changes.
 *
 * Return codes: 0 uploaded, -1 fatal (config/auth/4xx), -2 retryable
 * (network/5xx/timeout). Secrets (client_secret, refresh_token,
 * access_token, Authorization) are never logged.
 */
#include "drive.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "jsonlite.h"
#include "log.h"

#ifdef SD_PS4
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <netdb.h>

#include "clock.h"
#include "tls.h"
#endif /* SD_PS4 */

/* Chunk size: 8 MiB in production; tests compile -DSD_DRIVE_CHUNK=64 so a
 * few hundred bytes force several resumable PUTs. */
#ifndef SD_DRIVE_CHUNK
#define SD_DRIVE_CHUNK (8u << 20)
#endif

#define SD_TOKEN_HOST "oauth2.googleapis.com"
#define SD_GOOGLE_HOST "www.googleapis.com"
#define SD_HTTPS_PORT 443

#define SD_TOKEN_MAX 4096
#define SD_RESP_MAX (64u * 1024u)
#define SD_ENC_MAX 2048
#define SD_BODY_MAX 8192
#define SD_LOC_MAX 4096

/* ---------------------------------------------------------------- helpers */

static int io_write_all(sd_io_t *io, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len) {
    int n = io->write(io->ctx, p, len);
    if (n <= 0)
      return -2;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int parse_status(const char *buf, size_t len) {
  size_t i = 0;
  if (len < 12 || strncmp(buf, "HTTP/", 5) != 0)
    return -1;
  while (i < len && buf[i] != ' ')
    i++;
  if (i >= len)
    return -1;
  return atoi(buf + i + 1);
}

/* Case-insensitive header lookup over a header block. `len` is the header
 * length (status line + headers), body excluded. Returns pointer to the
 * value within `buf`, or NULL. */
static const char *header_find(const char *buf, size_t len, const char *name,
                               size_t *vlen) {
  size_t nlen = strlen(name);
  size_t i = 0;
  while (i + 1 < len && !(buf[i] == '\r' && buf[i + 1] == '\n'))
    i++;
  i += 2;
  while (i < len) {
    size_t line_end = i;
    while (line_end + 1 < len &&
           !(buf[line_end] == '\r' && buf[line_end + 1] == '\n'))
      line_end++;
    if (line_end == i)
      break; /* blank line: end of headers */
    if (line_end - i > nlen && strncasecmp(buf + i, name, nlen) == 0 &&
        buf[i + nlen] == ':') {
      size_t v = i + nlen + 1;
      while (v < line_end && (buf[v] == ' ' || buf[v] == '\t'))
        v++;
      if (vlen)
        *vlen = line_end - v;
      return buf + v;
    }
    i = line_end + 2;
  }
  return NULL;
}

static long parse_content_length(const char *buf, size_t hdr_end) {
  size_t vlen = 0;
  const char *v = header_find(buf, hdr_end, "Content-Length", &vlen);
  long n = 0;
  size_t i = 0;
  if (!v)
    return -1;
  while (i < vlen && v[i] >= '0' && v[i] <= '9') {
    n = n * 10 + (v[i] - '0');
    i++;
  }
  return i ? n : -1;
}

/* ------------------------------------------------------------ HTTP exchange */

typedef struct {
  int status;
  const char *body; /* points into the caller's buffer */
  size_t body_len;
} sd_http_t;

/* Writes one request (headers, then optional binary body) and reads the full
 * response. Handles responses split across any number of io->read calls:
 * headers are accumulated until CRLFCRLF, Content-Length parsed, then exactly
 * that many body bytes are read. Returns 0 on a complete exchange, -2 on any
 * transport error. */
static int sd_http_roundtrip(sd_io_t *io, const char *hdr, size_t hdr_len,
                             const void *body, size_t body_len, char *buf,
                             size_t cap, sd_http_t *out) {
  size_t len = 0, hdr_end = 0, scan = 0;
  long clen = -1;
  int have_hdr = 0;

  if (io_write_all(io, hdr, hdr_len) != 0)
    return -2;
  if (body_len && io_write_all(io, body, body_len) != 0)
    return -2;

  for (;;) {
    if (have_hdr) {
      if (clen < 0 || len >= hdr_end + (size_t)clen)
        break;
    }
    if (len >= cap)
      return -2; /* response larger than we will buffer */
    {
      int n = io->read(io->ctx, buf + len, cap - len);
      if (n < 0)
        return -2;
      if (n == 0) {
        if (!have_hdr)
          return -2; /* EOF mid-headers */
        break;
      }
      len += (size_t)n;
    }
    if (!have_hdr) {
      size_t i;
      for (i = scan; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
          hdr_end = i + 4;
          have_hdr = 1;
          break;
        }
      }
      scan = len >= 3 ? len - 3 : 0;
      if (have_hdr)
        clen = parse_content_length(buf, hdr_end);
    }
  }

  out->status = parse_status(buf, have_hdr ? hdr_end : len);
  out->body = buf + hdr_end;
  out->body_len = len > hdr_end ? len - hdr_end : 0;
  if (clen >= 0 && (size_t)clen < out->body_len)
    out->body_len = (size_t)clen;
  return 0;
}

static int url_encode(const char *src, char *dst, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *src; src++) {
    unsigned char c = (unsigned char)*src;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (o + 1 >= cap)
        return -1;
      dst[o++] = (char)c;
    } else {
      if (o + 3 >= cap)
        return -1;
      dst[o++] = '%';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0xf];
    }
  }
  dst[o] = 0;
  return 0;
}

/* --------------------------------------------------------------- protocol */

static int get_access_token(const sd_cfg_t *c, sd_io_t *io, char *token,
                            size_t tok_cap, char *rbuf, size_t rcap) {
  char enc[SD_ENC_MAX];
  char body[SD_BODY_MAX];
  char hdr[512];
  sd_http_t r;
  jl_val_t *root, *tok;
  int blen, hlen, rc;

  if (url_encode(c->client_id, enc, sizeof enc) != 0) {
    log_msg("drive: client_id too long");
    return -1;
  }
  blen = snprintf(body, sizeof body, "grant_type=refresh_token&client_id=%s", enc);
  if (blen <= 0 || (size_t)blen >= sizeof body) {
    log_msg("drive: token body overflow");
    return -1;
  }
  if (url_encode(c->client_secret, enc, sizeof enc) != 0) {
    log_msg("drive: client_secret too long");
    return -1;
  }
  blen += snprintf(body + blen, sizeof body - (size_t)blen, "&client_secret=%s", enc);
  if ((size_t)blen >= sizeof body) {
    log_msg("drive: token body overflow");
    return -1;
  }
  if (url_encode(c->refresh_token, enc, sizeof enc) != 0) {
    log_msg("drive: refresh_token too long");
    return -1;
  }
  blen += snprintf(body + blen, sizeof body - (size_t)blen, "&refresh_token=%s", enc);
  if ((size_t)blen >= sizeof body) {
    log_msg("drive: token body overflow");
    return -1;
  }

  hlen = snprintf(hdr, sizeof hdr,
                  "POST /token HTTP/1.1\r\nHost: %s\r\n"
                  "Content-Type: application/x-www-form-urlencoded\r\n"
                  "Content-Length: %d\r\n\r\n",
                  SD_TOKEN_HOST, blen);
  if (hlen <= 0 || (size_t)hlen >= sizeof hdr) {
    log_msg("drive: token header overflow");
    return -1;
  }

  rc = sd_http_roundtrip(io, hdr, (size_t)hlen, body, (size_t)blen, rbuf, rcap,
                         &r);
  if (rc != 0) {
    log_msg("drive: token network error");
    return -2;
  }
  if (r.status != 200) {
    log_msg("drive: token http %d", r.status);
    return -1;
  }

  root = jl_parse(r.body, r.body_len);
  tok = root ? jl_obj_get(root, "access_token") : NULL;
  if (!tok || tok->type != JL_STRING || !tok->str || tok->strlen == 0 ||
      tok->strlen >= tok_cap) {
    log_msg("drive: token missing");
    jl_free(root);
    return -1;
  }
  memcpy(token, tok->str, tok->strlen);
  token[tok->strlen] = 0;
  jl_free(root);
  log_msg("drive: access token ok");
  return 0;
}

/* Strips "scheme://host" from a Location value, leaving path+query. */
static const char *strip_scheme_host(const char *loc) {
  const char *p = strstr(loc, "://");
  const char *slash;
  if (!p)
    return loc;
  p += 3;
  slash = strchr(p, '/');
  return slash ? slash : "/";
}

static int create_session(const sd_cfg_t *c, const char *token,
                          const char *drive_name, sd_io_t *io, char *rbuf,
                          size_t rcap, char *loc, size_t loc_cap) {
  jl_val_t *root = jl_new_object();
  jl_val_t *arr = jl_new_array();
  char *json, *hdr;
  size_t jlen, need, hlen, vlen = 0;
  const char *v;
  sd_http_t r;
  int rc;

  jl_obj_set(root, "name", jl_new_string(drive_name));
  jl_arr_push(arr, jl_new_string(c->folder_id));
  jl_obj_set(root, "parents", arr);
  json = jl_stringify(root);
  jl_free(root);
  if (!json) {
    log_msg("drive: session json fail");
    return -1;
  }
  jlen = strlen(json);

  need = jlen + strlen(token) + 256;
  hdr = malloc(need);
  if (!hdr) {
    free(json);
    log_msg("drive: oom");
    return -1;
  }
  hlen = (size_t)snprintf(hdr, need,
                          "POST /upload/drive/v3/files?uploadType=resumable "
                          "HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n\r\n",
                          SD_GOOGLE_HOST, token, jlen);
  rc = sd_http_roundtrip(io, hdr, hlen, json, jlen, rbuf, rcap, &r);
  free(hdr);
  free(json);
  if (rc != 0) {
    log_msg("drive: session network error");
    return -2;
  }
  if (r.status == 401 || r.status == 403) {
    log_msg("drive: session http %d", r.status);
    return -1;
  }
  if (r.status >= 500) {
    log_msg("drive: session http %d", r.status);
    return -2;
  }
  if (r.status != 200 && r.status != 201) {
    log_msg("drive: session http %d", r.status);
    return -1;
  }

  v = header_find(rbuf, (size_t)(r.body - rbuf), "Location", &vlen);
  if (!v || vlen == 0 || vlen >= loc_cap) {
    log_msg("drive: session missing Location");
    return -1;
  }
  memcpy(loc, v, vlen);
  loc[vlen] = 0;
  log_msg("drive: session created");
  return 0;
}

/* One resumable PUT. Returns 0 on a complete exchange (status in *status_out),
 * -2 on transport error. *range_end is updated from a 308 Range header. */
static int put_chunk(sd_io_t *io, const char *path, const char *token,
                     const unsigned char *data, size_t len, const char *range,
                     char *rbuf, size_t rcap, int *status_out,
                     unsigned long *range_end) {
  size_t need = strlen(path) + strlen(token) + 256;
  char *hdr = malloc(need);
  sd_http_t r;
  int rc;
  size_t hlen;

  if (!hdr)
    return -2;
  hlen = (size_t)snprintf(hdr, need,
                          "PUT %s HTTP/1.1\r\nHost: %s\r\n"
                          "Authorization: Bearer %s\r\nContent-Length: %zu\r\n"
                          "Content-Range: %s\r\n\r\n",
                          path, SD_GOOGLE_HOST, token, len, range);
  rc = sd_http_roundtrip(io, hdr, hlen, data, len, rbuf, rcap, &r);
  free(hdr);
  if (rc != 0)
    return -2;
  *status_out = r.status;

  if (range_end && r.status == 308) {
    size_t vlen = 0;
    const char *v = header_find(rbuf, (size_t)(r.body - rbuf), "Range", &vlen);
    if (v && vlen) {
      size_t i = 0;
      unsigned long last = 0;
      int any = 0;
      while (i < vlen && v[i] != '=')
        i++;
      if (i < vlen) {
        i++;
        while (i < vlen && v[i] != '-')
          i++;
        if (i < vlen && v[i] == '-') {
          i++;
          while (i < vlen && v[i] >= '0' && v[i] <= '9') {
            last = last * 10 + (unsigned long)(v[i] - '0');
            i++;
            any = 1;
          }
        }
      }
      if (any)
        *range_end = last;
    }
  }
  return 0;
}

int sd_drive_put_io(const sd_cfg_t *c, const char *zip_path,
                    const char *drive_name, sd_io_t *io) {
  char token[SD_TOKEN_MAX];
  char loc[SD_LOC_MAX];
  char range[64];
  char *rbuf;
  unsigned char *chunk;
  const char *path;
  FILE *f;
  long fsz;
  unsigned long total, start;
  int rc, chunks;

  if (!c || !zip_path || !drive_name || !io || !io->write || !io->read)
    return -1;

  rbuf = malloc(SD_RESP_MAX);
  if (!rbuf) {
    log_msg("drive: oom");
    return -1;
  }

  log_msg("drive: start upload");

  rc = get_access_token(c, io, token, sizeof token, rbuf, SD_RESP_MAX);
  if (rc != 0) {
    free(rbuf);
    return rc;
  }

  rc = create_session(c, token, drive_name, io, rbuf, SD_RESP_MAX, loc,
                      sizeof loc);
  if (rc != 0) {
    free(rbuf);
    return rc;
  }
  path = strip_scheme_host(loc);

  f = fopen(zip_path, "rb");
  if (!f) {
    log_msg("drive: open failed");
    free(rbuf);
    return -1;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (fsz = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    log_msg("drive: stat failed");
    fclose(f);
    free(rbuf);
    return -1;
  }
  total = (unsigned long)fsz;

  chunk = malloc(SD_DRIVE_CHUNK);
  if (!chunk) {
    log_msg("drive: oom");
    fclose(f);
    free(rbuf);
    return -1;
  }

  rc = 0;
  chunks = 0;

  if (total == 0) {
    int status = 0;
    rc = put_chunk(io, path, token, (const unsigned char *)"", 0,
                   "bytes */0", rbuf, SD_RESP_MAX, &status, NULL);
    if (rc != 0)
      rc = -2;
    else if (status >= 400 && status < 500)
      rc = -1;
    else if (status == 200 || status == 201)
      rc = 0;
    else
      rc = -2;
    chunks = 1;
  } else {
    start = 0;
    while (start < total) {
      size_t want = (size_t)(total - start);
      size_t got;
      unsigned long end, range_end;
      int status = 0;

      if (want > (size_t)SD_DRIVE_CHUNK)
        want = (size_t)SD_DRIVE_CHUNK;
      got = fread(chunk, 1, want, f);
      if (got != want) {
        log_msg("drive: read failed");
        rc = -2;
        break;
      }
      end = start + (unsigned long)got - 1;
      snprintf(range, sizeof range, "bytes %lu-%lu/%lu", start, end, total);

      range_end = end;
      rc = put_chunk(io, path, token, chunk, got, range, rbuf, SD_RESP_MAX,
                     &status, &range_end);
      if (rc != 0) {
        log_msg("drive: put network error");
        rc = -2;
        break;
      }
      chunks++;
      log_msg("drive: chunk %d status %d sent %lu/%lu", chunks, status, end + 1,
              total);

      if (status == 308) {
        unsigned long next = end + 1;
        if (range_end >= start && range_end + 1 > next)
          next = range_end + 1;
        start = next;
        if (fseek(f, (long)start, SEEK_SET) != 0) {
          log_msg("drive: reseek failed");
          rc = -2;
          break;
        }
        continue;
      }
      if (status == 200 || status == 201) {
        rc = 0;
        break;
      }
      if (status >= 400 && status < 500) {
        log_msg("drive: fatal http %d", status);
        rc = -1;
        break;
      }
      log_msg("drive: retryable http %d", status);
      rc = -2;
      break;
    }
  }

  fclose(f);
  free(chunk);
  free(rbuf);
  log_msg("drive: done rc=%d chunks=%d", rc, chunks);
  return rc;
}

/* ------------------------------------------------------------- PS4 entry */

#ifdef SD_PS4

/* Resolves host and connects a TCP socket, returning the fd or -1.
 * The payload SDK's libc provides BSD sockets (same route orbisRPC's
 * SDK build uses); no orbis/Net.h and no app-world linkage. */
static int sd_tcp_connect(const char *host) {
  struct addrinfo hints, *res = NULL;
  char portbuf[16];
  int fd;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portbuf, sizeof portbuf, "%d", SD_HTTPS_PORT);
  if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
    log_msg("drive: resolve fail %s", host);
    return -1;
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    log_msg("drive: socket fail");
    freeaddrinfo(res);
    return -1;
  }
  {
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  }
  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    log_msg("drive: connect fail %s", host);
    close(fd);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
      fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  return fd;
}

struct ps4_io {
  char host[64];
  int fd;
  tls_ctx_t *tls;
};

static void ps4_io_close(struct ps4_io *p) {
  if (p->tls) {
    tls_free(p->tls);
    p->tls = NULL;
  }
  if (p->fd >= 0) {
    close(p->fd);
    p->fd = -1;
  }
  p->host[0] = 0;
}

/* Host: <value> out of a just-built request. */
static const char *req_host(const char *req, size_t len, char *out,
                            size_t cap) {
  size_t i;
  for (i = 0; i + 6 <= len; i++) {
    if ((req[i] == 'H' || req[i] == 'h') && strncasecmp(req + i, "Host:", 5) == 0) {
      size_t j = i + 5, o = 0;
      while (j < len && (req[j] == ' ' || req[j] == '\t'))
        j++;
      while (j < len && req[j] != '\r' && req[j] != '\n' && o + 1 < cap)
        out[o++] = req[j++];
      out[o] = 0;
      return out;
    }
  }
  return NULL;
}

static int ps4_io_write(void *ctx, const void *buf, size_t len) {
  struct ps4_io *p = (struct ps4_io *)ctx;
  char host[64];
  if (!req_host((const char *)buf, len, host, sizeof host)) {
    if (!p->host[0])
      return -1;
    strcpy(host, p->host);
  }
  if (strcmp(host, p->host) != 0) {
    int fd;
    ps4_io_close(p);
    fd = sd_tcp_connect(host);
    if (fd < 0)
      return -1;
    p->fd = fd;
    p->tls = tls_start(fd, host);
    if (!p->tls) {
      log_msg("drive: tls fail %s", host);
      close(fd);
      p->fd = -1;
      return -1;
    }
    strncpy(p->host, host, sizeof p->host - 1);
    log_msg("drive: connected %s", host);
  }
  return tls_write(p->tls, buf, len);
}

static int ps4_io_read(void *ctx, void *buf, size_t cap) {
  struct ps4_io *p = (struct ps4_io *)ctx;
  int64_t deadline;
  if (!p->tls)
    return -1;
  deadline = orbis_mono_s() + 10;
  for (;;) {
    int n = tls_read(p->tls, buf, cap);
    if (n != 0)
      return n; /* >0 data, <0 closed/error */
    if (orbis_mono_s() >= deadline)
      return -2; /* no data within the deadline */
    usleep(20000);
  }
}

int sd_drive_put(const sd_cfg_t *c, const char *zip_path,
                 const char *drive_name) {
  struct ps4_io p;
  sd_io_t io;
  int rc;
  memset(&p, 0, sizeof p);
  p.fd = -1;
  io.ctx = &p;
  io.write = ps4_io_write;
  io.read = ps4_io_read;
  rc = sd_drive_put_io(c, zip_path, drive_name, &io);
  ps4_io_close(&p);
  return rc;
}

#endif /* SD_PS4 */
