/**
 * @file threads.c
 * @brief Background worker threads, global state, and connect/disconnect logic.
 *
 * @details
 * Defines the application singletons @ref g_app and @ref g_gui and their
 * lifecycle (@ref app_state_init / @ref app_state_cleanup).  Hosts the three
 * worker threads — @ref thread_rx, @ref thread_tx, and @ref thread_stats — which
 * communicate results back to the GTK main thread via `gdk_threads_add_idle()`.
 * Also implements the in-memory trace capture (@ref trace_record) and the
 * @ref app_do_connect / @ref app_do_disconnect lifecycle that starts and joins
 * the threads.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "../inc/app_state.h"
#include "../inc/drv_can.h"
#include "../inc/gui.h"
#include "../inc/can_message.h"

/* ------------------------------------------------------------------ */
/* Global singletons                                                    */
/* ------------------------------------------------------------------ */

app_state_t   g_app; /**< Global application state singleton.   */
gui_widgets_t g_gui; /**< Global GUI widget registry singleton.  */

/* ------------------------------------------------------------------ */
/* State lifecycle                                                      */
/* ------------------------------------------------------------------ */

void app_state_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    memset(&g_gui, 0, sizeof(g_gui));

    pthread_mutex_init(&g_app.trace_mutex, NULL);
    pthread_mutex_init(&g_app.dedup_mutex, NULL);

    g_app.rx_queue     = g_async_queue_new_full(free);
    g_app.tx_queue     = g_async_queue_new_full(free);
    g_app.bitrate      = 500000;
    g_app.data_bitrate = 2000000;
    g_app.id_format    = APP_ID_FORMAT_HEX;
    g_app.data_format  = APP_DATA_FORMAT_HEX;
    strncpy(g_app.iface, "vcan0", APP_MAX_IFACE_LEN - 1);
}

void app_state_cleanup(void)
{
    if (g_app.connected)
        app_do_disconnect();

    if (g_app.rx_queue) {
        g_async_queue_unref(g_app.rx_queue);
        g_app.rx_queue = NULL;
    }
    if (g_app.tx_queue) {
        g_async_queue_unref(g_app.tx_queue);
        g_app.tx_queue = NULL;
    }
    if (g_app.trace_file) {
        fclose(g_app.trace_file);
        g_app.trace_file = NULL;
    }
    if (g_app.trace_buf) {
        free(g_app.trace_buf);
        g_app.trace_buf = NULL;
        g_app.trace_len = g_app.trace_cap = 0;
    }
    if (g_app.dedup_table) {
        g_hash_table_destroy(g_app.dedup_table);
        g_app.dedup_table = NULL;
    }

    pthread_mutex_destroy(&g_app.trace_mutex);
    pthread_mutex_destroy(&g_app.dedup_mutex);
}

/* ------------------------------------------------------------------ */
/* Idle callbacks (posted from worker threads to GTK main thread)      */
/* ------------------------------------------------------------------ */

/**
 * @brief Idle callback: hand a received frame to the trace view.
 * @param data Heap `can_msg_t*` (freed here).
 * @return G_SOURCE_REMOVE (one-shot).
 */
static gboolean idle_add_message(gpointer data)
{
    can_msg_t *msg = (can_msg_t *)data;
    if (!g_app.shutting_down && g_gui.trace_store)
        gui_add_message(msg);
    free(msg);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Idle callback: refresh the statistics bar on the GTK thread.
 * @param data Unused.
 * @return G_SOURCE_REMOVE (one-shot).
 */
static gboolean idle_update_stats(gpointer data)
{
    (void)data;
    if (!g_app.shutting_down && g_gui.lbl_rx)
        gui_update_stats();
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Trace capture (in-memory; exported to CSV via Trace > Save)          */
/* ------------------------------------------------------------------ */

#define TRACE_BUF_MAX 2000000u   /**< Hard cap so a long run can't exhaust RAM. */

/**
 * @brief Append one frame to the in-memory trace buffer.
 *
 * Called from the RX and TX worker threads, so it is guarded by
 * `g_app.trace_mutex` (shared with the CSV writer on the GTK thread). The buffer
 * grows geometrically and stops at #TRACE_BUF_MAX frames.
 *
 * @param msg  Frame to capture (copied by value).
 */
static void trace_record(const can_msg_t *msg)
{
    if (!g_app.tracing) return;

    pthread_mutex_lock(&g_app.trace_mutex);
    if (g_app.tracing && g_app.trace_len < TRACE_BUF_MAX) {
        if (g_app.trace_len >= g_app.trace_cap) {
            size_t ncap = g_app.trace_cap ? g_app.trace_cap * 2 : 4096;
            can_msg_t *nb = realloc(g_app.trace_buf,
                                    ncap * sizeof(can_msg_t));
            if (nb) {
                g_app.trace_buf = nb;
                g_app.trace_cap = ncap;
            }
        }
        if (g_app.trace_len < g_app.trace_cap)
            g_app.trace_buf[g_app.trace_len++] = *msg;
    }
    pthread_mutex_unlock(&g_app.trace_mutex);
}

/* ------------------------------------------------------------------ */
/* RX thread                                                            */
/* ------------------------------------------------------------------ */

void *thread_rx(void *arg)
{
    (void)arg;

    while (g_app.rx_running) {
        can_msg_t msg;
        int ret = drv_can_recv(&msg, 100);

        if (ret != DRV_CAN_OK) continue;

        msg.seq       = __atomic_add_fetch(&g_app.msg_seq, 1,
                                           __ATOMIC_SEQ_CST);
        msg.direction = CAN_DIR_RX;

        if (msg.is_error) {
            __atomic_add_fetch(&g_app.error_count, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&g_app.rx_count, 1, __ATOMIC_SEQ_CST);
            /* Accumulate bits for bus-load calculation */
            uint64_t frame_bits = (uint64_t)(msg.is_extended ? 67u : 47u)
                                + (uint64_t)msg.dlc * 8u;
            __atomic_add_fetch(&g_app.period_bits, frame_bits,
                               __ATOMIC_SEQ_CST);
        }

        /* Deliver to GUI */
        can_msg_t *copy = malloc(sizeof(can_msg_t));
        if (copy) {
            *copy = msg;
            gdk_threads_add_idle(idle_add_message, copy);
        }

        trace_record(&msg);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* TX thread                                                            */
/* ------------------------------------------------------------------ */

void *thread_tx(void *arg)
{
    (void)arg;

    while (g_app.tx_running) {
        /* Wait up to 100 ms for a message to appear.  g_async_queue_timeout_pop()
         * takes a RELATIVE timeout in microseconds (not an absolute time); using
         * an absolute monotonic time here made the thread block for ~uptime and
         * never observe tx_running == 0, hanging app_do_disconnect's join (which
         * froze the app on close/disconnect). */
        can_msg_t *msg = (can_msg_t *)
            g_async_queue_timeout_pop(g_app.tx_queue,
                                      100 * G_TIME_SPAN_MILLISECOND);

        if (!msg) continue;

        int ret = drv_can_send(msg);
        if (ret == DRV_CAN_OK) {
            msg->seq       = __atomic_add_fetch(&g_app.msg_seq, 1,
                                                __ATOMIC_SEQ_CST);
            msg->direction = CAN_DIR_TX;
            clock_gettime(CLOCK_REALTIME, &msg->timestamp);
            __atomic_add_fetch(&g_app.tx_count, 1, __ATOMIC_SEQ_CST);

            gdk_threads_add_idle(idle_update_stats, NULL);
            trace_record(msg);
        }
        free(msg);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Stats thread                                                         */
/* ------------------------------------------------------------------ */

void *thread_stats(void *arg)
{
    (void)arg;

    unsigned int ticks = 0;
    while (g_app.stats_running) {
        struct timespec ts = { 0, 50000000L }; /* 50 ms slices → responds within 50 ms */
        nanosleep(&ts, NULL);

        if (++ticks < 10) continue; /* accumulate 10 × 50 ms = 500 ms period */
        ticks = 0;

        if (!g_app.connected) continue;

        /* Atomically collect bits accumulated in the last period */
        uint64_t bits = __atomic_exchange_n(&g_app.period_bits, 0,
                                            __ATOMIC_SEQ_CST);

        /* bits / 0.5 s = bits/s;  load% = bits_per_sec / bitrate * 100 */
        double load = (double)(bits * 2) / (double)g_app.bitrate * 100.0;
        if (load > 100.0) load = 100.0;

        /* Double assignment is naturally atomic on x86-64 for aligned values */
        g_app.bus_load = load;

        gdk_threads_add_idle(idle_update_stats, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Connect / Disconnect                                                 */
/* ------------------------------------------------------------------ */

void app_do_connect(void)
{
    if (g_app.connected) {
        gui_show_error(g_gui.window, "Error", "Already connected.");
        return;
    }

    can_driver_t *drv = drv_can_get_socketcan();
    int ret = drv_can_init(drv,
                           g_app.iface,
                           g_app.bitrate,
                           g_app.data_bitrate,
                           g_app.fd_mode,
                           g_app.listen_only,
                           &g_app.bit_timing);
    if (ret != DRV_CAN_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to connect to %s:\n%s\n\n"
                 "Tip: ensure the interface exists and you have sufficient "
                 "privileges (or run with sudo).",
                 g_app.iface, drv_can_error_string(ret));
        gui_show_error(g_gui.window, "Connection Error", msg);
        return;
    }

    /* Apply the acceptance filter requested in the settings dialog, if any.
     * A filter restricts which CAN IDs the kernel delivers to this socket.
     * When disabled we leave the fresh socket at its default (receive-all);
     * installing an empty filter list would instead drop every frame. */
    if (g_app.filter_enabled)
        drv_can_set_filter(g_app.filter_id, g_app.filter_mask, g_app.filter_ext);

    /* Reset counters */
    __atomic_store_n(&g_app.rx_count,    0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_app.tx_count,    0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_app.error_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_app.msg_seq,     0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_app.period_bits, 0, __ATOMIC_SEQ_CST);
    g_app.bus_load  = 0.0;
    g_app.bus_state = CAN_BUS_ACTIVE;
    g_app.connected = 1;

    /* Start background threads */
    g_app.rx_running    = 1;
    g_app.tx_running    = 1;
    g_app.stats_running = 1;
    pthread_create(&g_app.rx_thread,    NULL, thread_rx,    NULL);
    pthread_create(&g_app.tx_thread,    NULL, thread_tx,    NULL);
    pthread_create(&g_app.stats_thread, NULL, thread_stats, NULL);

    /* Update toolbar sensitivity.
     * NOTE: the Connect button stays enabled while connected so the user can
     * re-open the connection dialog at any time (it prompts to disconnect
     * first).  Disabling it here was the cause of "the connection window is
     * not enabled again" after a connect. */
    if (g_gui.toolbar_connect)
        gtk_widget_set_sensitive(g_gui.toolbar_connect, TRUE);
    if (g_gui.toolbar_disconnect)
        gtk_widget_set_sensitive(g_gui.toolbar_disconnect, TRUE);

    /* Reflect the (possibly FD) link mode in the transmit panel so the per-row
     * DLC can grow to 64 bytes when CAN FD is enabled. */
    gui_tx_panel_update_fd();

    gui_status_message("Connected to %s @ %u bps%s",
                       g_app.iface, g_app.bitrate,
                       g_app.listen_only ? "  [listen-only]" : "");
}

void app_do_disconnect(void)
{
    if (!g_app.connected) return;

    /* Signal idle callbacks to discard any late-arriving messages */
    g_app.shutting_down = 1;

    g_app.rx_running    = 0;
    g_app.tx_running    = 0;
    g_app.stats_running = 0;

    pthread_join(g_app.rx_thread,    NULL);
    pthread_join(g_app.tx_thread,    NULL);
    pthread_join(g_app.stats_thread, NULL);

    /* Drain any pending GTK idle callbacks while widgets still exist. */
    while (g_main_context_iteration(NULL, FALSE)) {
        /* keep flushing */
    }

    drv_can_deinit();
    g_app.connected     = 0;
    g_app.shutting_down = 0;

    /* Stop trace capture if running, but keep the captured frames so the user
     * can still export them to CSV via Trace > Save after disconnecting. */
    if (g_app.tracing)
        g_app.tracing = 0;

    /* Update toolbar sensitivity */
    if (g_gui.toolbar_connect)
        gtk_widget_set_sensitive(g_gui.toolbar_connect, TRUE);
    if (g_gui.toolbar_disconnect)
        gtk_widget_set_sensitive(g_gui.toolbar_disconnect, FALSE);
    if (g_gui.toolbar_trace_start)
        gtk_widget_set_sensitive(g_gui.toolbar_trace_start, FALSE);
    if (g_gui.toolbar_trace_stop)
        gtk_widget_set_sensitive(g_gui.toolbar_trace_stop, FALSE);

    gui_update_stats();
    gui_status_message("Disconnected from %s", g_app.iface);
}
