/**
 * @file drv_can.c
 * @brief Generic CAN driver abstraction and the SocketCAN binding.
 *
 * @details
 * Implements the hardware-independent `drv_can_*` API declared in
 * @ref drv_can.h.  Each call dispatches through the currently selected
 * @ref can_driver_t vtable (set by @ref drv_can_init).  This file also defines
 * the built-in SocketCAN vtable, whose members are thin adapters that forward
 * to the `socketcan_*` functions while binding the shared @ref s_sck_ctx.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "../inc/drv_can.h"
#include "../inc/socketcan.h"

/**
 * @name SocketCAN vtable adapters
 * @brief Thin wrappers binding @ref s_sck_ctx to the `socketcan_*` calls.
 * @{
 */

/** @brief Per-process SocketCAN connection context. */
static socketcan_ctx_t s_sck_ctx;

static int sck_init(const char *iface, uint32_t bitrate,
                    uint32_t data_bitrate, int fd_mode, int listen_only,
                    const can_bit_timing_t *timing)
{
    return socketcan_open(&s_sck_ctx, iface, bitrate, data_bitrate,
                          fd_mode, listen_only, timing);
}

static int sck_deinit(void)
{
    return socketcan_close(&s_sck_ctx);
}

static int sck_send(const can_msg_t *msg)
{
    return socketcan_send(&s_sck_ctx, msg);
}

static int sck_recv(can_msg_t *msg, int timeout_ms)
{
    return socketcan_recv(&s_sck_ctx, msg, timeout_ms);
}

static int sck_get_stats(can_stats_t *stats)
{
    return socketcan_get_stats(&s_sck_ctx, stats);
}

static int sck_set_filter(uint32_t id, uint32_t mask, int is_extended)
{
    return socketcan_set_filter(&s_sck_ctx, id, mask, is_extended);
}

static int sck_clear_filter(void)
{
    return socketcan_clear_filter(&s_sck_ctx);
}

static int sck_reset(void)
{
    return socketcan_reset(&s_sck_ctx);
}

static const char *sck_error_string(int err)
{
    return socketcan_error_string(err);
}

/** @brief The built-in SocketCAN driver vtable. */
static can_driver_t s_socketcan_driver = {
    .init         = sck_init,
    .deinit       = sck_deinit,
    .send         = sck_send,
    .recv         = sck_recv,
    .get_stats    = sck_get_stats,
    .set_filter   = sck_set_filter,
    .clear_filter = sck_clear_filter,
    .reset        = sck_reset,
    .error_string = sck_error_string,
};
/** @} */

can_driver_t *drv_can_get_socketcan(void)
{
    return &s_socketcan_driver;
}

/** @brief Currently selected driver vtable, or NULL when disconnected. */
static can_driver_t *s_active_drv = NULL;

int drv_can_init(can_driver_t *drv, const char *iface, uint32_t bitrate,
                 uint32_t data_bitrate, int fd_mode, int listen_only,
                 const can_bit_timing_t *timing)
{
    if (!drv || !drv->init) return DRV_CAN_ERR_PARAM;
    s_active_drv = drv;
    return drv->init(iface, bitrate, data_bitrate, fd_mode, listen_only,
                     timing);
}

int drv_can_deinit(void)
{
    if (!s_active_drv || !s_active_drv->deinit) return DRV_CAN_ERR_INIT;
    int r = s_active_drv->deinit();
    s_active_drv = NULL;
    return r;
}

int drv_can_send(const can_msg_t *msg)
{
    if (!s_active_drv || !s_active_drv->send) return DRV_CAN_ERR_INIT;
    return s_active_drv->send(msg);
}

int drv_can_recv(can_msg_t *msg, int timeout_ms)
{
    if (!s_active_drv || !s_active_drv->recv) return DRV_CAN_ERR_INIT;
    return s_active_drv->recv(msg, timeout_ms);
}

int drv_can_get_stats(can_stats_t *stats)
{
    if (!s_active_drv || !s_active_drv->get_stats) return DRV_CAN_ERR_INIT;
    return s_active_drv->get_stats(stats);
}

int drv_can_set_filter(uint32_t id, uint32_t mask, int is_extended)
{
    if (!s_active_drv || !s_active_drv->set_filter) return DRV_CAN_ERR_INIT;
    return s_active_drv->set_filter(id, mask, is_extended);
}

int drv_can_clear_filter(void)
{
    if (!s_active_drv || !s_active_drv->clear_filter) return DRV_CAN_ERR_INIT;
    return s_active_drv->clear_filter();
}

int drv_can_reset(void)
{
    if (!s_active_drv || !s_active_drv->reset) return DRV_CAN_ERR_INIT;
    return s_active_drv->reset();
}

const char *drv_can_error_string(int err)
{
    if (s_active_drv && s_active_drv->error_string)
        return s_active_drv->error_string(err);
    return socketcan_error_string(err);
}
