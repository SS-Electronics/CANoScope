/**
 * @file drv_can.h
 * @brief Generic CAN driver abstraction (vtable) and thin wrapper API.
 *
 * @details
 * Declares the back-end-agnostic CAN driver interface used by the rest of the
 * application.  A concrete back-end (currently SocketCAN, see
 * `driver/socketcan.c`) provides a @ref can_driver_t function-pointer table; the
 * `drv_can_*` wrapper functions in `driver/drv_can.c` dispatch to the currently
 * selected driver.  Adding a new back-end (e.g. PCAN-Basic) only requires
 * implementing the vtable and passing it to @ref drv_can_init.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DRV_CAN_H
#define DRV_CAN_H

#include <stdint.h>
#include "can_message.h"

#define DRV_CAN_OK          0   /**< Operation succeeded.                     */
#define DRV_CAN_ERR        -1   /**< Generic failure.                         */
#define DRV_CAN_ERR_TIMEOUT -2  /**< Operation timed out.                     */
#define DRV_CAN_ERR_PARAM  -3   /**< Invalid argument.                        */
#define DRV_CAN_ERR_INIT   -4   /**< Initialisation failed.                   */

/** @brief Standard nominal CAN bit rates (bit/s). */
typedef enum {
    CAN_BAUD_10K   =   10000,
    CAN_BAUD_20K   =   20000,
    CAN_BAUD_50K   =   50000,
    CAN_BAUD_100K  =  100000,
    CAN_BAUD_125K  =  125000,
    CAN_BAUD_250K  =  250000,
    CAN_BAUD_500K  =  500000,
    CAN_BAUD_800K  =  800000,
    CAN_BAUD_1M    = 1000000,
} can_baud_t;

/** @brief Standard CAN FD data-phase bit rates (bit/s). */
typedef enum {
    CAN_FD_BAUD_1M  =  1000000,
    CAN_FD_BAUD_2M  =  2000000,
    CAN_FD_BAUD_4M  =  4000000,
    CAN_FD_BAUD_5M  =  5000000,
    CAN_FD_BAUD_8M  =  8000000,
    CAN_FD_BAUD_10M = 10000000,
} can_fd_baud_t;

/**
 * @brief Optional manual CAN bit-timing parameters.
 *
 * When @ref enabled is non-zero these segment values are passed to the kernel
 * instead of letting it derive timing from the bit rate alone.
 */
typedef struct {
    int      enabled;       /**< Use these manual timing values when non-zero. */
    uint32_t brp;           /**< Bit-rate prescaler / time quantum.           */
    uint32_t prop_seg;      /**< Propagation segment.                         */
    uint32_t phase_seg1;    /**< Phase segment 1.                             */
    uint32_t phase_seg2;    /**< Phase segment 2.                             */
    uint32_t sjw;           /**< Synchronisation jump width.                  */
    uint32_t sample_point;  /**< Sample point (per-mille).                    */
} can_bit_timing_t;

/**
 * @brief CAN driver vtable — implemented by each concrete back-end.
 *
 * Every member is a function pointer mirroring one `drv_can_*` wrapper.
 */
typedef struct can_driver {
    /** Open/configure the interface. @see drv_can_init */
    int         (*init)(const char *iface, uint32_t bitrate,
                        uint32_t data_bitrate, int fd_mode,
                        int listen_only,
                        const can_bit_timing_t *timing);
    int         (*deinit)(void);                       /**< Close the interface. */
    int         (*send)(const can_msg_t *msg);         /**< Transmit one frame.  */
    int         (*recv)(can_msg_t *msg, int timeout_ms);/**< Receive one frame.  */
    int         (*get_stats)(can_stats_t *stats);      /**< Query statistics.    */
    int         (*set_filter)(uint32_t id, uint32_t mask, int is_extended); /**< Install an acceptance filter. */
    int         (*clear_filter)(void);                 /**< Remove all filters.  */
    int         (*reset)(void);                        /**< Reset the interface. */
    const char *(*error_string)(int err);              /**< Map a code to text.  */
} can_driver_t;

/**
 * @brief Return the built-in SocketCAN driver vtable singleton.
 * @return Pointer to a statically allocated @ref can_driver_t.
 */
struct can_driver *drv_can_get_socketcan(void);

/**
 * @brief Select a driver and open/configure a CAN interface.
 * @param drv          Driver vtable to use (e.g. from @ref drv_can_get_socketcan).
 * @param iface        Interface name (e.g. `"can0"`, `"vcan0"`).
 * @param bitrate      Nominal bit rate in bit/s.
 * @param data_bitrate CAN FD data-phase bit rate in bit/s (0 if unused).
 * @param fd_mode      Non-zero to enable CAN FD.
 * @param listen_only  Non-zero for passive (no-ACK) monitoring.
 * @param timing       Optional manual bit timing (may be NULL).
 * @return #DRV_CAN_OK on success or a negative `DRV_CAN_ERR_*` code.
 */
int drv_can_init(can_driver_t *drv, const char *iface, uint32_t bitrate,
                 uint32_t data_bitrate, int fd_mode, int listen_only,
                 const can_bit_timing_t *timing);

/** @brief Close the active interface and release driver resources. @return Status code. */
int drv_can_deinit(void);

/** @brief Transmit a single frame. @param msg Frame to send. @return Status code. */
int drv_can_send(const can_msg_t *msg);

/**
 * @brief Receive a single frame, blocking up to @p timeout_ms.
 * @param msg         Output frame.
 * @param timeout_ms  Timeout in milliseconds (<0 to block indefinitely).
 * @return #DRV_CAN_OK, #DRV_CAN_ERR_TIMEOUT, or another `DRV_CAN_ERR_*` code.
 */
int drv_can_recv(can_msg_t *msg, int timeout_ms);

/** @brief Query driver statistics. @param stats Output snapshot. @return Status code. */
int drv_can_get_stats(can_stats_t *stats);

/**
 * @brief Install a hardware acceptance filter.
 * @param id          Filter identifier.
 * @param mask        Filter mask.
 * @param is_extended Non-zero for a 29-bit filter.
 * @return Status code.
 */
int drv_can_set_filter(uint32_t id, uint32_t mask, int is_extended);

/** @brief Remove all acceptance filters (accept everything). @return Status code. */
int drv_can_clear_filter(void);

/** @brief Reset the CAN controller/interface. @return Status code. */
int drv_can_reset(void);

/** @brief Translate a `DRV_CAN_*` code to a human-readable string. @param err Code. @return Static string. */
const char *drv_can_error_string(int err);

#endif /* DRV_CAN_H */
