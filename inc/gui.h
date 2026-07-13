/**
 * @file gui.h
 * @brief GTK GUI widget registry and the application's GUI/thread API.
 *
 * @details
 * Declares the trace-view column layout, the global widget bundle
 * (@ref gui_widgets_t / @ref g_gui), and every public function implemented by
 * the `gui/` sources: window construction, trace/statistics updates, dialogs,
 * value formatting, the RX/TX/stats thread entry points, and the
 * connect/disconnect lifecycle helpers.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef GUI_H
#define GUI_H

#include <stddef.h>
#include <gtk/gtk.h>
#include "can_message.h"
#include "app_state.h"
#include "dbc.h"

/**
 * @brief Column indices of the trace `GtkListStore`.
 *
 * Columns up to #TCOL_COUNT are visible; the remainder are hidden helper
 * columns used for sorting, row roll-up, and re-formatting.
 */
enum {
    TCOL_SEQ = 0,   /**< Sequence number.                                     */
    TCOL_DIR,       /**< Direction ("Rx"/"Tx").                               */
    TCOL_TIME,      /**< Formatted timestamp.                                 */
    TCOL_ID,        /**< Formatted CAN identifier.                            */
    TCOL_TYPE,      /**< Frame type string.                                   */
    TCOL_DLC,       /**< Data length code.                                    */
    TCOL_DATA,      /**< Formatted payload.                                   */
    TCOL_INTERVAL,  /**< Inter-frame interval (ms).                           */
    TCOL_FREQ,      /**< Frame frequency (Hz).                                */
    TCOL_COUNT,     /**< Hit count for rolled-up rows.                        */
    TCOL_FG,        /**< Row foreground colour (markup).                      */
    TCOL_ID_RAW,    /**< Hidden numeric ID for sorting / reformatting.        */
    TCOL_EXT_RAW,   /**< Hidden extended-ID flag.                             */
    TCOL_DIR_RAW,   /**< Hidden direction flag for row roll-up.               */
    TCOL_DATA_RAW,  /**< Hidden compact hex data for reformatting.            */
    TCOL_TS_NS,     /**< Hidden last timestamp in nanoseconds.                */
    TCOL_NUM        /**< Number of columns (model width).                     */
};

#define GUI_TRACE_ID_TEXT_MAX   16
#define GUI_TRACE_DATA_TEXT_MAX 512

/**
 * @brief Snapshot of one RX row from the main trace table.
 */
typedef struct {
    uint32_t     id;           /**< Raw 11-/29-bit identifier.                */
    uint8_t      is_extended;  /**< Non-zero for a 29-bit extended ID.        */
    uint8_t      dlc;          /**< Payload length.                           */
    uint8_t      data[CANFD_DATA_MAX]; /**< Latest payload bytes.             */
    unsigned int count;        /**< Trace row hit count.                      */
    char         id_text[GUI_TRACE_ID_TEXT_MAX];     /**< Display ID text.    */
    char         data_text[GUI_TRACE_DATA_TEXT_MAX]; /**< Display payload.    */
} gui_trace_rx_message_t;

/**
 * @brief Bundle of long-lived widgets, populated by @ref gui_create_main_window.
 */
typedef struct {
    GtkWidget    *window;              /**< Top-level application window.      */
    GtkWidget    *toolbar_connect;     /**< Connect tool button.              */
    GtkWidget    *toolbar_disconnect;  /**< Disconnect tool button.          */
    GtkWidget    *toolbar_clear;       /**< Clear-trace tool button.         */
    GtkWidget    *toolbar_trace_start; /**< Start-trace tool button.         */
    GtkWidget    *toolbar_trace_stop;  /**< Stop-trace tool button.          */
    GtkWidget    *trace_view;          /**< Trace `GtkTreeView`.             */
    GtkListStore *trace_store;         /**< Trace backing model.             */
    guint         trace_row_seq;       /**< Compact #-column counter (unique
                                            rows; reset by gui_clear_trace).  */
    GtkWidget    *lbl_connection;      /**< Stats bar: interface name.       */
    GtkWidget    *lbl_bitrate;         /**< Stats bar: bit rate.             */
    GtkWidget    *lbl_stats_summary;   /**< Optional one-line summary label. */
    GtkWidget    *lbl_bus_load;        /**< Stats bar: bus-load text.        */
    GtkWidget    *lbl_rx;              /**< Stats bar: Rx count.             */
    GtkWidget    *lbl_tx;              /**< Stats bar: Tx count.             */
    GtkWidget    *lbl_err;             /**< Stats bar: error count.          */
    GtkWidget    *lbl_bus_state;       /**< Stats bar: bus state.            */
    GtkWidget    *progress_bus_load;   /**< Stats bar: bus-load progress bar.*/
    GtkWidget    *statusbar;           /**< Bottom status bar.               */
    guint         statusbar_ctx;       /**< Status-bar context id.           */
    GtkWidget    *sig_view;            /**< Signal Analysis `GtkTreeView`.   */
    GtkListStore *sig_store;           /**< Signal Analysis backing model.   */
    GtkWidget    *lbl_dbc;             /**< Signal tab: loaded-database label.*/
} gui_widgets_t;

/** @brief The process-wide GUI widget registry (defined in threads.c). */
extern gui_widgets_t g_gui;

/* ---- Main window ---------------------------------------------------------- */

/**
 * @brief Build the main application window and all of its widgets.
 * @param app  The owning GtkApplication.
 * @return The newly created top-level window.
 */
GtkWidget *gui_create_main_window(GtkApplication *app);

/**
 * @brief Sync the transmit panel with the current CAN FD link mode.
 *
 * Widens each row's DLC range to 64 in CAN FD mode (8 in classic) and refreshes
 * the number of visible data-byte fields.
 */
void gui_tx_panel_update_fd(void);

/* ---- Trace / statistics --------------------------------------------------- */

/** @brief Append (or roll up) one frame into the trace view. @param msg Frame. */
void gui_add_message(const can_msg_t *msg);

/** @brief Remove every row from the trace view. */
void gui_clear_trace(void);

/** @brief Re-render existing trace rows after a display-format change. */
void gui_refresh_trace_display(void);

/**
 * @brief Collect current RX rows from the Receive/Transmit trace.
 * @param out  Optional output array. Pass NULL to count only.
 * @param max  Capacity of @p out.
 * @return Total RX rows available, even when greater than @p max.
 */
size_t gui_trace_collect_rx_messages(gui_trace_rx_message_t *out, size_t max);

/* ---- Signal Analysis (DBC-decoded) ---------------------------------------- */

/**
 * @brief Build the Signal Analysis tab (live DBC-decoded signal table).
 * @return A container widget for use as a notebook page.
 */
GtkWidget *gui_create_signal_view(void);

/**
 * @brief Load (or replace) the active CAN database for signal decoding.
 * @param path  Path to a `.dbc` file.
 * @return TRUE on success; on failure a status message is shown and the
 *         previous database (if any) is kept.
 */
gboolean gui_signal_load_dbc(const char *path);

/** @brief Auto-load the bundled demo database if one is found. */
void gui_signal_load_default_dbc(void);

/** @brief Forget the active database and clear the signal table. */
void gui_signal_clear_dbc(void);

/**
 * @brief Decode one frame against the active database and update the signal rows.
 * @param msg  Frame (RX or TX); ignored when no database is loaded.
 */
void gui_signal_decode_message(const can_msg_t *msg);

/**
 * @brief Return the active DBC path used by Signal Analysis, or NULL.
 */
const char *gui_signal_get_dbc_path(void);

/* ---- DB Creation ---------------------------------------------------------- */

/**
 * @brief Build the DB Creation tab for creating/updating DBC files from RX rows.
 * @return A container widget for use as a notebook page.
 */
GtkWidget *gui_create_db_creation_view(void);

/**
 * @brief Notify the DB Creation tab that the trace RX list may have changed.
 */
void gui_db_creation_trace_changed(void);

/* ---- Signal Analysis Viewer (signal-vs-time graph) ------------------------ */

/**
 * @brief Build the Signal Analysis Viewer tab (multi-signal time graph).
 * @return A container widget for use as a notebook page.
 */
GtkWidget *gui_create_signal_plot(void);

/**
 * @brief Rebuild the plot's series list from a database (or clear it).
 * @param db  Active database, or NULL to remove all series.
 */
void gui_plot_set_database(const dbc_db_t *db);

/**
 * @brief Append a decoded sample to a plot series (timestamped "now").
 * @param id       Raw CAN identifier.
 * @param is_ext   Non-zero for an extended-ID message.
 * @param sig_idx  Signal index within its message.
 * @param value    Physical value.
 */
void gui_plot_add_sample(uint32_t id, int is_ext, int sig_idx, double value);

/** @brief Refresh the statistics bar from @ref g_app counters. */
void gui_update_stats(void);

/**
 * @brief Set the status-bar text (printf-style).
 * @param fmt  printf format string.
 * @param ...  Format arguments.
 */
void gui_status_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Dialogs -------------------------------------------------------------- */

/** @brief Show the connection-settings dialog. @param parent Transient parent. */
void gui_show_settings_dialog(GtkWidget *parent);

/** @brief Show the advanced transmit window. */
void gui_show_transmit_window(void);

/** @brief Show the About dialog (Taksys branding). @param parent Transient parent. */
void gui_show_about_dialog(GtkWidget *parent);

/**
 * @brief Show a modal error dialog.
 * @param parent  Transient parent (may be NULL).
 * @param title   Window title.
 * @param msg     Message body.
 */
void gui_show_error(GtkWidget *parent, const char *title, const char *msg);

/* ---- Formatting helpers --------------------------------------------------- */

/**
 * @brief Format a CAN identifier into @p buf per the active display radix.
 * @param buf     Output buffer.
 * @param sz      Buffer size.
 * @param id      Identifier value.
 * @param is_ext  Non-zero for an extended (29-bit) ID.
 */
void        gui_format_id(char *buf, size_t sz, uint32_t id, int is_ext);

/**
 * @brief Format payload bytes into @p buf per the active data format.
 * @param buf  Output buffer.
 * @param sz   Buffer size.
 * @param d    Payload bytes.
 * @param dlc  Number of bytes.
 */
void        gui_format_data(char *buf, size_t sz, const uint8_t *d, uint8_t dlc);

/** @brief Return a short frame-type label ("Std", "Ext", "FD+BRS", …). @param m Frame. @return Static string. */
const char *gui_msg_type_str(const can_msg_t *m);

/** @brief Return a bus-state label for a `CAN_BUS_*` value. @param st State. @return Static string. */
const char *gui_bus_state_str(uint8_t st);

/** @brief Return a Pango colour name for a `CAN_BUS_*` value. @param st State. @return Static string. */
const char *gui_bus_state_color(uint8_t st);

/* ---- Worker-thread entry points ------------------------------------------- */

/** @brief Receive-thread entry point. @param arg Unused. @return NULL. */
void *thread_rx(void *arg);

/** @brief Transmit-thread entry point. @param arg Unused. @return NULL. */
void *thread_tx(void *arg);

/** @brief Statistics-thread entry point. @param arg Unused. @return NULL. */
void *thread_stats(void *arg);

/* ---- Connect / disconnect lifecycle --------------------------------------- */

/** @brief Open the interface in @ref g_app and start the worker threads. */
void app_do_connect(void);

/** @brief Stop the worker threads and close the interface. */
void app_do_disconnect(void);

#endif /* GUI_H */
