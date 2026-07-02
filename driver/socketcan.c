/**
 * @file socketcan.c
 * @brief SocketCAN back-end for CANoScope (`PF_CAN` / `SOCK_RAW`).
 *
 * @details
 * Implements the `socketcan_*` operations declared in @ref socketcan.h on top
 * of the Linux SocketCAN subsystem.  Interface bring-up and bit-rate
 * configuration are delegated to the `ip` utility (run directly when the
 * process already has `CAP_NET_ADMIN`, otherwise transparently re-tried through
 * `pkexec` for a graphical authentication) so no bespoke Netlink code is
 * required.  Classic CAN and CAN FD frames are both supported.
 *
 * @see https://www.kernel.org/doc/Documentation/networking/can.rst
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>

#include "../inc/socketcan.h"
#include "../inc/drv_can.h"
#include "../inc/can_message.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int socketcan_iface_name_valid(const char *iface)
{
    if (!iface || iface[0] == '\0')
        return 0;

    size_t len = strlen(iface);
    if (len >= IFNAMSIZ || len >= SOCKETCAN_MAX_IFACE)
        return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)iface[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.')
            return 0;
    }
    return 1;
}

/** @brief Return non-zero if interface @p iface currently has `IFF_UP`. */
static int iface_is_up(const char *iface)
{
    char path[300];
    snprintf(path, sizeof(path), "/sys/class/net/%s/flags", iface);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    unsigned int flags = 0;
    int n = fscanf(f, "%x", &flags);
    fclose(f);
    return (n == 1) && (flags & 1u /* IFF_UP */);
}

/** @brief Locate an absolute path to the `ip` tool (pkexec ignores `$PATH`). */
static const char *ip_tool_path(void)
{
    static const char *candidates[] = {
        "/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip", NULL
    };
    for (int i = 0; candidates[i]; i++)
        if (access(candidates[i], X_OK) == 0)
            return candidates[i];
    return "ip";
}

/**
 * @brief Fork/exec @p argv and wait for completion.
 * @param argv         NULL-terminated argument vector (argv[0] is the program).
 * @param quiet_stderr  Non-zero to redirect the child's stderr to /dev/null.
 * @return 0 if the child exited with status 0, -1 otherwise.
 */
static int run_argv(const char *const argv[], int quiet_stderr)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        if (quiet_stderr) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}

/**
 * @brief Run an `ip …` command, retrying through `pkexec` if it fails.
 *
 * Tries the command directly first (succeeds with CAP_NET_ADMIN); on failure it
 * re-runs `pkexec <abs ip> <args…>` for a graphical polkit prompt, so the user
 * can bring up vcan/CAN interfaces without a terminal `sudo`.
 * @param ip_argv       `ip` argument vector (argv[0] == "ip").
 * @param quiet_stderr  Non-zero to silence stderr on the elevated attempt.
 * @return 0 on success, -1 otherwise.
 */
static int run_ip_priv(const char *const ip_argv[], int quiet_stderr)
{
    /* Fast path: succeeds when already running with sufficient privileges. */
    if (run_argv(ip_argv, 1) == 0)
        return 0;

    /* Elevated path: pkexec <abs ip> <args…> (skip the leading "ip"). */
    const char *pk_argv[40];
    size_t cap = sizeof(pk_argv) / sizeof(pk_argv[0]);
    size_t n = 0;
    pk_argv[n++] = "pkexec";
    pk_argv[n++] = ip_tool_path();
    for (size_t i = 1; ip_argv[i] != NULL && n < cap - 1; i++)
        pk_argv[n++] = ip_argv[i];
    pk_argv[n] = NULL;

    return run_argv(pk_argv, quiet_stderr);
}

/**
 * @brief Bring an interface down, apply CAN/CAN FD timing, and bring it up.
 * @param iface        Interface name.
 * @param bitrate      Nominal bit rate (bit/s).
 * @param data_bitrate CAN FD data bit rate (bit/s).
 * @param fd_mode      Non-zero to enable CAN FD.
 * @param listen_only  Non-zero for listen-only mode.
 * @param timing       Optional manual bit timing (may be NULL).
 * @return 0 on success, -1 on failure.
 */
static int configure_interface(const char *iface, uint32_t bitrate,
                                uint32_t data_bitrate, int fd_mode,
                                int listen_only,
                                const can_bit_timing_t *timing)
{
    char bitrate_buf[16];
    char data_bitrate_buf[16];
    char tq_buf[16];
    char prop_seg_buf[16];
    char phase_seg1_buf[16];
    char phase_seg2_buf[16];
    char sjw_buf[16];
    char sample_point_buf[16];
    char dtq_buf[16];
    char dprop_seg_buf[16];
    char dphase_seg1_buf[16];
    char dphase_seg2_buf[16];
    char dsjw_buf[16];
    char dsample_point_buf[16];

    snprintf(bitrate_buf, sizeof(bitrate_buf), "%u", bitrate);
    snprintf(data_bitrate_buf, sizeof(data_bitrate_buf), "%u", data_bitrate);

    if (timing && timing->enabled) {
        snprintf(tq_buf, sizeof(tq_buf), "%u", timing->brp);
        snprintf(prop_seg_buf, sizeof(prop_seg_buf), "%u", timing->prop_seg);
        snprintf(phase_seg1_buf, sizeof(phase_seg1_buf), "%u", timing->phase_seg1);
        snprintf(phase_seg2_buf, sizeof(phase_seg2_buf), "%u", timing->phase_seg2);
        snprintf(sjw_buf, sizeof(sjw_buf), "%u", timing->sjw);
        snprintf(sample_point_buf, sizeof(sample_point_buf), "%u", timing->sample_point);
        snprintf(dtq_buf, sizeof(dtq_buf), "%u", timing->brp);
        snprintf(dprop_seg_buf, sizeof(dprop_seg_buf), "%u", timing->prop_seg);
        snprintf(dphase_seg1_buf, sizeof(dphase_seg1_buf), "%u", timing->phase_seg1);
        snprintf(dphase_seg2_buf, sizeof(dphase_seg2_buf), "%u", timing->phase_seg2);
        snprintf(dsjw_buf, sizeof(dsjw_buf), "%u", timing->sjw);
        snprintf(dsample_point_buf, sizeof(dsample_point_buf), "%u", timing->sample_point);
    }

    /* Bring down first (ignore error – may already be down) */
    const char *down_args[] = {
        "ip", "link", "set", iface, "down", NULL
    };
    run_ip_priv(down_args, 1);

    const char *args[32];
    int pos = 0;

    args[pos++] = "ip";
    args[pos++] = "link";
    args[pos++] = "set";
    args[pos++] = iface;
    args[pos++] = "type";
    args[pos++] = "can";
    args[pos++] = "bitrate";
    args[pos++] = bitrate_buf;

    if (fd_mode && data_bitrate > 0) {
        args[pos++] = "dbitrate";
        args[pos++] = data_bitrate_buf;
        args[pos++] = "fd";
        args[pos++] = "on";
    }

    if (timing && timing->enabled) {
        args[pos++] = "tq";
        args[pos++] = tq_buf;
        args[pos++] = "prop-seg";
        args[pos++] = prop_seg_buf;
        args[pos++] = "phase-seg1";
        args[pos++] = phase_seg1_buf;
        args[pos++] = "phase-seg2";
        args[pos++] = phase_seg2_buf;
        args[pos++] = "sjw";
        args[pos++] = sjw_buf;
        args[pos++] = "sample-point";
        args[pos++] = sample_point_buf;
        if (fd_mode && data_bitrate > 0) {
            args[pos++] = "dtq";
            args[pos++] = dtq_buf;
            args[pos++] = "dprop-seg";
            args[pos++] = dprop_seg_buf;
            args[pos++] = "dphase-seg1";
            args[pos++] = dphase_seg1_buf;
            args[pos++] = "dphase-seg2";
            args[pos++] = dphase_seg2_buf;
            args[pos++] = "dsjw";
            args[pos++] = dsjw_buf;
            args[pos++] = "dsample-point";
            args[pos++] = dsample_point_buf;
        }
    }

    args[pos] = NULL;

    if (run_ip_priv(args, 0) != 0) {
        fprintf(stderr, "socketcan: failed to set CAN bitrate on %s\n", iface);
        return -1;
    }

    if (listen_only) {
        const char *listen_args[] = {
            "ip", "link", "set", iface, "type", "can",
            "listen-only", "on",
            NULL
        };
        if (run_ip_priv(listen_args, 0) != 0) {
            fprintf(stderr, "socketcan: failed to enable listen-only on %s\n",
                    iface);
            return -1;
        }
    }

    const char *up_args[] = {
        "ip", "link", "set", iface, "up", NULL
    };
    if (run_ip_priv(up_args, 0) != 0) {
        fprintf(stderr, "socketcan: failed to bring up %s\n", iface);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int socketcan_open(socketcan_ctx_t *ctx, const char *iface,
                   uint32_t bitrate, uint32_t data_bitrate,
                   int fd_mode, int listen_only,
                   const can_bit_timing_t *timing)
{
    if (!ctx || !socketcan_iface_name_valid(iface))
        return DRV_CAN_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->sock_fd     = -1;
    ctx->fd_enabled  = fd_mode;
    ctx->listen_only = listen_only;
    ctx->bitrate     = bitrate;
    ctx->data_bitrate = data_bitrate;
    if (timing) {
        ctx->timing = *timing;
    } else {
        ctx->timing.enabled = 0;
    }
    memcpy(ctx->iface, iface, strlen(iface) + 1);

    /* Configure interface bitrate (skip initial vcan bitrate config) */
    if (strncmp(iface, "vcan", 4) != 0) {
        if (configure_interface(iface, bitrate, data_bitrate,
                                fd_mode, listen_only, timing) != 0)
            return DRV_CAN_ERR_INIT;
    } else {
        /* For vcan interfaces, reuse an existing one when available; otherwise
         * create it.  Creation/bring-up needs CAP_NET_ADMIN, so run_ip_priv
         * elevates through pkexec (graphical auth – no terminal sudo). */
        if (if_nametoindex(iface) == 0) {
            const char *add_args[] = {
                "ip", "link", "add", "dev", iface,
                "type", "vcan", NULL
            };
            if (run_ip_priv(add_args, 0) != 0 || if_nametoindex(iface) == 0) {
                fprintf(stderr, "socketcan: failed to create %s\n", iface);
                return DRV_CAN_ERR_INIT;
            }
        }

        /* Ensure the interface is up (a freshly created vcan starts down, and
         * a down interface neither transmits nor receives).  Skip when already
         * up so we don't trigger an unnecessary pkexec authentication. */
        if (!iface_is_up(iface)) {
            const char *up_args[] = {
                "ip", "link", "set", iface, "up", NULL
            };
            run_ip_priv(up_args, 1);
        }
    }

    /* Create raw CAN socket */
    ctx->sock_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (ctx->sock_fd < 0) {
        perror("socketcan: socket");
        return DRV_CAN_ERR_INIT;
    }

    /* Enable error frames */
    can_err_mask_t err_mask = CAN_ERR_MASK;
    setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
               &err_mask, sizeof(err_mask));

    /* Keep locally transmitted frames out of the receive path. */
    int recv_own_msgs = 0;
    setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
               &recv_own_msgs, sizeof(recv_own_msgs));

    /* Enable CAN FD if requested */
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                       &enable, sizeof(enable)) < 0) {
            fprintf(stderr, "socketcan: CAN FD not supported on this "
                    "interface, falling back to classic CAN\n");
            ctx->fd_enabled = 0;
        }
    }

    /* Bind to the interface */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, iface, strlen(iface) + 1);
    if (ioctl(ctx->sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("socketcan: ioctl SIOCGIFINDEX");
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return DRV_CAN_ERR_INIT;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("socketcan: bind");
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return DRV_CAN_ERR_INIT;
    }

    return DRV_CAN_OK;
}

int socketcan_close(socketcan_ctx_t *ctx)
{
    if (!ctx) return DRV_CAN_ERR_PARAM;
    if (ctx->sock_fd >= 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    if (ctx->iface[0] != '\0' && strncmp(ctx->iface, "vcan", 4) != 0) {
        /* Do not bring down vcan interfaces on close, since they can be
         * created and managed outside the application and may be opened
         * without root privileges. */
        const char *down_args[] = {
            "ip", "link", "set", ctx->iface, "down", NULL
        };
        run_ip_priv(down_args, 1);
    }
    return DRV_CAN_OK;
}

int socketcan_send(socketcan_ctx_t *ctx, const can_msg_t *msg)
{
    if (!ctx || ctx->sock_fd < 0 || !msg)
        return DRV_CAN_ERR_PARAM;

    if (msg->is_fd && ctx->fd_enabled) {
        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = msg->id;
        if (msg->is_extended) frame.can_id |= CAN_EFF_FLAG;
        if (msg->is_remote)   frame.can_id |= CAN_RTR_FLAG;
        frame.len = msg->dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : msg->dlc;
        if (msg->is_brs) frame.flags |= CANFD_BRS;
        if (msg->is_esi) frame.flags |= CANFD_ESI;
        memcpy(frame.data, msg->data, frame.len);

        ssize_t n = write(ctx->sock_fd, &frame, sizeof(frame));
        if (n != sizeof(frame)) return DRV_CAN_ERR;
    } else {
        struct can_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id  = msg->id;
        if (msg->is_extended) frame.can_id |= CAN_EFF_FLAG;
        if (msg->is_remote)   frame.can_id |= CAN_RTR_FLAG;
        frame.can_dlc = msg->dlc > CAN_MAX_DLC ? CAN_MAX_DLC : msg->dlc;
        memcpy(frame.data, msg->data, frame.can_dlc);

        ssize_t n = write(ctx->sock_fd, &frame, sizeof(frame));
        if (n != sizeof(frame)) return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_recv(socketcan_ctx_t *ctx, can_msg_t *msg, int timeout_ms)
{
    if (!ctx || ctx->sock_fd < 0 || !msg)
        return DRV_CAN_ERR_PARAM;

    /* Wait with timeout */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->sock_fd, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(ctx->sock_fd + 1, &rfds, NULL, NULL,
                     timeout_ms >= 0 ? &tv : NULL);
    if (ret < 0)  return DRV_CAN_ERR;
    if (ret == 0) return DRV_CAN_ERR_TIMEOUT;

    memset(msg, 0, sizeof(*msg));
    clock_gettime(CLOCK_REALTIME, &msg->timestamp);

    /* Try CAN FD frame first */
    struct canfd_frame fdf;
    ssize_t n = read(ctx->sock_fd, &fdf, sizeof(fdf));
    if (n < 0) return DRV_CAN_ERR;

    if (n == (ssize_t)sizeof(struct canfd_frame) && ctx->fd_enabled) {
        /* CAN FD frame */
        msg->is_fd       = 1;
        msg->is_brs      = (fdf.flags & CANFD_BRS) ? 1 : 0;
        msg->is_esi      = (fdf.flags & CANFD_ESI) ? 1 : 0;
        msg->is_extended = (fdf.can_id & CAN_EFF_FLAG) ? 1 : 0;
        msg->is_remote   = (fdf.can_id & CAN_RTR_FLAG) ? 1 : 0;
        msg->is_error    = (fdf.can_id & CAN_ERR_FLAG) ? 1 : 0;
        msg->id          = fdf.can_id & (msg->is_extended ? CAN_EFF_MASK
                                                           : CAN_SFF_MASK);
        msg->dlc         = fdf.len;
        memcpy(msg->data, fdf.data, fdf.len);
    } else {
        /* Classic CAN frame */
        struct can_frame *cf = (struct can_frame *)&fdf;
        msg->is_fd       = 0;
        msg->is_extended = (cf->can_id & CAN_EFF_FLAG) ? 1 : 0;
        msg->is_remote   = (cf->can_id & CAN_RTR_FLAG) ? 1 : 0;
        msg->is_error    = (cf->can_id & CAN_ERR_FLAG) ? 1 : 0;
        msg->id          = cf->can_id & (msg->is_extended ? CAN_EFF_MASK
                                                           : CAN_SFF_MASK);
        msg->dlc         = cf->can_dlc;
        memcpy(msg->data, cf->data, cf->can_dlc);
    }
    msg->direction = CAN_DIR_RX;
    return DRV_CAN_OK;
}

int socketcan_get_stats(socketcan_ctx_t *ctx, can_stats_t *stats)
{
    (void)ctx;
    /* Stats are maintained by the application layer; just return OK */
    (void)stats;
    return DRV_CAN_OK;
}

int socketcan_set_filter(socketcan_ctx_t *ctx, uint32_t id,
                         uint32_t mask, int is_extended)
{
    if (!ctx || ctx->sock_fd < 0)
        return DRV_CAN_ERR_PARAM;

    struct can_filter filter;
    filter.can_id   = id | (is_extended ? CAN_EFF_FLAG : 0);
    filter.can_mask = mask | CAN_EFF_FLAG;

    if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filter, sizeof(filter)) < 0) {
        perror("socketcan: setsockopt CAN_RAW_FILTER");
        return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_clear_filter(socketcan_ctx_t *ctx)
{
    if (!ctx || ctx->sock_fd < 0)
        return DRV_CAN_ERR_PARAM;

    /* Install a single accept-all filter {id:0, mask:0}.  An *empty* filter
     * list (optlen 0) would make the socket receive nothing, so use a wildcard
     * match instead to genuinely accept every frame. */
    struct can_filter accept_all = { .can_id = 0, .can_mask = 0 };
    if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &accept_all, sizeof(accept_all)) < 0) {
        perror("socketcan: setsockopt clear filter");
        return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_reset(socketcan_ctx_t *ctx)
{
    if (!ctx || ctx->iface[0] == '\0')
        return DRV_CAN_ERR_PARAM;

    const char *down_args[] = {
        "ip", "link", "set", ctx->iface, "down", NULL
    };
    const char *up_args[] = {
        "ip", "link", "set", ctx->iface, "up", NULL
    };
    run_ip_priv(down_args, 1);
    run_ip_priv(up_args, 1);
    return DRV_CAN_OK;
}

const char *socketcan_error_string(int err)
{
    switch (err) {
    case DRV_CAN_OK:          return "OK";
    case DRV_CAN_ERR:         return "General error";
    case DRV_CAN_ERR_TIMEOUT: return "Timeout";
    case DRV_CAN_ERR_PARAM:   return "Invalid parameter";
    case DRV_CAN_ERR_INIT:    return "Initialization failed";
    default:                  return "Unknown error";
    }
}

int socketcan_list_interfaces(char ifnames[][SOCKETCAN_MAX_IFACE],
                               int max_count)
{
    int count = 0;
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        if (ent->d_name[0] == '.') continue;

        char path[300];
        snprintf(path, sizeof(path),
                 "/sys/class/net/%s/type", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        int type = 0;
        int n = fscanf(f, "%d", &type);
        fclose(f);

        if (n == 1 && type == 280 /* ARPHRD_CAN */) {
            size_t nl = strlen(ent->d_name);
            if (nl >= SOCKETCAN_MAX_IFACE) continue; /* name too long, skip */
            memcpy(ifnames[count], ent->d_name, nl + 1);
            count++;
        }
    }
    closedir(d);
    return count;
}
