/**
 * @file can_message.h
 * @brief Hardware-independent CAN/CAN FD message and statistics data types.
 *
 * @details
 * Defines the in-application representation of a CAN frame (@ref can_msg_t) and
 * an aggregate bus-statistics snapshot (@ref can_stats_t).  These types are the
 * common currency exchanged between the SocketCAN driver layer
 * (`driver/socketcan.c`), the worker threads (`gui/threads.c`), and the GTK
 * presentation layer (`gui/message_view.c`).  They are deliberately free of any
 * GTK or kernel dependency so the driver can be reused in isolation.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef CAN_MESSAGE_H
#define CAN_MESSAGE_H

#include <stdint.h>
#include <time.h>

#define CAN_MAX_DLC      8   /**< Maximum payload of a classic CAN frame.      */
#define CANFD_DATA_MAX  64   /**< Maximum payload of a CAN FD frame.           */

#define CAN_DIR_RX      0    /**< Frame direction: received.                   */
#define CAN_DIR_TX      1    /**< Frame direction: transmitted.                */

#define CAN_BUS_ACTIVE  0    /**< Error-active bus state.                      */
#define CAN_BUS_WARNING 1    /**< Error-warning bus state.                     */
#define CAN_BUS_PASSIVE 2    /**< Error-passive bus state.                     */
#define CAN_BUS_OFF     3    /**< Bus-off state.                               */

/**
 * @brief A single CAN or CAN FD frame as handled by the application.
 *
 * Carries both classic and FD-specific fields; the @ref is_fd flag selects the
 * interpretation.  Up to #CANFD_DATA_MAX payload bytes are stored inline.
 */
typedef struct {
    uint32_t        id;          /**< CAN identifier (11- or 29-bit).          */
    uint8_t         dlc;         /**< Payload length in bytes (0..64).         */
    uint8_t         data[CANFD_DATA_MAX]; /**< Payload bytes.                  */
    struct timespec timestamp;   /**< Capture time (CLOCK_REALTIME).           */
    uint8_t         is_extended; /**< Non-zero for a 29-bit extended ID.       */
    uint8_t         is_remote;   /**< Non-zero for a remote (RTR) frame.       */
    uint8_t         is_error;    /**< Non-zero for an error frame.             */
    uint8_t         is_fd;       /**< Non-zero for a CAN FD frame.             */
    uint8_t         is_brs;      /**< CAN FD bit-rate-switch flag.             */
    uint8_t         is_esi;      /**< CAN FD error-state-indicator flag.       */
    uint8_t         direction;   /**< #CAN_DIR_RX or #CAN_DIR_TX.              */
    uint64_t        seq;         /**< Monotonic application sequence number.   */
} can_msg_t;

/**
 * @brief Aggregate bus statistics snapshot.
 */
typedef struct {
    uint64_t rx_count;     /**< Total received frames.                        */
    uint64_t tx_count;     /**< Total transmitted frames.                     */
    uint64_t error_count;  /**< Total error frames.                           */
    double   bus_load;     /**< Estimated bus load, percent (0..100).         */
    uint8_t  bus_state;    /**< One of the `CAN_BUS_*` states.                */
} can_stats_t;

#endif /* CAN_MESSAGE_H */
