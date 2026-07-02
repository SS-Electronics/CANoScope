/**
 * @file socketcan.h
 * @brief SocketCAN back-end interface (Linux `PF_CAN` / `SOCK_RAW`).
 *
 * @details
 * Declares the per-connection context (@ref socketcan_ctx_t) and the low-level
 * SocketCAN operations that implement the generic @ref can_driver_t vtable.
 * Interface bring-up/configuration is performed via the `ip` utility (elevated
 * through `pkexec` when required), avoiding a bespoke Netlink implementation.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef SOCKETCAN_H
#define SOCKETCAN_H

#include <stdint.h>
#include "can_message.h"
#include "drv_can.h"

#define SOCKETCAN_MAX_IFACE 32  /**< Max interface-name buffer size (incl. NUL). */

/**
 * @brief State for one open SocketCAN connection.
 */
typedef struct {
    int      sock_fd;       /**< Raw CAN socket fd, or -1 when closed.         */
    int      fd_enabled;    /**< Non-zero if CAN FD frames are enabled.        */
    int      listen_only;   /**< Non-zero if opened in listen-only mode.       */
    char     iface[SOCKETCAN_MAX_IFACE]; /**< Interface name (e.g. "vcan0").   */
    uint32_t bitrate;       /**< Nominal bit rate (bit/s).                     */
    uint32_t data_bitrate;  /**< CAN FD data bit rate (bit/s).                 */
    can_bit_timing_t timing;/**< Optional manual bit-timing configuration.     */
} socketcan_ctx_t;

/**
 * @brief Open and configure a SocketCAN interface.
 * @param ctx          Context to initialise (zeroed on entry).
 * @param iface        Interface name; `vcan*` interfaces are auto-created/up'd.
 * @param bitrate      Nominal bit rate (bit/s).
 * @param data_bitrate CAN FD data bit rate (bit/s); 0 if unused.
 * @param fd_mode      Non-zero to request CAN FD.
 * @param listen_only  Non-zero for passive monitoring.
 * @param timing       Optional manual bit timing (may be NULL).
 * @return #DRV_CAN_OK or a negative `DRV_CAN_ERR_*` code.
 */
int  socketcan_open(socketcan_ctx_t *ctx, const char *iface,
                    uint32_t bitrate, uint32_t data_bitrate,
                    int fd_mode, int listen_only,
                    const can_bit_timing_t *timing);

/** @brief Close the socket (and bring real interfaces down). @param ctx Context. @return Status. */
int  socketcan_close(socketcan_ctx_t *ctx);

/** @brief Write one frame to the bus. @param ctx Context. @param msg Frame. @return Status. */
int  socketcan_send(socketcan_ctx_t *ctx, const can_msg_t *msg);

/**
 * @brief Read one frame with a timeout.
 * @param ctx        Context.
 * @param msg        Output frame.
 * @param timeout_ms Timeout in milliseconds (<0 blocks).
 * @return #DRV_CAN_OK, #DRV_CAN_ERR_TIMEOUT, or another code.
 */
int  socketcan_recv(socketcan_ctx_t *ctx, can_msg_t *msg, int timeout_ms);

/** @brief Statistics hook (app-maintained; returns OK). @param ctx Context. @param stats Output. @return Status. */
int  socketcan_get_stats(socketcan_ctx_t *ctx, can_stats_t *stats);

/**
 * @brief Install a `CAN_RAW_FILTER` acceptance filter.
 * @param ctx         Context.
 * @param id          Filter id.
 * @param mask        Filter mask.
 * @param is_extended Non-zero for extended IDs.
 * @return Status.
 */
int  socketcan_set_filter(socketcan_ctx_t *ctx, uint32_t id,
                          uint32_t mask, int is_extended);

/** @brief Clear all acceptance filters. @param ctx Context. @return Status. */
int  socketcan_clear_filter(socketcan_ctx_t *ctx);

/** @brief Bounce the interface down/up. @param ctx Context. @return Status. */
int  socketcan_reset(socketcan_ctx_t *ctx);

/** @brief Map a `DRV_CAN_*` code to text. @param err Code. @return Static string. */
const char *socketcan_error_string(int err);

/**
 * @brief Enumerate available CAN-type interfaces from `/sys/class/net`.
 * @param ifnames    Output array of name buffers.
 * @param max_count  Capacity of @p ifnames.
 * @return Number of interfaces written.
 */
int socketcan_list_interfaces(char ifnames[][SOCKETCAN_MAX_IFACE],
                              int max_count);

#endif /* SOCKETCAN_H */
