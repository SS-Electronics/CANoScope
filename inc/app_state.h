/**
 * @file app_state.h
 * @brief Global application state shared across the GUI and worker threads.
 *
 * @details
 * Declares the singleton @ref app_state_t that holds the active connection
 * parameters, live statistics counters, worker-thread handles and run flags,
 * the RX/TX message queues, the in-memory trace-capture buffer, and display
 * preferences.  GLib container types are forward-declared so the driver layer
 * can include this header without pulling in GTK.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "can_message.h"
#include "drv_can.h"

/* Forward-declare GLib/GTK types so driver files don't need GTK headers */
typedef struct _GAsyncQueue GAsyncQueue; /**< Opaque GLib async queue.        */
typedef struct _GHashTable  GHashTable;  /**< Opaque GLib hash table.          */

#define APP_MAX_IFACE_LEN   32  /**< Max interface-name length in app state.  */
#define APP_TRACE_FILE_LEN 256  /**< Max trace-file path length.              */

/** @brief CAN identifier display radix. */
typedef enum {
    APP_ID_FORMAT_HEX = 0,  /**< Hexadecimal IDs. */
    APP_ID_FORMAT_DEC = 1,  /**< Decimal IDs.     */
} app_id_format_t;

/** @brief Payload-byte display format. */
typedef enum {
    APP_DATA_FORMAT_HEX   = 0,  /**< Hexadecimal bytes. */
    APP_DATA_FORMAT_DEC   = 1,  /**< Decimal bytes.     */
    APP_DATA_FORMAT_ASCII = 2,  /**< ASCII rendering.   */
} app_data_format_t;

/**
 * @brief Process-wide application state singleton (see @ref g_app).
 */
typedef struct {
    /** @name Connection */
    /**@{*/
    volatile int  connected;             /**< Non-zero while a link is open.   */
    char          iface[APP_MAX_IFACE_LEN]; /**< Selected interface name.       */
    uint32_t      bitrate;               /**< Nominal bit rate (bit/s).         */
    uint32_t      data_bitrate;          /**< CAN FD data bit rate (bit/s).     */
    int           fd_mode;               /**< Non-zero when CAN FD is enabled.  */
    int           listen_only;           /**< Non-zero for listen-only mode.    */
    can_bit_timing_t bit_timing;         /**< Optional manual bit timing.       */
    int           filter_enabled;        /**< Apply the acceptance filter below.*/
    uint32_t      filter_id;             /**< Acceptance-filter identifier.     */
    uint32_t      filter_mask;           /**< Acceptance-filter mask.           */
    int           filter_ext;            /**< Filter matches extended (29-bit). */
    /**@}*/

    /** @name Statistics */
    /**@{*/
    volatile uint64_t rx_count;          /**< Received frame counter.           */
    volatile uint64_t tx_count;          /**< Transmitted frame counter.        */
    volatile uint64_t error_count;       /**< Error frame counter.              */
    volatile double   bus_load;          /**< Latest bus-load estimate (%).     */
    volatile uint8_t  bus_state;         /**< Latest `CAN_BUS_*` state.         */
    volatile uint64_t msg_seq;           /**< Monotonic frame sequence source.  */
    volatile uint64_t period_bits;       /**< Bits seen this stats period.      */
    /**@}*/

    /** @name Worker threads */
    /**@{*/
    pthread_t    rx_thread;              /**< Receive thread handle.            */
    pthread_t    tx_thread;              /**< Transmit thread handle.           */
    pthread_t    stats_thread;           /**< Statistics thread handle.         */
    volatile int rx_running;             /**< RX thread run flag.               */
    volatile int tx_running;             /**< TX thread run flag.               */
    volatile int stats_running;          /**< Stats thread run flag.            */
    volatile int shutting_down;          /**< Set during teardown to drop late callbacks. */
    /**@}*/

    /** @name Message queues (GLib `GAsyncQueue` of `can_msg_t*`) */
    /**@{*/
    GAsyncQueue *rx_queue;               /**< Pending received frames.          */
    GAsyncQueue *tx_queue;               /**< Frames awaiting transmission.     */
    /**@}*/

    /** @name Trace capture (Trace menu: Start/Stop/Save CSV) */
    /**@{*/
    volatile int    tracing;             /**< Non-zero while capturing.         */
    FILE           *trace_file;          /**< Legacy file handle (unused/NULL). */
    char            trace_filename[APP_TRACE_FILE_LEN]; /**< Last save path.     */
    pthread_mutex_t trace_mutex;         /**< Guards the capture buffer.        */
    can_msg_t      *trace_buf;           /**< Dynamically grown capture buffer. */
    size_t          trace_len;           /**< Frames currently captured.        */
    size_t          trace_cap;           /**< Allocated capacity (frames).      */
    /**@}*/

    /** @name Display options / deduplication */
    /**@{*/
    int             dedup_mode;          /**< Roll up repeated TX IDs.          */
    int             id_format;           /**< @ref app_id_format_t value.       */
    int             data_format;         /**< @ref app_data_format_t value.     */
    pthread_mutex_t dedup_mutex;         /**< Guards @ref dedup_table.          */
    GHashTable     *dedup_table;         /**< Maps CAN id → row index.          */
    /**@}*/

    can_driver_t   *driver;              /**< Active CAN driver vtable.         */
} app_state_t;

/** @brief The process-wide application state singleton (defined in threads.c). */
extern app_state_t g_app;

/** @brief Initialise @ref g_app (queues, mutexes, default settings). */
void app_state_init(void);

/** @brief Disconnect if needed and release all @ref g_app resources. */
void app_state_cleanup(void);

#endif /* APP_STATE_H */
