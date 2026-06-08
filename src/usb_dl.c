/*
 * usb_dl.c
 *
 * Linux-only source-level reconstruction of the CVITEK/Sophgo/Milk-V `usb_dl` host tool,
 * reduced to the normal Linux/XML flashing path for CV180x/CV181x.
 *
 * What is intentionally NOT included here:
 *   - vendor static library internals that were linked into the original binaries
 *   - MinGW CRT/libgcc/MSVCRT glue from the Windows binary
 *   - glibc/pthread implementation details from the Linux binary
 *
 * Build-time dependency boundary:
 *   this file uses the Linux CDC ACM tty driver directly.  No direct USB userspace-library dependency remains.
 *
 * This compatibility-trimmed variant keeps the vendor-style short command-line API
 * for the normal Linux/XML flashing path and flashes immediately like the vendor binary.
 *
 * This trimmed variant permanently selects the Linux/XML flashing path.
 * The OS selector, ramboot path, MAC update path, and non-CV180x/CV181x chip paths are removed.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <poll.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <expat.h>

#define ROM_VID 0x3346u
#define ROM_PID 0x1000u

#ifndef CV_DL_MAGIC_PATH
#define CV_DL_MAGIC_PATH "./cv_dl_magic.bin"
#endif

#define USB_READ_TIMEOUT_MS     3000
#define USB_WRITE_TIMEOUT_MS    5000
#define USB_CONNECT_TIMEOUT_MS  30000

#define MSG_HEADER_SHORT 8u
#define MSG_HEADER_LONG  16u
#define MSG_SCRATCH_SIZE 21u
#define SMALL_PACKET_SIZE 0x100u
#define USB_BULK_MAX_SIZE 0x80000u
#define CIMG_RAW_CHUNK_SIZE 0x1000000u
#define CIMG_HEADER_SIZE 0x40u

#define DUMMY_ADDR       0xffu
#define UBOOT_CMD_ADDR   0x04003000u

/* Constants recovered from `usb_dl.c` in both binaries. */
enum cvi_cmd {
    CVI_USB_TX_DATA_TO_RAM = 0x00,
    CVI_USB_TX_FLAG        = 0x01,
    CV_USB_BREAK           = 0x02,
    CV_USB_KEEP_DL         = 0x03,
    CV_USB_UBREAK          = 0x04,
    CV_USB_PRG_CMD         = 0x06,
    CVI_USB_REBOOT         = 0x16,
    CVI_USB_S2D            = 0x81,
    CVI_USB_D2S            = 0x82,
    CVI_USB_PROGRAM        = 0x83,
};


struct vidpid {
    uint16_t vid;
    uint16_t pid;
};

struct image_entry {
    char *label;
    char *file;
    char *path;
    uint64_t offset;
    uint64_t part_size;
    uint64_t file_size;
    struct image_entry *next;
};

struct manifest {
    struct image_entry *head;
    struct image_entry *tail;
    size_t count;
    char storage[32];
};

struct usbdl {
    uint16_t vid;
    uint16_t pid;
    int fd;
    int port_level;
    int port_num;
    char tty_path[PATH_MAX];
    char usb_path[64];

    uint32_t fip_tx_offset;
    uint32_t fip_tx_size;
    uint32_t fip_size;
    uint64_t sent_size;
    uint64_t total_size;

    int verbose;
    int quiet;
    int allow_timeout;
    int read_timeout_forever;
    int settle_ms;
    int debug_xfer;
};

struct opts {
    const char *image_dir;
    const char *magic_path;
    uint16_t vid;
    uint16_t pid;
    bool have_vidpid;
    int port_level;
    int port_num;
    bool query;
    bool verbose;
    bool quiet;
    int connect_timeout_ms;
};

/* ------------------------------------------------------------------------- */
/* Logging / helpers                                                         */
/* ------------------------------------------------------------------------- */

static void log_msg(struct usbdl *c, int level, const char *fmt, ...) {
    (void)level;
    if (c && c->quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n, s);
    if (!p) {
        perror("calloc");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) {
        perror("strdup");
        exit(2);
    }
    return p;
}

static char *xasprintf3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *out = xcalloc(la + lb + lc + 1, 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc);
    return out;
}

static char *join_path(const char *dir, const char *name) {
    if (!dir || !*dir) return xstrdup(name);
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') return xasprintf3(dir, "", name);
    return xasprintf3(dir, "/", name);
}

static int file_size_u64(const char *path, uint64_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return -errno;
    *out = (uint64_t)st.st_size;
    return 0;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int env_int(const char *name, int defval) {
    const char *v = getenv(name);
    if (!v || !*v) return defval;
    char *end = NULL;
    long x = strtol(v, &end, 0);
    if (end == v) return defval;
    if (x < 0) x = 0;
    if (x > 60000) x = 60000;
    return (int)x;
}

static uint64_t parse_u64_auto(const char *s) {
    if (!s) return 0;
    while (isspace((unsigned char)*s)) s++;
    return strtoull(s, NULL, 0);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}


/* ------------------------------------------------------------------------- */
/* CRC and protocol headers: recovered from the vendor protocol path          */
/* ------------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else crc <<= 1;
        }
    }
    return crc;
}


static size_t fill_msg_header(uint8_t *h, uint8_t cmd, uint16_t len, uint64_t addr, uint64_t data_size) {
    /* Vendor helper clears a 16-byte header scratch area. */
    memset(h, 0, MSG_HEADER_LONG);
    h[0] = cmd;
    h[1] = (uint8_t)(len >> 8);
    h[2] = (uint8_t)(len & 0xff);
    h[3] = (uint8_t)((addr >> 32) & 0xff);
    h[4] = (uint8_t)((addr >> 24) & 0xff);
    h[5] = (uint8_t)((addr >> 16) & 0xff);
    h[6] = (uint8_t)((addr >> 8) & 0xff);
    h[7] = (uint8_t)(addr & 0xff);
    if (data_size) {
        for (int i = 0; i < 8; ++i) h[8 + i] = (uint8_t)(data_size >> (8 * i));
        return MSG_HEADER_LONG;
    }
    return MSG_HEADER_SHORT;
}


/* ------------------------------------------------------------------------- */
/* CDC ACM transport: Linux tty backend for the CVI byte-stream protocol      */
/* ------------------------------------------------------------------------- */

struct acm_candidate {
    char tty_name[64];
    char devnode[PATH_MAX];
    char sys_tty_device[PATH_MAX];
    char usb_dir[PATH_MAX];
    char usb_path[64];
    char serial[128];
    uint16_t vid;
    uint16_t pid;
    int port_level;
    int port_num;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void maybe_dump_xfer(struct usbdl *c, const char *tag, const uint8_t *buf, int len, int rc) {
    if (!c || !c->debug_xfer) return;
    fprintf(stderr, "%s len=%d rc=%d", tag, len, rc);
    int n = len < 16 ? len : 16;
    for (int i = 0; i < n; ++i) fprintf(stderr, " %02x", buf[i]);
    fprintf(stderr, "\n");
}

static const char *basename_c(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void path_dirname_inplace(char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    char *slash = strrchr(p, '/');
    if (!slash) { strcpy(p, "."); return; }
    if (slash == p) { p[1] = 0; return; }
    *slash = 0;
}

static int read_text_trim(const char *path, char *out, size_t outsz) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -errno;
    if (!fgets(out, (int)outsz, fp)) { int e = ferror(fp) ? errno : ENODATA; fclose(fp); return -e; }
    fclose(fp);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
    return 0;
}

static int parse_hex16_text(const char *s, uint16_t *out) {
    unsigned v = 0;
    if (sscanf(s, "%x", &v) != 1 || v > 0xffffu) return -EINVAL;
    *out = (uint16_t)v;
    return 0;
}

static int parse_usb_port_path(const char *name, char *path_out, size_t path_out_sz, int *level, int *port_num) {
    const char *dash = strchr(name, '-');
    if (!dash || !dash[1]) return -EINVAL;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", dash + 1);
    char *colon = strchr(tmp, ':');
    if (colon) *colon = 0;
    if (!tmp[0]) return -EINVAL;
    snprintf(path_out, path_out_sz, "%s", tmp);

    int lvl = 1;
    int last = atoi(tmp);
    for (char *p = tmp; *p; ++p) {
        if (*p == '.') {
            lvl++;
            last = atoi(p + 1);
        }
    }
    *level = lvl;
    *port_num = last;
    return 0;
}

static int fill_candidate_from_tty(const char *tty_name, struct acm_candidate *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->tty_name, sizeof(out->tty_name), "%s", tty_name);
    snprintf(out->devnode, sizeof(out->devnode), "/dev/%s", tty_name);

    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "/sys/class/tty/%s/device", tty_name);
    char resolved[PATH_MAX];
    if (!realpath(link_path, resolved)) return -errno;
    snprintf(out->sys_tty_device, sizeof(out->sys_tty_device), "%s", resolved);

    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", resolved);
    for (;;) {
        char vp[PATH_MAX + 32], pp[PATH_MAX + 32], sp[PATH_MAX + 32];
        char vbuf[32], pbuf[32];
        snprintf(vp, sizeof(vp), "%s/idVendor", cur);
        snprintf(pp, sizeof(pp), "%s/idProduct", cur);
        if (read_text_trim(vp, vbuf, sizeof(vbuf)) == 0 && read_text_trim(pp, pbuf, sizeof(pbuf)) == 0) {
            if (parse_hex16_text(vbuf, &out->vid) != 0 || parse_hex16_text(pbuf, &out->pid) != 0) return -EINVAL;
            snprintf(out->usb_dir, sizeof(out->usb_dir), "%s", cur);
            (void)parse_usb_port_path(basename_c(cur), out->usb_path, sizeof(out->usb_path), &out->port_level, &out->port_num);
            snprintf(sp, sizeof(sp), "%s/serial", cur);
            if (read_text_trim(sp, out->serial, sizeof(out->serial)) != 0) out->serial[0] = 0;
            return 0;
        }
        if (strcmp(cur, "/") == 0 || strcmp(cur, "/sys") == 0) break;
        path_dirname_inplace(cur);
    }
    return -ENODEV;
}

static bool candidate_matches(const struct acm_candidate *cand, const struct vidpid *pairs, size_t npairs,
                              int want_level, int want_num) {
    bool want = npairs == 0;
    for (size_t i = 0; i < npairs; ++i) {
        if (cand->vid == pairs[i].vid && cand->pid == pairs[i].pid) { want = true; break; }
    }
    if (!want) return false;
    if (want_level >= 0 && cand->port_level != want_level) return false;
    if (want_num >= 0 && cand->port_num != want_num) return false;
    return true;
}

static int configure_acm_fd(int fd) {
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) return -errno;
    cfmakeraw(&tio);
#ifdef B921600
    cfsetispeed(&tio, B921600);
    cfsetospeed(&tio, B921600);
#else
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
#endif
    tio.c_cflag |= CLOCAL | CREAD;
#ifdef HUPCL
    tio.c_cflag &= ~HUPCL;
#endif
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) return -errno;
    (void)tcflush(fd, TCIOFLUSH);

    /*
     * The original libusb backend sends CDC SET_CONTROL_LINE_STATE with
     * wValue = 0, i.e. DTR/RTS deasserted.  Do not assert modem lines here:
     * some CVI ROM stages reset or drop the ACM endpoint when DTR/RTS are
     * raised by the tty driver.
     */
#if defined(TIOCMBIC)
    int modem = 0;
#ifdef TIOCM_DTR
    modem |= TIOCM_DTR;
#endif
#ifdef TIOCM_RTS
    modem |= TIOCM_RTS;
#endif
    if (modem) (void)ioctl(fd, TIOCMBIC, &modem);
#endif
    return 0;
}


static bool acm_transient_error(int rc) {
    return rc == -EIO || rc == -ENODEV || rc == -EPIPE;
}

static int acm_write_once(struct usbdl *c, const uint8_t *buf, int len, int timeout_ms) {
    if (!c || c->fd < 0) return -ENODEV;
    int total = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (total < len) {
        int wait = timeout_ms < 0 ? -1 : (int)(deadline - now_ms());
        if (wait < 0) return -ETIMEDOUT;
        struct pollfd pfd = { .fd = c->fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, wait);
        if (pr < 0) { if (errno == EINTR) continue; return -errno; }
        if (pr == 0) return -ETIMEDOUT;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        ssize_t wr = write(c->fd, buf + total, (size_t)(len - total));
        if (wr < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -errno;
        }
        if (wr == 0) return -EIO;
        total += (int)wr;
    }
    maybe_dump_xfer(c, "acm TX ", buf, len, total);
    return total;
}

static int acm_read_once(struct usbdl *c, uint8_t *buf, int len, int timeout_ms) {
    if (!c || c->fd < 0) return -ENODEV;
    int total = 0;
    int forever = c->read_timeout_forever;
    int64_t deadline = now_ms() + timeout_ms;
    while (total < len) {
        int wait = forever ? -1 : (int)(deadline - now_ms());
        if (!forever && wait < 0) {
            if (c->allow_timeout) return 0;
            return total ? -EIO : -ETIMEDOUT;
        }
        struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, wait);
        if (pr < 0) { if (errno == EINTR) continue; return -errno; }
        if (pr == 0) {
            if (c->allow_timeout) return 0;
            return total ? -EIO : -ETIMEDOUT;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (c->allow_timeout) return 0;
            return -EIO;
        }
        ssize_t rd = read(c->fd, buf + total, (size_t)(len - total));
        if (rd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (c->allow_timeout && (errno == EIO || errno == ENODEV)) return 0;
            return -errno;
        }
        if (rd == 0) {
            if (c->allow_timeout) return 0;
            return -EIO;
        }
        total += (int)rd;
    }
    maybe_dump_xfer(c, "acm RX ", buf, len, total);
    return total;
}

static int read_verify(struct usbdl *c, uint16_t expected_crc, bool verify_crc) {
    uint8_t ack[16] = {0};
    int rc = acm_read_once(c, ack, (int)sizeof(ack), USB_READ_TIMEOUT_MS);
    if (rc < 0) return rc;

    if (verify_crc) {
        uint16_t got = ((uint16_t)ack[2] << 8) | ack[3];
        if (got != expected_crc) {
            fprintf(stderr, "ACK_CRC_ERROR cmp_crc: %x, ret_crc: %x\n", expected_crc, got);
            return -EBADMSG;
        }
        c->fip_tx_offset = ((uint32_t)ack[8] << 24) | ((uint32_t)ack[9] << 16) |
                           ((uint32_t)ack[10] << 8) | ack[11];
        c->fip_tx_size   = ((uint32_t)ack[12] << 24) | ((uint32_t)ack[13] << 16) |
                           ((uint32_t)ack[14] << 8) | ack[15];
    }
    return 0;
}

static int send_req_data(struct usbdl *c, uint8_t cmd, uint64_t addr, const void *data, size_t data_len) {
    if (data_len > 0xffffu) return -EINVAL;
    sleep_ms(1);

    /*
     * Vendor usb_send_data() builds the protocol header in a 16-byte
     * scratch buffer, copies only the returned header length, then appends
     * the payload.  Do the same here.  Calling fill_msg_header() directly
     * on an 8+payload allocation is wrong because the vendor helper clears
     * a 16-byte header scratch area.  That bug showed up as glibc
     * "buffer overflow detected" on the short 4-byte MGN1 flag packet.
     */
    uint8_t hdr[MSG_HEADER_LONG];
    size_t header_len = fill_msg_header(hdr, cmd, (uint16_t)data_len, addr, 0);
    uint8_t *pkt = xcalloc(header_len + data_len, 1);
    memcpy(pkt, hdr, header_len);
    if (data_len && data) memcpy(pkt + header_len, data, data_len);

    if (cmd == CV_USB_BREAK || cmd == CV_USB_UBREAK || cmd == CVI_USB_PROGRAM || cmd == CVI_USB_REBOOT)
        c->allow_timeout = 1;
    int rc = acm_write_once(c, pkt, (int)(header_len + data_len), USB_WRITE_TIMEOUT_MS);
    free(pkt);
    if (rc < 0) { c->allow_timeout = 0; return rc; }
    rc = read_verify(c, 0, false);
    c->allow_timeout = 0;
    return rc;
}

static int send_file_sliced(struct usbdl *c, const char *path, uint32_t offset, uint32_t size,
                            uint64_t addr, uint8_t cmd) {
    sleep_ms(1);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -errno;
    }
    uint64_t fs = 0;
    if (file_size_u64(path, &fs) != 0) { fclose(fp); return -errno; }
    if ((uint64_t)offset + (uint64_t)size > fs) {
        fprintf(stderr, "updating file is too large, offset: %u, size: %u, file_size: %llu\n",
                offset, size, (unsigned long long)fs);
        fclose(fp);
        return -EFBIG;
    }
    if (size == 0 || (uint64_t)size > fs) size = (uint32_t)fs;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) { fclose(fp); return -errno; }

    uint8_t pkt[SMALL_PACKET_SIZE];
    uint32_t left = size;
    while (left) {
        size_t header_len = MSG_HEADER_SHORT;
        size_t max_payload = SMALL_PACKET_SIZE - header_len;
        size_t chunk = left > max_payload ? max_payload : left;
        memset(pkt, 0, sizeof(pkt));
        size_t tx_len = header_len + chunk;
        (void)fill_msg_header(pkt, cmd, (uint16_t)tx_len, addr, 0);
        if (chunk && fread(pkt + header_len, 1, chunk, fp) != chunk) {
            fclose(fp);
            return -EIO;
        }
        int rc = acm_write_once(c, pkt, (int)tx_len, USB_WRITE_TIMEOUT_MS);
        if (rc < 0) { fclose(fp); return rc; }
        uint16_t crc = crc16_ccitt(pkt, (size_t)rc);
        int sent_payload = rc - (int)header_len;
        if (sent_payload < 0) sent_payload = 0;
        rc = read_verify(c, crc, true);
        if (rc != 0) { fclose(fp); return rc; }
        addr += (uint32_t)sent_payload;
        left -= (uint32_t)sent_payload;
    }
    fclose(fp);
    return 0;
}

static int recv_data(struct usbdl *c, uint64_t addr, void *buf, size_t len) {
    if (len > 0xffffu) return -EINVAL;
    uint8_t hdr[MSG_SCRATCH_SIZE];
    (void)fill_msg_header(hdr, CVI_USB_D2S, (uint16_t)len, addr, 0);
    int rc = acm_write_once(c, hdr, MSG_HEADER_SHORT, USB_WRITE_TIMEOUT_MS);
    if (rc < 0) return rc;
    sleep_ms(10);
    return acm_read_once(c, buf, (int)len, USB_READ_TIMEOUT_MS);
}

static int msg_s2d_once(struct usbdl *c, uint64_t addr, const uint8_t *buf, size_t len) {
    if (len > UINT32_MAX) return -EINVAL;
    uint8_t hdr[MSG_SCRATCH_SIZE];
    size_t header_len = fill_msg_header(hdr, CVI_USB_S2D, MSG_HEADER_LONG, addr, len);
    int rc = acm_write_once(c, hdr, (int)header_len, USB_WRITE_TIMEOUT_MS);
    if (rc < 0) return rc;
    uint16_t crc = crc16_ccitt(hdr, (size_t)rc);
    rc = read_verify(c, crc, true);
    if (rc != 0) return rc;
    rc = acm_write_once(c, buf, (int)len, USB_WRITE_TIMEOUT_MS);
    if (rc < 0) return rc;
    return (rc == (int)len) ? 0 : -EIO;
}

static int send_chunk_from_file(struct usbdl *c, FILE *fp, uint64_t size, uint64_t addr) {
    uint8_t *buf = xcalloc(USB_BULK_MAX_SIZE, 1);
    uint64_t left = size;
    while (left) {
        size_t chunk = left > USB_BULK_MAX_SIZE ? USB_BULK_MAX_SIZE : (size_t)left;
        if (fread(buf, 1, chunk, fp) != chunk) { free(buf); return -EIO; }
        int rc = msg_s2d_once(c, addr, buf, chunk);
        if (rc != 0) { free(buf); return rc; }
        addr += chunk;
        left -= chunk;
    }
    free(buf);
    return 0;
}

static uint64_t get_update_addr(struct usbdl *c) {
    uint8_t buf[8] = {0};
    (void)recv_data(c, 0, buf, sizeof(buf));
    return read_le64(buf);
}

static int get_dl_stage(struct usbdl *c) {
    uint8_t buf[64] = {0};

    /*
     * Match the vendor binary here: get_dl_stage() stores the return value
     * from usb_recv_data(), but does not branch on it.  It only tests
     * buf[6].  With the ACM backend, the stage switch often appears as an
     * EIO/timeout while the tty is being torn down; treating that as a
     * transient retry reopens the same 5340 node forever.  Leaving buf[]
     * zeroed on a failed read makes buf[6] != 0x82, so the ROM loop exits
     * exactly like the original code.
     */
    (void)recv_data(c, 0, buf, sizeof(buf));
    return (buf[6] == CVI_USB_D2S) ? 0 : 1;
}

static int setenv_cmd(struct usbdl *c, const char *name, const char *value) {
    char cmd[256];
    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd) - 8, "setenv %s %s", name, value ? value : "");
    if (c && c->verbose) fprintf(stderr, "setenv cmd: %s, len: %zu\n", cmd, strlen(cmd) + 8);
    return send_req_data(c, CV_USB_PRG_CMD, 0, cmd, strlen(cmd) + 8);
}

static int reboot_device(struct usbdl *c) {
    log_msg(c, 2, "reboot usb device\n");
    return send_req_data(c, CVI_USB_REBOOT, UBOOT_CMD_ADDR, NULL, 0);
}


static void wait_for_tty_disappear(const char *path, int timeout_ms) {
    if (!path || !*path) return;
    int waited = 0;
    while (waited <= timeout_ms) {
        if (access(path, F_OK) != 0) return;
        sleep_ms(50);
        waited += 50;
    }
}

static void usb_release_device(struct usbdl *c) {
    if (!c || c->fd < 0) return;
    close(c->fd);
    c->fd = -1;
    c->tty_path[0] = 0;
}

static void usb_deinit(struct usbdl *c) {
    usb_release_device(c);
}

static int usb_restart_context(struct usbdl *c) {
    char old_path[PATH_MAX];
    old_path[0] = '\0';
    if (c && c->tty_path[0]) snprintf(old_path, sizeof(old_path), "%s", c->tty_path);
    usb_release_device(c);
    if (old_path[0]) wait_for_tty_disappear(old_path, env_int("CVI_ACM_WAIT_GONE_MS", 1500));
    return 0;
}

static int usb_init(struct usbdl *c) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->settle_ms = env_int("CVI_ACM_SETTLE_MS", env_int("CVI_USB_SETTLE_MS", 0));
    c->debug_xfer = env_int("CVI_ACM_DEBUG_XFER", env_int("CVI_USB_DEBUG_XFER", 0)) ? 1 : 0;
    return 0;
}

static int open_matching_device(struct usbdl *c, const struct vidpid *pairs, size_t npairs,
                                int port_level, int port_num) {
    const char *forced = getenv("CVI_ACM_DEVICE");
    if (forced && *forced) {
        const char *tty = basename_c(forced);
        struct acm_candidate cand;
        int frc = fill_candidate_from_tty(tty, &cand);
        if (frc != 0) {
            memset(&cand, 0, sizeof(cand));
            snprintf(cand.tty_name, sizeof(cand.tty_name), "%s", tty);
            snprintf(cand.devnode, sizeof(cand.devnode), "%s", forced);
            cand.vid = pairs && npairs ? pairs[0].vid : ROM_VID;
            cand.pid = pairs && npairs ? pairs[0].pid : ROM_PID;
        }
        if (!candidate_matches(&cand, pairs, npairs, port_level, port_num)) return -ENODEV;
        int fd = open(cand.devnode, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) return -errno;
        int rc = configure_acm_fd(fd);
        if (rc != 0) { close(fd); return rc; }
        c->fd = fd;
        c->vid = cand.vid;
        c->pid = cand.pid;
        c->port_level = cand.port_level;
        c->port_num = cand.port_num;
        snprintf(c->tty_path, sizeof(c->tty_path), "%s", cand.devnode);
        snprintf(c->usb_path, sizeof(c->usb_path), "%s", cand.usb_path);
        if (c->settle_ms > 0) sleep_ms(c->settle_ms);
        fprintf(stderr, "found acm device vid=0x%04x pid=0x%04x portlevel=%d portnum=%d tty=%s path=%s\n",
                cand.vid, cand.pid, cand.port_level, cand.port_num, cand.devnode, cand.usb_path);
        return 0;
    }

    DIR *d = opendir("/sys/class/tty");
    if (!d) return -errno;
    int rc = -ENODEV;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "ttyACM", 6) != 0) continue;
        struct acm_candidate cand;
        if (fill_candidate_from_tty(de->d_name, &cand) != 0) continue;
        if (!candidate_matches(&cand, pairs, npairs, port_level, port_num)) continue;

        int fd = open(cand.devnode, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) { rc = -errno; continue; }
        rc = configure_acm_fd(fd);
        if (rc != 0) { close(fd); continue; }

        c->fd = fd;
        c->vid = cand.vid;
        c->pid = cand.pid;
        c->port_level = cand.port_level;
        c->port_num = cand.port_num;
        snprintf(c->tty_path, sizeof(c->tty_path), "%s", cand.devnode);
        snprintf(c->usb_path, sizeof(c->usb_path), "%s", cand.usb_path);

        if (c->settle_ms > 0) {
            if (c->verbose) fprintf(stderr, "post-open settle: %d ms\n", c->settle_ms);
            sleep_ms(c->settle_ms);
        }

        fprintf(stderr, "found acm device vid=0x%04x pid=0x%04x portlevel=%d portnum=%d tty=%s path=%s\n",
                cand.vid, cand.pid, cand.port_level, cand.port_num, cand.devnode, cand.usb_path);
        closedir(d);
        return 0;
    }
    closedir(d);
    return rc;
}

static int connect_wait(struct usbdl *c, const struct vidpid *pairs, size_t npairs,
                        int port_level, int port_num, int timeout_ms) {
    int waited = 0;
    while (timeout_ms < 0 || waited <= timeout_ms) {
        int rc = open_matching_device(c, pairs, npairs, port_level, port_num);
        if (rc == 0) return 0;
        if (rc == -EACCES || rc == -EPERM) return rc;
        sleep_ms(100);
        waited += 100;
    }
    fprintf(stderr, "timeout waiting for ACM device");
    for (size_t i = 0; i < npairs; ++i) fprintf(stderr, " %04x:%04x", pairs[i].vid, pairs[i].pid);
    fprintf(stderr, "\n");
    return -ETIMEDOUT;
}

static int query_devices(struct usbdl *c, const struct vidpid *pairs, size_t npairs,
                         int port_level, int port_num) {
    (void)c;
    DIR *d = opendir("/sys/class/tty");
    if (!d) return -errno;
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "ttyACM", 6) != 0) continue;
        struct acm_candidate cand;
        if (fill_candidate_from_tty(de->d_name, &cand) != 0) continue;
        if (!candidate_matches(&cand, pairs, npairs, port_level, port_num)) continue;
        printf("vid=0x%04x pid=0x%04x portlevel=%d portnum=%d tty=%s path=%s%s%s\n",
               cand.vid, cand.pid, cand.port_level, cand.port_num, cand.devnode, cand.usb_path,
               cand.serial[0] ? " serial=" : "", cand.serial[0] ? cand.serial : "");
        found++;
    }
    closedir(d);
    return found ? 0 : -ENODEV;
}

/* ------------------------------------------------------------------------- */
/* Manifest parsing: Linux XML partition path only */
/* ------------------------------------------------------------------------- */

static void manifest_add(struct manifest *m, struct image_entry *e) {
    if (!m->head) m->head = e;
    else m->tail->next = e;
    m->tail = e;
    m->count++;
}

static void manifest_free(struct manifest *m) {
    struct image_entry *e = m->head;
    while (e) {
        struct image_entry *n = e->next;
        free(e->label); free(e->file); free(e->path); free(e);
        e = n;
    }
    memset(m, 0, sizeof(*m));
}

static const char *xml_local_name(const char *name) {
    const char *colon = strrchr(name ? name : "", ':');
    return colon ? colon + 1 : (name ? name : "");
}

static const char *xml_attr_get(const char **attrs, const char *name) {
    if (!attrs || !name) return NULL;
    for (size_t i = 0; attrs[i] && attrs[i + 1]; i += 2) {
        if (strcasecmp(xml_local_name(attrs[i]), name) == 0) return attrs[i + 1];
    }
    return NULL;
}

static char *find_file_with_ext(const char *dir, const char *want_substr, const char *ext1, const char *ext2) {
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *ent;
    char *best = NULL;
    while ((ent = readdir(d))) {
        const char *n = ent->d_name;
        const char *dot = strrchr(n, '.');
        if (!dot) continue;
        if (ext1 && strcasecmp(dot, ext1) != 0 && (!ext2 || strcasecmp(dot, ext2) != 0)) continue;
        if (want_substr && *want_substr && !strstr(n, want_substr)) continue;
        best = join_path(dir, n);
        break;
    }
    closedir(d);
    return best;
}

struct xml_manifest_ctx {
    const char *image_dir;
    struct manifest *manifest;
    bool storage_type_seen;
    bool fip_added;
};

static void manifest_add_file(struct xml_manifest_ctx *x, const char *file) {
    if (!x || !x->manifest || !file || !*file) return;
    struct image_entry *e = xcalloc(1, sizeof(*e));
    e->file = xstrdup(file);
    e->label = xstrdup(file);
    e->path = join_path(x->image_dir, e->file);
    (void)file_size_u64(e->path, &e->file_size);
    manifest_add(x->manifest, e);
}

static void xml_start_element(void *user_data, const XML_Char *name, const XML_Char **attrs) {
    (void)name;
    struct xml_manifest_ctx *x = (struct xml_manifest_ctx *)user_data;
    if (!x || !x->manifest) return;

    if (!x->storage_type_seen) {
        const char *storage = xml_attr_get((const char **)attrs, "type");
        if (storage && *storage) {
            snprintf(x->manifest->storage, sizeof(x->manifest->storage), "%s", storage);
            x->storage_type_seen = true;
            if (!x->fip_added && strncmp(storage, "emmc", 4) == 0) {
                manifest_add_file(x, "fip.bin");
                x->fip_added = true;
            }
        }
        return;
    }

    const char *szkb = xml_attr_get((const char **)attrs, "size_in_kb");
    if (!szkb || !*szkb || parse_u64_auto(szkb) == 0) return;

    const char *file = xml_attr_get((const char **)attrs, "file");
    if (!file || !*file) return;

    manifest_add_file(x, file);
}

static int parse_manifest_xml(const char *image_dir, struct manifest *m) {
    memset(m, 0, sizeof(*m));

    char *xml = find_file_with_ext(image_dir, "partition", ".xml", NULL);
    if (!xml) xml = find_file_with_ext(image_dir, NULL, ".xml", NULL);
    if (!xml) return -ENOENT;

    FILE *fp = fopen(xml, "rb");
    if (!fp) {
        int rc = -errno;
        free(xml);
        return rc;
    }

    XML_Parser parser = XML_ParserCreate(NULL);
    if (!parser) {
        fclose(fp);
        free(xml);
        return -ENOMEM;
    }

    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);

    struct xml_manifest_ctx x = {
        .image_dir = image_dir,
        .manifest = m,
        .storage_type_seen = false,
        .fip_added = false,
    };
    XML_SetUserData(parser, &x);
    XML_SetElementHandler(parser, xml_start_element, NULL);

    char buf[16384];
    int rc = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (ferror(fp)) {
            rc = -EIO;
            break;
        }
        int done = feof(fp);
        if (XML_Parse(parser, buf, (int)n, done) == XML_STATUS_ERROR) {
            fprintf(stderr, "XML parse error in %s at line %lu:%lu: %s\n",
                    xml,
                    (unsigned long)XML_GetCurrentLineNumber(parser),
                    (unsigned long)XML_GetCurrentColumnNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
            rc = -EINVAL;
            break;
        }
        if (done) break;
    }

    XML_ParserFree(parser);
    fclose(fp);

    if (rc == 0) {
        fprintf(stderr, "xml file is %s\nxml storage type: %s\n", xml, m->storage);
        rc = m->count ? 0 : -ENOENT;
    }

    free(xml);
    if (rc != 0) manifest_free(m);
    return rc;
}

static int parse_manifest(const struct opts *o, struct manifest *m) {
    return parse_manifest_xml(o->image_dir, m);
}

/* ------------------------------------------------------------------------- */
/* CIMG transfer: Linux/XML path expects already packaged CIMG images         */
/* ------------------------------------------------------------------------- */

/* Linux/XML path expects images as already packaged CIMG files, like the vendor binary. */

/* ------------------------------------------------------------------------- */
/* ROM stage: recovered usb_rom_dl/2x/3x/6x                                  */
/* ------------------------------------------------------------------------- */

static char *firmware_file(const char *dir, const char *name) {
    return join_path(dir, name);
}

static int send_flag_word(struct usbdl *c, uint32_t addr, uint8_t cmd, const char bytes[4], const char *label) {
    fprintf(stderr, "%s\n", label);
    return send_req_data(c, cmd, addr, bytes, 4);
}

static int send_mgn1(struct usbdl *c, uint32_t addr, uint8_t cmd) {
    const char flag[4] = {'1','N','G','M'};
    return send_flag_word(c, addr, cmd, flag, "set MGN1 flag");
}
static int send_break_cmd(struct usbdl *c, uint8_t cmd, uint32_t addr) {
    fprintf(stderr, "break\n");
    return send_req_data(c, cmd, addr, NULL, 0);
}

static int connect_rom(struct usbdl *c, const struct opts *o) {
    const struct vidpid rom = {ROM_VID, ROM_PID};
    return connect_wait(c, &rom, 1, o->port_level, o->port_num, o->connect_timeout_ms);
}

static int rom_loop_common(struct usbdl *c, const struct opts *o, const char *fip,
                           uint32_t fip_addr, uint32_t flag_addr, uint8_t fip_cmd,
                           uint8_t flag_cmd, uint8_t break_cmd, uint32_t break_addr) {
    for (;;) {
        int rc = connect_rom(c, o);
        if (rc != 0) return rc;
        rc = get_dl_stage(c);
        if (acm_transient_error(rc)) {
            usb_restart_context(c);
            sleep_ms(50);
            continue;
        }
        if (rc < 0) return rc;
        if (rc == 1) return 0;

        fprintf(stderr, "send magic bin\n");
        rc = send_file_sliced(c, o->magic_path, 0, 0, DUMMY_ADDR, CV_USB_KEEP_DL);
        if (acm_transient_error(rc)) {
            usb_restart_context(c);
            sleep_ms(50);
            continue;
        }
        if (rc != 0) return rc;

        uint32_t off = c->fip_tx_offset;
        uint32_t sz = c->fip_tx_size;
        fprintf(stderr, "fip_tx_offset: %u, fip_tx_size: %u\n", off, sz);
        fprintf(stderr, "Send fip.bin...\n");
        rc = send_file_sliced(c, fip, off, sz, fip_addr, fip_cmd);
        if (rc != 0) return rc;
        rc = send_mgn1(c, flag_addr, flag_cmd);
        if (rc != 0) return rc;
        rc = send_break_cmd(c, break_cmd, break_addr);
        if (rc != 0) return rc;
        fprintf(stderr, "Connecting to ROM 2nd stage...\n");
        sleep_ms(10);
        rc = usb_restart_context(c);
        if (rc != 0) return rc;
    }
    /* unreachable in vendor-style loop */
    return 0;
}

static int rom_stage_cv180x_181x(struct usbdl *c, const struct opts *o) {
    char *fip = firmware_file(o->image_dir, "fip.bin");
    uint64_t fs = 0;
    if (file_size_u64(fip, &fs) == 0) c->fip_size = (uint32_t)fs;
    fprintf(stderr, "USB download start...\n");

    int rc = connect_rom(c, o);
    if (rc != 0) { free(fip); return rc; }
    fprintf(stderr, "send magic bin\n");
    rc = send_file_sliced(c, o->magic_path, 0, 0, DUMMY_ADDR, CV_USB_KEEP_DL);
    if (rc != 0) { free(fip); return rc; }

    rc = usb_restart_context(c);
    if (rc != 0) { free(fip); return rc; }
    rc = connect_rom(c, o);
    if (rc != 0) { free(fip); return rc; }
    fprintf(stderr, "fip.bin size: %u\n", c->fip_size);
    rc = send_file_sliced(c, fip, 0, 0x1000, 0, CVI_USB_TX_DATA_TO_RAM);
    if (rc != 0) { free(fip); return rc; }
    rc = send_mgn1(c, 0x0e000004u, CVI_USB_TX_FLAG);
    if (rc != 0) { free(fip); return rc; }
    rc = send_break_cmd(c, CV_USB_BREAK, DUMMY_ADDR);
    if (rc != 0) { free(fip); return rc; }
    fprintf(stderr, "Connecting to ROM 2nd stage...\n");
    sleep_ms(10);
    rc = usb_restart_context(c);
    if (rc != 0) { free(fip); return rc; }

    rc = rom_loop_common(c, o, fip, 0, 0x0e000004u, CVI_USB_TX_DATA_TO_RAM, CVI_USB_TX_FLAG, CV_USB_BREAK, DUMMY_ADDR);
    free(fip);
    return rc;
}
static int rom_stage(struct usbdl *c, const struct opts *o) {
    return rom_stage_cv180x_181x(c, o);
}

/* ------------------------------------------------------------------------- */
/* U-Boot / utask stage                                                      */
/* ------------------------------------------------------------------------- */

static int send_cimg_entry(struct usbdl *c, const struct image_entry *e, uint64_t update_addr) {
    if (e->file_size == 0) {
        fprintf(stderr, "skip empty/missing image: %s\n", e->path ? e->path : e->file);
        return 0;
    }

    fprintf(stderr, "downloading file: %s\n", e->path);
    FILE *fp = fopen(e->path, "rb");
    if (!fp) { perror(e->path); return -errno; }

    /* Vendor flow sends the 0x40 image header first, then parses the same header locally. */
    int rc = send_chunk_from_file(c, fp, CIMG_HEADER_SIZE, update_addr);
    if (rc != 0) { fclose(fp); return rc; }

    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -errno; }
    uint8_t hdr[CIMG_HEADER_SIZE] = {0};
    size_t got = fread(hdr, 1, sizeof(hdr), fp);
    if (got != sizeof(hdr)) {
        fprintf(stderr, "get image header filed! %zu %u\n", got, CIMG_HEADER_SIZE);
        fclose(fp);
        return -EIO;
    }

    uint32_t chunk_header_size = read_le32(hdr + 8);
    uint32_t chunk_count = read_le32(hdr + 12);
    uint32_t total_payload = read_le32(hdr + 16);
    fprintf(stderr, "magic: %x, chunk_sz: %x, total_chunk: %x, file_size: %x\n",
            read_le32(hdr), chunk_header_size, chunk_count, total_payload);

    uint32_t remaining = total_payload;
    for (uint32_t i = 0; i < chunk_count && remaining; ++i) {
        uint32_t tx_limit = chunk_header_size + CIMG_RAW_CHUNK_SIZE;
        uint32_t tx = remaining > tx_limit ? tx_limit : remaining;
        rc = send_chunk_from_file(c, fp, tx, update_addr);
        if (rc != 0) { fclose(fp); return rc; }

        fprintf(stderr, "CVI_USB_PROGRAM\n");
        c->read_timeout_forever = 1;
        rc = send_req_data(c, CVI_USB_PROGRAM, UBOOT_CMD_ADDR, NULL, 0);
        c->read_timeout_forever = 0;
        if (rc != 0) { fclose(fp); return rc; }

        remaining -= tx;
        c->sent_size += tx;
        fprintf(stderr, "updated size: %llu/%llu(%llu%%)\n",
                (unsigned long long)c->sent_size, (unsigned long long)c->total_size,
                c->total_size ? (unsigned long long)(c->sent_size * 100 / c->total_size) : 100ull);
    }
    fclose(fp);
    return 0;
}

static void calculate_all_file_size(struct usbdl *c, const struct manifest *m) {
    c->total_size = 0;
    for (const struct image_entry *e = m->head; e; e = e->next) {
        c->total_size += e->file_size;
    }
    fprintf(stderr, "update total size: %llu byte\n", (unsigned long long)c->total_size);
}

static bool is_fip_name(const char *file) {
    /* Vendor usb_uboot_dl() uses memcmp(file, "fip.bin", 7). */
    return file && memcmp(file, "fip.bin", 7) == 0;
}

static int send_fip_uboot_entry(struct usbdl *c, const struct opts *o, const struct image_entry *e, uint64_t update_addr) {
    char fsz[32];
    snprintf(fsz, sizeof(fsz), "0x%x", c->fip_size);
    int rc = setenv_cmd(c, "filesize", fsz);
    if (rc != 0) return rc;

    rc = send_file_sliced(c, e->path, 0, 0, update_addr, CVI_USB_TX_DATA_TO_RAM);
    if (rc != 0) return rc;
    fprintf(stderr, "send fip.bin finish\n");

    rc = send_break_cmd(c, CV_USB_UBREAK, DUMMY_ADDR);
    if (rc != 0) return rc;

    c->sent_size += e->file_size;
    rc = usb_restart_context(c);
    if (rc != 0) return rc;
    return connect_rom(c, o);
}

static int uboot_stage(struct usbdl *c, const struct opts *o, const struct manifest *m) {
    uint64_t update_addr = get_update_addr(c);
    fprintf(stderr, "update address: 0x%llx\n", (unsigned long long)update_addr);

    for (const struct image_entry *e = m->head; e; e = e->next) {
        if (!e->file || !*e->file) continue;
        if (is_fip_name(e->file)) {
            int rc = send_fip_uboot_entry(c, o, e, update_addr);
            if (rc != 0) return rc;
            continue;
        }
        int rc = send_cimg_entry(c, e, update_addr);
        if (rc != 0) return rc;
    }
    return 0;
}


/* ------------------------------------------------------------------------- */
/* CLI                                                                       */
/* ------------------------------------------------------------------------- */


static int parse_chip_name(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "180x") == 0 || strcmp(s, "cv180x") == 0) return 0;
    if (strcmp(s, "181x") == 0 || strcmp(s, "cv181x") == 0) return 0;
    return -1;
}


static int parse_vidpid(const char *s, uint16_t *vid, uint16_t *pid) {
    unsigned v = 0, p = 0;
    if (sscanf(s, "%x:%x", &v, &p) != 2 || v > 0xffff || p > 0xffff) return -1;
    *vid = (uint16_t)v; *pid = (uint16_t)p;
    return 0;
}

static int parse_port(const char *s, int *level, int *num) {
    int l = -1, n = -1;
    if (sscanf(s, "%d,%d", &l, &n) != 2) return -1;
    *level = l; *num = n;
    return 0;
}

static void print_usage(FILE *fp, const char *argv0) {
    fprintf(fp,
        "Usage: %s [-V] [-v] [-q] [-r] [-d vid:pid] [-p portlevel,portnum] [-c <chip>] -i <firmware directory>\n"
        "  -c <chip>                 Choose update chip: 180x or 181x\n"
        "  -d <vid:pid>              Target device, such as USB VID:PID\n"
        "  -i <path>                 Directory where the firmware to be download\n"
        "  -p <portlevel,portnum>    Target device, as a USB physical port level and port number\n"
        "  -r                        Read port level and port number\n"
        "  -V                        Print program version\n"
        "  -v                        Increase verbosity\n"
        "  -q                        Decrease verbosity (silent mode)\n"
        "\nThis source build always uses the Linux/XML flashing path. Only 180x/181x chips are supported.\n"
        "cv_dl_magic.bin path: " CV_DL_MAGIC_PATH "\n",
        argv0);
}

int main(int argc, char **argv) {
    struct opts o;
    memset(&o, 0, sizeof(o));
    o.magic_path = CV_DL_MAGIC_PATH;
    o.port_level = -1;
    o.port_num = -1;
    o.connect_timeout_ms = USB_CONNECT_TIMEOUT_MS;

    int ch;
    while ((ch = getopt(argc, argv, "c:i:d:p:rvqVh")) != -1) {
        switch (ch) {
        case 'c':
            if (parse_chip_name(optarg) != 0) {
                fprintf(stderr, "please specify choose chip as cv180x or cv181x\n");
                return 2;
            }
            break;

        case 'i': o.image_dir = optarg; break;
        case 'd':
            if (parse_vidpid(optarg, &o.vid, &o.pid) != 0) { fprintf(stderr, "bad VID:PID: %s\n", optarg); return 2; }
            o.have_vidpid = true;
            break;
        case 'p':
            if (parse_port(optarg, &o.port_level, &o.port_num) != 0) { fprintf(stderr, "bad port selector: %s\n", optarg); return 2; }
            break;
        case 'r': o.query = true; break;
        case 'v': o.verbose = true; break;
        case 'q': o.quiet = true; break;
        case 'V': printf("usb_dl: Oct 17 2023\n"); return 0;
        case 'h': print_usage(stdout, argv[0]); return 0;
        default: print_usage(stderr, argv[0]); return 2;
        }
    }

    struct usbdl c;
    int rc = usb_init(&c);
    if (rc != 0) return 1;
    c.verbose = o.verbose ? 1 : 0;
    c.quiet = o.quiet ? 1 : 0;

    if (o.query) {
        struct vidpid pair = {ROM_VID, ROM_PID};
        if (o.have_vidpid) { pair.vid = o.vid; pair.pid = o.pid; }
        rc = query_devices(&c, &pair, 1, o.port_level, o.port_num);
        usb_deinit(&c);
        return rc == 0 ? 0 : 1;
    }

    if (!o.image_dir) {
        fprintf(stderr, "no image path specified\n");
        print_usage(stderr, argv[0]);
        usb_deinit(&c);
        return 2;
    }

    struct manifest m;
    memset(&m, 0, sizeof(m));

    rc = rom_stage(&c, &o);
    if (rc != 0) goto out;

    rc = parse_manifest(&o, &m);
    if (rc != 0) {
        fprintf(stderr, "failed to parse firmware manifest from %s\n", o.image_dir);
        goto out;
    }
    calculate_all_file_size(&c, &m);

    rc = uboot_stage(&c, &o, &m);
    if (rc != 0) goto out;

    rc = reboot_device(&c);
    if (rc == 0) fprintf(stderr, "USB download complete\n");

out:
    manifest_free(&m);
    usb_deinit(&c);
    if (rc != 0) {
        fprintf(stderr, "failed: %s (%d)\n", strerror(rc < 0 ? -rc : rc), rc);
    }
    return rc == 0 ? 0 : 1;
}
