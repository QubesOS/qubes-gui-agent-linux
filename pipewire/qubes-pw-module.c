/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright © 2022  Demi Marie Obenour  <demi@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */
/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define QUBES_PA_SINK_VCHAN_PORT 4713
#define QUBES_PA_SOURCE_VCHAN_PORT 4714

#define QUBES_PA_SOURCE_START_CMD 0x00010001
#define QUBES_PA_SOURCE_STOP_CMD 0x00010000
#define QUBES_PA_SINK_CORK_CMD 0x00020000
#define QUBES_PA_SINK_UNCORK_CMD 0x00020001

#define QUBES_AUDIOVM_QUBESDB_ENTRY "/qubes-audio-domain-xid"
#define QUBES_AUDIOVM_PW_KEY "org.qubes-os.audio-domain-xid"
#define QUBES_PW_KEY_BUFFER_SPACE   "org.qubes-os.vchan-buffer-size"
#define QUBES_PW_KEY_RECORD_BUFFER_SPACE   "org.qubes-os.record-buffer-size"
#define QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE   "org.qubes-os.playback-buffer-size"

/* C11 headers */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX headers */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/pod.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/log.h>

#include <libvchan.h>
#include <qubesdb-client.h>

/** \page page_module_example_sink PipeWire Module: Example Sink
 */

#define NAME "qubes-audio"

#ifdef PW_LOG_TOPIC_STATIC
PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic
#elif defined PW_LOG_TOPIC_INIT
#error bad PipeWire includes?
#endif

#if PW_CHECK_VERSION(0, 3, 30)
#define MODULE_EXTRA_USAGE "[ stream.sink.props=<properties> ] " \
                           "[ stream.source.props=<properties> ] "
#else
#define MODULE_EXTRA_USAGE ""
#endif

#define MODULE_USAGE    "[ node.latency=<latency as fraction> ] "                \
                        "[ node.name=<name of the nodes> ] "                    \
                        "[ node.description=<description of the nodes> ] "            \
                        "[ " QUBES_AUDIOVM_PW_KEY "=<AudioVM XID (default: use the one from QubesDB)> ] " \
                        "[ " QUBES_PW_KEY_BUFFER_SPACE "=<default vchan buffer space (headroom)> ] " \
                        "[ " QUBES_PW_KEY_RECORD_BUFFER_SPACE "=<recording headroom> ]" \
                        "[ " QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE "=<playback headroom> ]" \
                        MODULE_EXTRA_USAGE


static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>, "
                            "Demi Marie Obenour <demi@invisiblethingslab.com>" },
    { PW_KEY_MODULE_DESCRIPTION, "Qubes OS audio device" },
    { PW_KEY_MODULE_USAGE, MODULE_USAGE },
    { PW_KEY_MODULE_VERSION, "4.1.0" },
};

struct impl;

struct qubes_stream {
    struct pw_properties *stream_props;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_audio_info_raw info;
    struct libvchan *vchan, *closed_vchan;
    struct impl *impl;
    atomic_size_t current_state, last_state;
    struct spa_source source;
    size_t buffer_size;
    bool is_open, direction;
    atomic_bool dead, in_use;
};

struct impl {
    struct pw_context *context;

    struct pw_properties *props;

    struct pw_impl_module *module;

    struct spa_hook module_listener;

    struct pw_core *core;
    struct spa_loop *data_loop, *main_loop;
    struct spa_hook core_proxy_listener;
    struct spa_hook core_listener;

    struct qubes_stream stream[2];

    uint32_t frame_size, domid;

    bool do_disconnect, unloading;
};

static void unload_module(struct impl *impl)
{
    if (!impl->unloading) {
        impl->unloading = true;
        pw_impl_module_destroy(impl->module);
    }
}

static_assert(ATOMIC_BOOL_LOCK_FREE, "PipeWire agent requires lock-free atomic booleans");
static_assert(PW_DIRECTION_INPUT == 0, "wrong PW_DIRECTION_INPUT");
static_assert(PW_DIRECTION_OUTPUT == 1, "wrong PW_DIRECTION_OUTPUT");

static int vchan_error_callback(struct spa_loop *loop,
                                bool async,
                                uint32_t seq,
                                const void *data,
                                size_t size,
                                void *user_data);

/**
 * Disconnect a stream from its event loop.  Must be called on the realtime
 * thread.
 */
static void stop_watching_vchan(struct qubes_stream *stream)
{
    if (stream->vchan) {
        // Must do this first, so that EPOLL_CTL_DEL is called before the
        // file descriptor is closed.
        if (stream->impl)
            spa_loop_remove_source(stream->impl->data_loop, &stream->source);
        stream->closed_vchan = stream->vchan;
        stream->vchan = NULL;
    }
    stream->is_open = false;
    // Update the main-thread state asynchronously.
    if (stream->impl)
        spa_loop_invoke(stream->impl->main_loop,
                        vchan_error_callback, 0, NULL, 0, false,
                        stream);
}

static int remove_stream_cb(struct spa_loop *loop,
                            bool async,
                            uint32_t seq,
                            const void *data,
                            size_t size,
                            void *user_data)
{
    stop_watching_vchan(user_data);
    return 0;
}

static void vchan_ready(struct spa_source *source);

/**
 * Called on the realtime thread after creating a vchan.  The main
 * thread is suspended.
 *
 * @param loop The event loop to use.
 * @param async Was this called synchronously or asynchronously?
 * @param seq a 32-bit sequence number.
 * @param data The data argument to spa_loop_invoke().  Not used by this
 *        function.
 * @param size The size of that data.
 * @param user_data The user data passed to spa_loop_invoke().
 */
static int add_stream(struct spa_loop *loop,
                      bool async,
                      uint32_t seq,
                      const void *data,
                      size_t size,
                      void *user_data)
{
    struct qubes_stream *stream = user_data;
    if (stream->dead)
        return -ESHUTDOWN;
    spa_assert(stream->closed_vchan);
    spa_assert(!stream->vchan);
    stream->vchan = stream->closed_vchan;
    stream->closed_vchan = NULL;
    stream->source.loop = stream->impl->data_loop;
    stream->source.func = vchan_ready;
    stream->source.data = stream;
    stream->source.fd = libvchan_fd_for_select(stream->vchan);
    stream->source.mask = SPA_IO_IN;
    return spa_loop_add_source(loop, &stream->source);
}

/**
 * Connect a Qubes stream.  Must be called on the main thread.
 *
 * @param stream The stream to connect.
 */
static void connect_stream(struct qubes_stream *stream)
{
    const char *msg = stream->direction ? "capture" : "playback";
    uint16_t domid = stream->impl->domid;
    spa_assert(stream->vchan == NULL);
    spa_assert(stream->closed_vchan == NULL);
    stream->closed_vchan = stream->direction ?
        libvchan_server_init((int)domid, QUBES_PA_SOURCE_VCHAN_PORT, stream->buffer_size, 128) :
        libvchan_server_init((int)domid, QUBES_PA_SINK_VCHAN_PORT, 128, stream->buffer_size);
    if (stream->closed_vchan == NULL) {
        pw_log_error("can't create %s vchan, audio will not work", msg);
    }
    if (spa_loop_invoke(stream->impl->data_loop, add_stream, 0, NULL, 0, true, stream))
        pw_log_error("spa_loop_add_source() failed, audio will not work");
    return;
}

/**
 * Called on the main thread to shut down a stream.
 *
 * \param stream The stream to shut down.
 */
static void stream_shutdown(struct qubes_stream *stream)
{
    if (stream->stream)
        pw_stream_disconnect(stream->stream);
    pw_log_info("Closing stale vchan");
    if (stream->closed_vchan)
        libvchan_close(stream->closed_vchan);
    stream->closed_vchan = NULL;
}

/**
 * Called on the main thread to destroy a stream.
 */
static void stream_destroy(struct impl *impl, enum spa_direction direction)
{
    struct qubes_stream *stream = impl->stream + direction;
    if (stream->dead)
        return;
    stream->dead = true;
    spa_loop_invoke(impl->data_loop, remove_stream_cb, 0, NULL, 0, true, stream);
    /* after this point the realtime thread is definitely aware of the shutdown */
    stream_shutdown(stream);
    spa_hook_remove(&stream->stream_listener);
    stream->stream = NULL;
    if (stream->stream_props)
        pw_properties_free(stream->stream_props);
}

/**
 * Called on the main thread to destroy the capture stream.
 */
static void capture_stream_destroy(void *d)
{
    stream_destroy(d, PW_DIRECTION_INPUT);
}

/**
 * Called on the main thread to destroy the playback stream.
 */
static void playback_stream_destroy(void *d)
{
    stream_destroy(d, PW_DIRECTION_OUTPUT);
}

/** Called on the main thread to set the stream state */
static void set_stream_state(struct qubes_stream *stream, bool state)
{
    static_assert(atomic_is_lock_free(&stream->current_state),
                  "PipeWire agent requires lock-free atomic size_t");
    stream->current_state = state;
}

static const struct pw_stream_events capture_stream_events, playback_stream_events;

/**
 * Called on the main thread when a vchan has been disconnected.
 */
static int vchan_error_callback(struct spa_loop *loop,
                                bool async,
                                uint32_t seq,
                                const void *data,
                                size_t size,
                                void *user_data)
{
    struct qubes_stream *stream = user_data;

    spa_assert(!stream->vchan);
    stream_shutdown(stream);
    if (!stream->dead)
        connect_stream(stream);
    return 0;
}

static void discard_unwanted_recorded_data(struct qubes_stream *stream);

/* Called on the main thread by spa_loop_invoke() from vchan_ready() */
static int main_thread_connect(struct spa_loop *loop,
                               bool async,
                               uint32_t seq,
                               const void *data,
                               size_t size,
                               void *user_data)
{

    struct qubes_stream *stream;
    uint32_t n_params = 0;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = { 0 };

    if ((stream = user_data)->dead)
        return 0;
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    params[n_params++] = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &stream->info);

    if (pw_stream_connect(stream->stream,
            stream->direction,
            PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS |
            0,
            params, n_params) < 0)
        pw_log_error("Could not connect stream: %m");
    return 0;
}

static int process_control_commands(struct impl *impl);

static int process_control_commands_cb(struct spa_loop *loop,
                                       bool async,
                                       uint32_t seq,
                                       const void *data,
                                       size_t size,
                                       void *user_data)
{
    return process_control_commands(user_data);
}

/**
 * Called on the realtime thread when a vchan's event channel
 * is signaled.  Must not be called on any other thread.
 *
 * @param source The spa_source that triggered the event.
 */
static void vchan_ready(struct spa_source *source)
{
    struct qubes_stream *stream = source->data;
    struct qubes_stream *playback_stream = stream->impl->stream;

    // 0: Check if the vchan exists
    if (!stream->vchan) {
        spa_assert(!stream->is_open && "no vchan on open stream?");
        pw_log_error("vchan_ready() called with vchan closed???");
        return;
    }

    // 1: Acknowledge vchan event
    pw_log_debug("Waiting for vchan");
    libvchan_wait(stream->vchan);
    pw_log_debug("Vchan awaited");

    // 2: Figure out if stream is open
    bool is_open = libvchan_is_open(stream->vchan);
    if (is_open != stream->is_open) {
        if (is_open) {
            // vchan connected
            spa_loop_invoke(stream->impl->main_loop,
                            main_thread_connect, 0, NULL, 0, false, stream);
            if (stream->direction) {
                // vchan just opened, no need to check for buffer space
                const uint32_t control_commands[2] = {
                    (stream->last_state = stream->current_state) ?
                        QUBES_PA_SOURCE_START_CMD : QUBES_PA_SOURCE_STOP_CMD,
                    (playback_stream->last_state = playback_stream->current_state) ?
                        QUBES_PA_SINK_UNCORK_CMD : QUBES_PA_SINK_CORK_CMD,
                };
                if (libvchan_write(stream->vchan, control_commands, sizeof control_commands) !=
                    sizeof control_commands)
                    pw_log_error("Cannot write stream initial states to vchan");
            }

            stream->is_open = true;
        } else {
            // vchan disconnected.  Stop watching for events on it.
            stop_watching_vchan(stream);
        }
    }
    if (!is_open)
        return; /* vchan closed */
    if (!stream->direction)
        return; // Nothing to do for playback
    discard_unwanted_recorded_data(stream);
    process_control_commands(stream->impl);
}

/**
 * Called on the realtime thread to discard unwanted data from the daemon.
 */
static void discard_unwanted_recorded_data(struct qubes_stream *stream)
{
    if (stream->last_state)
        return; // Nothing to do, capture_stream_process() will deal with it

    if (!stream->vchan)
        return; // No vchan

    if (!libvchan_is_open(stream->vchan))
        return; // vchan closed

    // Discard unexpected data
    char buf[512];
    int ready = libvchan_data_ready(stream->vchan);
    if (ready <= 0)
        return;

    size_t to_read = (size_t)ready;
    pw_log_debug("Discarding %d bytes of unwanted data", ready);
    while (to_read > 0) {
        int res = libvchan_read(stream->vchan, buf, to_read > sizeof buf ? sizeof buf : to_read);
        if (res <= 0)
            break;
        to_read -= (size_t)res;
    }
}

/**
 * Called on the realtime thread to process control commands.
 */
static int process_control_commands(struct impl *impl)
{
    struct qubes_stream *capture_stream = impl->stream + PW_DIRECTION_OUTPUT,
                       *playback_stream = impl->stream + PW_DIRECTION_INPUT;
    bool new_state = playback_stream->current_state;
    struct libvchan *control_vchan = capture_stream->vchan;

    if (!control_vchan) {
        pw_log_error("Control vchan closed, cannot issue control command");
        return -EPIPE;
    }

    if (new_state != playback_stream->last_state) {
        uint32_t cmd = new_state ? QUBES_PA_SINK_UNCORK_CMD : QUBES_PA_SINK_CORK_CMD;

        if (libvchan_buffer_space(control_vchan) < (int)sizeof(cmd)) {
            pw_log_error("cannot write command to control vchan: no buffer space");
            return -ENOBUFS;
        }

        if (libvchan_send(control_vchan, &cmd, sizeof(cmd)) != sizeof(cmd)) {
            pw_log_error("error writing command to control vchan");
            return -EPROTO;
        }

        pw_log_info("Audio playback %s", new_state ? "started" : "stopped");

        playback_stream->last_state = new_state;
    }

    new_state = capture_stream->current_state;
    if (new_state != capture_stream->last_state) {
        uint32_t cmd = new_state ? QUBES_PA_SOURCE_START_CMD : QUBES_PA_SOURCE_STOP_CMD;

        if (libvchan_buffer_space(control_vchan) < (int)sizeof(cmd)) {
            pw_log_error("cannot write command to control vchan: no buffer space");
            return -ENOSPC;
        }

        if (libvchan_send(control_vchan, &cmd, sizeof(cmd)) != sizeof(cmd)) {
            pw_log_error("error writing command to control vchan");
            return -ENOSPC;
        }

        pw_log_info("Audio capturing %s", new_state ? "started" : "stopped");

        capture_stream->last_state = new_state;
    }

    return 0;
}

static void stream_state_changed_common(void *d, enum pw_stream_state old,
        enum pw_stream_state state, const char *error, bool playback)
{
    struct impl *impl = d;
    const char *const name = playback ? "playback" : "capture";

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        pw_log_error("%s error: %s", name, error ? error : "(null)");
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], false);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        pw_log_debug("%s unconnected", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], false);
        break;
    case PW_STREAM_STATE_CONNECTING:
        pw_log_debug("%s connected", name);
        return;
    case PW_STREAM_STATE_PAUSED:
        pw_log_debug("%s paused", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], false);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_log_debug("%s streaming", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], true);
        break;
    default:
        pw_log_error("unknown %s stream state %d", name, state);
        return;
    }
    spa_loop_invoke(impl->data_loop, process_control_commands_cb, 0, NULL, 0, true, impl);
    pw_log_debug("Successfully queued message");
}

static void playback_stream_state_changed(void *d, enum pw_stream_state old,
        enum pw_stream_state state, const char *error)
{
    return stream_state_changed_common(d, old, state, error, true);
}

static void capture_stream_state_changed(void *d, enum pw_stream_state old,
        enum pw_stream_state state, const char *error)
{
    return stream_state_changed_common(d, old, state, error, false);
}

static void capture_stream_process(void *d)
{
    struct impl *impl = d;
    struct pw_buffer *b;
    struct qubes_stream *stream = impl->stream + PW_DIRECTION_OUTPUT;
    uint8_t *dst;
    uint32_t bytes_ready = 0, size;

    if ((b = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
        pw_log_warn("out of capture buffers: %m");
        return;
    }

    if (!stream->vchan || !libvchan_is_open(stream->vchan))
        pw_log_error("vchan not open yet!");
    else {
        int ready = libvchan_data_ready(stream->vchan);
        if (ready < 0)
            pw_log_error("vchan problem!");
        else
            bytes_ready = (uint32_t)ready;
    }

    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas < 1 || (dst = buf->datas[0].data) == NULL)
        goto done;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = 4;
    buf->datas[0].chunk->size = 0;

    spa_assert(buf->n_datas == 1 && "wrong number of datas");

    size = buf->datas[0].maxsize;
    // TODO: handle more data
#if PW_CHECK_VERSION(0, 3, 49)
    if (SPA_UNLIKELY(b->requested > size))
        pw_log_warn("Can only record %" PRIu32 " bytes of %" PRIu64, size,
                    b->requested);
    else if (b->requested)
        size = b->requested;
#endif

    if (__builtin_mul_overflow(size, impl->frame_size, &size)) {
        pw_log_error("Overflow calculating amount of data there is room for????");
        goto done;
    }
    buf->datas[0].chunk->size = size;

    if (size > bytes_ready) {
        pw_log_warn("Underrun: asked to read %" PRIu32 " bytes, but only %d available", size, (int)bytes_ready);
        memset(dst + bytes_ready, 0, size - bytes_ready);
        size = bytes_ready;
    }

    pw_log_debug("reading %" PRIu32 " bytes from vchan", size);
    if (size && libvchan_read(stream->vchan, dst, size) != (int)size) {
        pw_log_error("vchan error: %m");
        // avoid recording uninitialized memory
        memset(dst, 0, size);
    }
done:
    pw_stream_queue_buffer(stream->stream, b);
}

static void playback_stream_process(void *d)
{
    struct impl *impl = d;
    struct pw_buffer *buf;
    struct qubes_stream *stream = impl->stream + PW_DIRECTION_INPUT;
    struct qubes_stream *capture_stream = impl->stream + PW_DIRECTION_OUTPUT;
    struct spa_data *bd;
    uint8_t *data;
    uint32_t size;

    if (!stream->vchan || !libvchan_is_open(stream->vchan)) {
        pw_log_error("Cannot read data, vchan not functional");
        return;
    }

    int ready = libvchan_buffer_space(stream->vchan);

    discard_unwanted_recorded_data(capture_stream);

    pw_log_debug("%d bytes ready for writing", ready);

    if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
        pw_log_error("out of buffers: %m");
        return;
    }

    spa_assert(buf->buffer->n_datas == 1 && "wrong number of datas");

    bd = &buf->buffer->datas[0];
    spa_assert(bd->chunk->offset == 0);
    data = bd->data + bd->chunk->offset;
    size = bd->chunk->size;

    if (ready <= 0 || size > (uint32_t)ready) {
        pw_log_warn("Overrun: asked to write %" PRIu32 " bytes, but can only write %d", size, ready);
        if (stream->last_state) {
            // Force process_control_commands() to send QUBES_PA_SINK_UNCORK_CMD
            stream->last_state = 2;
        }
        process_control_commands(stream->impl);
        size = ready;
    }

    pw_log_debug("writing %" PRIu32 " bytes to vchan", size);
    if (size > 0 && libvchan_write(stream->vchan, data, size) != (int)size) {
        pw_log_error("vchan error: %m");
        return;
    }
    pw_stream_queue_buffer(stream->stream, buf);
}

static const struct spa_audio_info_raw qubes_audio_format = {
    .format = SPA_AUDIO_FORMAT_S16_LE,
    .flags = 0,
    .rate = 44100,
    .channels = 2,
    .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
};

static void stream_param_changed(void *data, uint32_t id,
        const struct spa_pod *param, enum spa_direction direction)
{
    struct impl *impl = data;
    uint32_t media_type = UINT32_MAX, media_subtype = UINT32_MAX;
    struct spa_audio_info_raw info = { 0 };
    uint64_t params_buffer[64];
    int res;

    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *params[5];

    switch (id) {
    case SPA_PARAM_Format:
        break;
    case SPA_PARAM_Props:
        /* TODO: reconfigure the stream according to the new properties */
        return;
    default:
        pw_log_error("Unknown id %" PRIu32, id);
        return;
    }

    spa_assert(direction >= 0 && direction <= 1);

    if (param == NULL)
        goto doit;

    if ((res = spa_format_parse(param, &media_type, &media_subtype) < 0)) {
        errno = -res;
        pw_log_error("spa_format_parse() failed: %m");
        errno = -res;
        return;
    }

    if (media_type != SPA_MEDIA_TYPE_audio ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        pw_log_error("Ignoring format info that isn't for raw audio");
        errno = ENOTSUP;
        return;
    }

    if (spa_format_audio_raw_parse(param, &info) < 0) {
        pw_log_error("Could not parse raw audio format info");
        errno = ENOTSUP;
        return;
    }

    if (info.format != SPA_AUDIO_FORMAT_S16_LE) {
        pw_log_error("Unsupported audio format %d", info.format);
        errno = ENOTSUP;
        return;
    }

    if (info.rate != 44100) {
        pw_log_error("Unsupported audio rate %" PRIu32, info.rate);
        errno = ENOTSUP;
        return;
    }

    if (info.channels != 2) {
        pw_log_error("Expected 2 channels, got %" PRIu32, info.channels);
        errno = ENOTSUP;
        return;
    }

    if (info.position[0] != SPA_AUDIO_CHANNEL_FL) {
        pw_log_error("Expected channel SPA_AUDIO_CHANNEL_FL, got %" PRIu32,
                     info.position[0]);
        errno = ENOTSUP;
        return;
    }

    if (info.position[1] != SPA_AUDIO_CHANNEL_FR) {
        pw_log_error("Expected channel SPA_AUDIO_CHANNEL_FR, got %" PRIu32,
                     info.position[1]);
        errno = ENOTSUP;
        return;
    }

doit:
    params[0] = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 64),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(2048 * (int)impl->frame_size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)impl->frame_size),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr)));
    spa_assert_se(params[0]);

    params[1] = spa_format_audio_raw_build(&b, SPA_PARAM_Format,
            (struct spa_audio_info_raw *)&qubes_audio_format);
    spa_assert_se(params[1]);

    spa_assert_se(b.state.offset <= sizeof params_buffer);

    if ((res = pw_stream_update_params(impl->stream[direction].stream, params, 2)) < 0) {
        errno = -res;
        pw_log_error("Failed to negotiate parameters: %m");
        errno = -res;
    }
}

static void capture_stream_param_changed(void *data, uint32_t id,
        const struct spa_pod *param)
{
    stream_param_changed(data, id, param, PW_DIRECTION_OUTPUT);
}

static const struct pw_stream_events capture_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = capture_stream_destroy,
    .state_changed = capture_stream_state_changed,
    .control_info = NULL,
    .io_changed = NULL,
    .param_changed = capture_stream_param_changed,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = capture_stream_process,
    .drained = NULL,
};

static void playback_stream_param_changed(void *data, uint32_t id,
        const struct spa_pod *param)
{
    stream_param_changed(data, id, param, PW_DIRECTION_INPUT);
}

static const struct pw_stream_events playback_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = playback_stream_destroy,
    .state_changed = playback_stream_state_changed,
    .control_info = NULL,
    .io_changed = NULL,
    .param_changed = playback_stream_param_changed,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = playback_stream_process,
    .drained = NULL,
};

static int create_stream(struct impl *impl, enum spa_direction direction)
{
    struct qubes_stream *stream = impl->stream + direction;

    stream->stream = pw_stream_new(impl->core, direction == PW_DIRECTION_INPUT ? "Qubes Sink" : "Qubes Source", stream->stream_props);
    stream->stream_props = NULL;

    if (stream->stream == NULL)
        return -errno;
    return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct impl *impl = data;

    pw_log_error("error id:%u seq:%d res:%d (%s): %s",
            id, seq, res, spa_strerror(res), message);

    if (id == PW_ID_CORE && res == -EPIPE)
        unload_module(impl);
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

static void core_destroy(void *d)
{
    struct impl *impl = d;
    spa_hook_remove(&impl->core_proxy_listener);
    spa_hook_remove(&impl->core_listener);
    impl->core = NULL;
    unload_module(impl);
}

static const struct pw_proxy_events core_proxy_events = {
    .destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
    stream_destroy(impl, PW_DIRECTION_INPUT);
    stream_destroy(impl, PW_DIRECTION_OUTPUT);
    if (impl->core) {
        if (impl->do_disconnect)
            pw_core_disconnect(impl->core);
        if (impl->core) {
            spa_hook_remove(&impl->core_proxy_listener);
            spa_hook_remove(&impl->core_listener);
        }
    }
    pw_properties_free(impl->props);
    free(impl);
}

static void module_destroy(void *data)
{
    struct impl *impl = data;
    impl->unloading = true;
    spa_hook_remove(&impl->module_listener);
    impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy,
    .free = NULL,
    .initialized = NULL,
    .registered = NULL,
};

#define QUBES_ARRAY_SIZE(s) \
    ({ _Pragma("GCC diagnostic push"); \
       _Pragma("GCC diagnostic error \"-Wsizeof-pointer-div\""); \
       _Static_assert(!__builtin_types_compatible_p(__typeof__(s), \
                            __typeof__(&*(s))), \
              "expression " #s " is a pointer, not an array"); \
       _Static_assert(__builtin_types_compatible_p(__typeof__(s), \
                                                   __typeof__((s)[0])[sizeof(s)/sizeof((s)[0])]), \
              "expression " #s " is not an array"); \
       (sizeof(s)/sizeof((s)[0])); \
       _Pragma("GCC diagnostic pop"); \
     })

static void parse_audio_info(struct impl *impl)
{
    for (uint8_t i = 0; i < 2; ++i) {
        struct spa_audio_info_raw *info = &impl->stream[i].info;

        _Static_assert(QUBES_ARRAY_SIZE(impl->stream) == 2, "out of bounds bug");
        spa_zero(*info);

        info->format = SPA_AUDIO_FORMAT_S16_LE;
        info->channels = 2;
        info->rate = 44100;
        _Static_assert(QUBES_ARRAY_SIZE(info->position) >= 2, "out of bounds bug");
        info->position[0] = SPA_AUDIO_CHANNEL_FL;
        info->position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    impl->frame_size = 4;
}

static const struct spa_dict_item source_props[] = {
    { PW_KEY_NODE_NAME, "qubes-source" },
    { PW_KEY_NODE_DESCRIPTION, "Qubes Virtual Audio Source" },
    { "node.want-driver", "true" },
    // { PW_KEY_MEDIA_TYPE, "Audio" },
    { PW_KEY_MEDIA_CLASS, "Audio/Source" },
    { PW_KEY_AUDIO_RATE, "44100" },
    { PW_KEY_AUDIO_CHANNELS, "2" },
    { PW_KEY_AUDIO_FORMAT, "S16LE" },
    { "audio.position", "[ FL FR ]" },
};

static const struct spa_dict source_dict = SPA_DICT_INIT_ARRAY(source_props);

static const struct spa_dict_item sink_props[] = {
    { PW_KEY_NODE_NAME, "qubes-sink" },
    { PW_KEY_NODE_DESCRIPTION, "Qubes Virtual Audio Sink" },
    { "node.want-driver", "true" },
    // { PW_KEY_MEDIA_TYPE, "Audio" },
    { PW_KEY_MEDIA_CLASS, "Audio/Sink" },
    { PW_KEY_AUDIO_RATE, "44100" },
    { PW_KEY_AUDIO_CHANNELS, "2" },
    { PW_KEY_AUDIO_FORMAT, "S16LE" },
    { "audio.position", "[ FL FR ]"},
};

static const struct spa_dict sink_dict = SPA_DICT_INIT_ARRAY(sink_props);

// FIXME: this should be a Qubes-wide domID parsing function
static int parse_number(const char *const str,
        size_t max_value, size_t *res, const char *const msg)
{
    char *endptr = (void *)1;
    errno = *res = 0;
    unsigned long long value = strtoull(str, &endptr, 10);
    if (errno) {
        int i = errno;
        pw_log_error("Invalid %s \"%s\": %m", msg, str);
        return -i;
    } else if (*endptr) {
        pw_log_error("Invalid %s \"%s\": trailing junk (\"%s\")",
                msg, str, endptr);
        return -EINVAL;
    } else if (value > max_value) {
        pw_log_error("Invalid %s \"%s\": exceeds maximum %s %zu",
                msg, str, msg, max_value);
        return -ERANGE;
    } else {
        *res = (size_t)value;
        return 0;
    }
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL, *arg_props = NULL;
    const struct pw_properties *global_props = NULL;
    struct impl *impl;
    const char *str;
    const char *peer_domain_prop = NULL;
    int res = -EFAULT; /* should never be returned, modulo bugs */

#ifdef PW_LOG_TOPIC_INIT
    PW_LOG_TOPIC_INIT(mod_topic);
#endif
    pw_log_info("hello from qubes module");

    impl = calloc(1, sizeof(struct impl));
    if (impl == NULL)
        return -errno;

    if (args == NULL)
        args = "";

    impl->module = module;
    impl->context = context;

    uint32_t n_support;
    const struct spa_support *support = pw_context_get_support(context, &n_support);
    if (!support) {
        res = -errno;
        pw_log_error("cannot get support: %m");
        goto error;
    }

    impl->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
    if (!impl->data_loop) {
        res = -errno;
        pw_log_error("cannot get data loop: %m");
        goto error;
    }

    impl->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
    if (!impl->main_loop) {
        res = -errno;
        pw_log_error("cannot get main loop: %m");
        goto error;
    }

    if (!(global_props = pw_context_get_properties(context))) {
        res = -errno || -ENOMEM;
        pw_log_error("cannot obtain properties: %m");
        goto error;
    }

    if (!(props = pw_properties_copy(global_props))) {
        res = -errno || -ENOMEM;
        pw_log_error("cannot clone properties: %m");
        goto error;
    }

    if ((arg_props = pw_properties_new_string(args)) == NULL) {
        res = -errno;
        pw_log_error( "can't parse arguments: %m");
        goto error;
    }

    if ((res = pw_properties_update(props, &arg_props->dict)) < 0) {
        res = -errno;
        pw_log_error( "can't update properties: %m");
        goto error;
    }

    impl->props = props;
    if ((peer_domain_prop = pw_properties_get(props, QUBES_AUDIOVM_PW_KEY)) == NULL) {
        qdb_handle_t qdb = qdb_open(NULL);
        if (!qdb) {
            res = -errno;
            pw_log_error("Could not open QubesDB to get %s property: %m",
                     QUBES_AUDIOVM_PW_KEY);
            goto error;
        }

        char *qdb_entry = qdb_read(qdb, QUBES_AUDIOVM_QUBESDB_ENTRY, NULL);
        if (qdb_entry == NULL) {
            res = -errno;
            if (res == -ENOENT)
                pw_log_error("%s not specified, and no %s entry in QubesDB",
                         QUBES_AUDIOVM_PW_KEY, QUBES_AUDIOVM_QUBESDB_ENTRY);
            else
                pw_log_error("%s not specified, and unable to obtain %s entry from QubesDB: %m",
                         QUBES_AUDIOVM_PW_KEY, QUBES_AUDIOVM_QUBESDB_ENTRY);
            qdb_close(qdb);
            goto error;
        }
        qdb_close(qdb);
        pw_properties_set(props, QUBES_AUDIOVM_PW_KEY, qdb_entry);
        free(qdb_entry);
        if (!(peer_domain_prop = pw_properties_get(props, QUBES_AUDIOVM_PW_KEY))) {
            pw_log_error("Failed to set %s key - out of memory?", QUBES_AUDIOVM_PW_KEY);
            res = -ENOMEM;
            goto error;
        }
    }

    {
        size_t domid;
        if ((res = parse_number(peer_domain_prop, INT_MAX / 2, &domid, "domain ID")))
            goto error;
        impl->domid = domid;

        size_t read_min = 0x8000, write_min = 0x8000;
        const char *record_size = pw_properties_get(props, QUBES_PW_KEY_RECORD_BUFFER_SPACE);
        const char *playback_size = pw_properties_get(props, QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE);
        if (!record_size || !playback_size) {
            const char *buffer_size = pw_properties_get(props, QUBES_PW_KEY_BUFFER_SPACE);
            if (!record_size)
                record_size = buffer_size;
            if (!playback_size)
                playback_size = buffer_size;
        }

        if (record_size &&
            (res = parse_number(record_size, INT32_MAX / 2, &read_min, "record buffer size")))
            goto error;

        if (playback_size &&
            (res = parse_number(playback_size, INT32_MAX / 2, &write_min, "playback buffer size")))
            goto error;

        impl->stream[PW_DIRECTION_OUTPUT].buffer_size = read_min;
        impl->stream[PW_DIRECTION_INPUT].buffer_size = write_min;
    }

    pw_log_info("module %p: new (%s), peer id is %d", impl, args, (int)impl->domid);
    pw_log_info("module %p: record buffer size %zu, playback buffer size %zu",
                impl,
                impl->stream[PW_DIRECTION_OUTPUT].buffer_size,
                impl->stream[PW_DIRECTION_INPUT].buffer_size);

    for (uint8_t i = 0; i < 2; ++i) {
        const char *msg = i ? "sink" : "source";
        const struct spa_dict *dict = i ? &source_dict : &sink_dict;
        struct qubes_stream *stream = impl->stream + i;
        stream->stream_props = pw_properties_new_dict(dict);
        if (stream->stream_props == NULL) {
            res = -errno;
            pw_log_error("can't create %s properties: %m", msg);
            goto error;
        }
    }

#if PW_CHECK_VERSION(0, 3, 30)
    if ((str = pw_properties_get(arg_props, "stream.source.props")) != NULL)
        pw_properties_update_string(impl->stream[PW_DIRECTION_OUTPUT].stream_props, str, strlen(str));

    if ((str = pw_properties_get(arg_props, "stream.sink.props")) != NULL)
        pw_properties_update_string(impl->stream[PW_DIRECTION_INPUT].stream_props, str, strlen(str));
#endif

    pw_properties_free(arg_props);
    arg_props = NULL;

    parse_audio_info(impl);

    impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
    if (impl->core == NULL) {
        str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
        impl->core = pw_context_connect(impl->context,
                pw_properties_new(
                    PW_KEY_REMOTE_NAME, str,
                    NULL),
                0);
        impl->do_disconnect = true;
    }

    if (impl->core == NULL) {
        res = -errno;
        pw_log_error("can't connect: %m");
        goto error;
    }

    pw_proxy_add_listener((struct pw_proxy*)impl->core,
            &impl->core_proxy_listener,
            &core_proxy_events, impl);
    pw_core_add_listener(impl->core,
            &impl->core_listener,
            &core_events, impl);

    if ((res = create_stream(impl, PW_DIRECTION_INPUT)) < 0)
        goto error;

    if ((res = create_stream(impl, PW_DIRECTION_OUTPUT)) < 0)
        goto error;

    for (uint8_t i = 0; i < 2; ++i) {
        struct qubes_stream *stream = impl->stream + i;
        stream->impl = impl;
        stream->direction = i;
        connect_stream(stream);
        pw_stream_add_listener(stream->stream,
                &stream->stream_listener,
                stream->direction ? &capture_stream_events : &playback_stream_events,
                impl);
    }

    pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

    pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

    return 0;

error:
    impl_destroy(impl);
    return res;
}
