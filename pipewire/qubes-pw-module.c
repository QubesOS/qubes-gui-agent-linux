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
/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-FileCopyrightText: Copyright © 2019,2021-2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#define QUBES_PA_SINK_VCHAN_PORT 4713
#define QUBES_PA_SOURCE_VCHAN_PORT 4714

#define QUBES_PA_SOURCE_START_CMD 0x00010001
#define QUBES_PA_SOURCE_STOP_CMD 0x00010000
#define QUBES_PA_SINK_CORK_CMD 0x00020000
#define QUBES_PA_SINK_UNCORK_CMD 0x00020001
#define QUBES_STREAM_RATE 44100

#define QUBES_AUDIOVM_QUBESDB_ENTRY "/qubes-audio-domain-xid"
#define QUBES_AUDIOVM_PW_KEY "org.qubes-os.audio-domain-xid"
#define QUBES_PW_KEY_BUFFER_SPACE   "org.qubes-os.vchan-buffer-size"
#define QUBES_PW_KEY_RECORD_BUFFER_SPACE   "org.qubes-os.record.buffer-size"
#define QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE   "org.qubes-os.playback.buffer-size"
#define QUBES_PW_KEY_PLAYBACK_TARGET_BUFFER_FILL   "org.qubes-os.playback.target-buffer-fill"
#define QUBES_PW_KEY_CAPTURE_TARGET_BUFFER_FILL   "org.qubes-os.record.target-buffer-fill"
#define QUBES_PW_KEY_ASSUME_MIC_ATTACHED   "org.qubes-os.assume-mic-attached"

/* Check that PipeWire supports a specific feature.
 * The "1 &&" is for ease of testing: by replacing the 1 with 0, one can
 * easily disable the feature manually to make sure the code still compiles. */
#define QUBES_PW_HAS_SCHEDULE_DESTROY (1 && PW_CHECK_VERSION(0, 3, 65))
#define QUBES_PW_HAS_TARGET (1 && PW_CHECK_VERSION(0, 3, 68))

/* C11 headers */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

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

#include <pipewire/data-loop.h>
#include <pipewire/impl.h>
#include <pipewire/log.h>

#include <libvchan.h>
#include <qubesdb-client.h>

/* Throughout this file, input and output are from _the stream's_
 * perspective.  This means that audio _recording_ uses an _output_
 * stream, while audio _playback_ uses an _input_ stream.  This is
 * the _opposite_ of what one would expect, and of what an application
 * would use.
 *
 * Specifically:
 *
 * - Input stream, Audio/Sink media class: this is a _sink_, which applications
 *   will play audio into.
 * - Output stream, Audio/Source media class: this is a _source_, which
 *   applications will record audio from.
 * - Output stream, Stream/Output/Audio media class: this is application
 *   _playing audio_, which will be sent to a sink.
 * - Input stream, Stream/Input/Audio media class: this is an application
 *   _recording audio_, which will be taken from a source.
 *
 * Yes, this is confusing.  Yes, this means that info->stream[PW_DIRECTION_OUTPUT]
 * gives the _capture_ stream, and info->stream[PW_DIRECTION_INPUT] gives the
 * _playback_ stream.
 */

#if PW_CHECK_VERSION(0, 3, 50)
#include <spa/utils/dll.h>
#else

/* vendored copy for old PipeWire */

#include <stddef.h>
#include <math.h>

#define SPA_DLL_BW_MAX		0.128
#define SPA_DLL_BW_MIN		0.016

struct spa_dll {
	double bw;
	double z1, z2, z3;
	double w0, w1, w2;
};

static inline void spa_dll_init(struct spa_dll *dll)
{
	dll->bw = 0.0;
	dll->z1 = dll->z2 = dll->z3 = 0.0;
}

static inline void spa_dll_set_bw(struct spa_dll *dll, double bw, unsigned period, unsigned rate)
{
	double w = 2 * M_PI * bw * period / rate;
	dll->w0 = 1.0 - exp (-20.0 * w);
	dll->w1 = w * 1.5 / period;
	dll->w2 = w / 1.5;
	dll->bw = bw;
}

static inline double spa_dll_update(struct spa_dll *dll, double err)
{
	dll->z1 += dll->w0 * (dll->w1 * err - dll->z1);
	dll->z2 += dll->w0 * (dll->z1 - dll->z2);
	dll->z3 += dll->w2 * dll->z2;
	return 1.0 - (dll->z2 + dll->z3);
}
#endif


/** \page page_module_example_sink PipeWire Module: Example Sink
 */

#define NAME "qubes-audio"

#ifdef PW_LOG_TOPIC_STATIC
PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic
#elif defined PW_LOG_TOPIC_INIT
#error bad PipeWire includes?
#endif

#define MODULE_USAGE    "[ node.latency=<latency as fraction> ] "                \
                        "[ node.name=<name of the nodes> ] "                    \
                        "[ node.description=<description of the nodes> ] "            \
                        "[ " QUBES_AUDIOVM_PW_KEY "=<AudioVM XID (default: use the one from QubesDB)> ] " \
                        "[ " QUBES_PW_KEY_BUFFER_SPACE "=<default vchan buffer space (headroom)> ] " \
                        "[ " QUBES_PW_KEY_RECORD_BUFFER_SPACE "=<recording headroom> ]" \
                        "[ " QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE "=<playback headroom> ]" \
                        "[ stream.sink.props=<properties> ] " \
                        "[ stream.source.props=<properties> ] "

// FIXME: this should be a Qubes-wide domID parsing function
static int parse_number(const char *const str,
        size_t max_value, size_t *res, const char *const msg)
{
    char *endptr = (void *)1;
    errno = *res = 0;
    unsigned long long value = strtoull(str, &endptr, 0);
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

static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>, "
                            "Demi Marie Obenour <demi@invisiblethingslab.com>" },
    { PW_KEY_MODULE_DESCRIPTION, "Qubes OS audio device" },
    { PW_KEY_MODULE_USAGE, MODULE_USAGE },
    { PW_KEY_MODULE_VERSION, "4.1.0" },
};

struct impl;

/** Rate matching information */
struct rate_match {
    /** The maximum error that will be used to update the DLL. */
    double max_error;

    /** If we are driving the graph, the amount by which the timestamp
     * will be increased.  Otherwise, the inverse of the rate match value. */
    double corr;

    /** Delay-Locked Loop used for timing decisions */
    struct spa_dll dll;

    /** The target fill level for the buffer. */
    uint32_t target_buffer;

    /** Period of the system. FIXME: the meaning of this isn't clear. */
    uint32_t period;

    /** Pointer to the rate-match IO */
    struct spa_io_rate_match *rate_match;
};

struct qubes_stream {
    // Stream properties.  Immutable after init.
    struct pw_properties *stream_props;
    // Pointer to stream.  Immutable after init.
    struct pw_stream *stream;
    // Stream event listener.  Accessed on main thread only.
    struct spa_hook stream_listener;
    // Position control.  Pointer is immutable after being set.
    // Pointee accessed on realtime thread only.
    struct spa_io_position *position;
    // Audio format info.  Accessed only on main thread.
    struct spa_audio_info_raw info;
    // Vchans, explicitly synchronized using message passing.
    struct libvchan *vchan, *closed_vchan;
    // Pointer to the implementation.
    struct impl *impl;
    atomic_size_t current_state, last_state;
    struct spa_source source;
    // Set at creation, immutable afterwards
    size_t buffer_size;
    // Whether the stream is open.  Only accessed on RT thread.
    bool is_open;
    // true for capture, false for playback
    // Set at creation, immutable afterwards
    unsigned direction : 1;
    // Implicitly serialized by PipeWire, in theory at least (so atomic, just in case)
    atomic_bool driving;
    atomic_bool dead;
    // Time information
    uint64_t next_time;
    struct spa_source timer;
    struct rate_match rm;
};

static inline bool
qubes_stream_is_capture(const struct qubes_stream *stream)
{
    return stream->direction;
}

static inline bool
qubes_stream_is_playback(const struct qubes_stream *stream)
{
    return !stream->direction;
}

static int
rate_buff_init(struct rate_match *matcher, uint32_t target_buffer, uint32_t rate)
{
    uint32_t norm_dll_period;
    uint32_t dll_period = 256;
    uint32_t frame_size = 4;

    if (__builtin_mul_overflow(frame_size, dll_period, &norm_dll_period)) {
        return -ERANGE;
    }

    matcher->target_buffer = target_buffer;
    matcher->max_error = norm_dll_period;
    matcher->corr = 1.0;
    spa_dll_init(&matcher->dll);
    spa_dll_set_bw(&matcher->dll, SPA_DLL_BW_MIN, dll_period, rate);
    matcher->period = dll_period;
    return 0;
}

/** For capture, filled is returned by libvchan_data_ready(), for playback
 * it is returned by libvchan_buffer_space(). */
static void update_rate(struct rate_match *rate_match, uint32_t filled, bool driving, bool direction)
{
	double error;

	if (rate_match->rate_match == NULL)
		return;

	error = (double)rate_match->target_buffer - (double)(filled);
	error = SPA_CLAMP(error, -rate_match->max_error, rate_match->max_error);

	rate_match->corr = spa_dll_update(&rate_match->dll, error);
	pw_log_debug("direction:%s error:%f corr:%f current:%u target:%" PRIu32,
		     direction ? "capture" : "playback", error, rate_match->corr, filled, rate_match->target_buffer);

	if (!driving) {
		SPA_FLAG_SET(rate_match->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE);
		rate_match->rate_match->rate = direction ? 1.0 / rate_match->corr : rate_match->corr;
	}
}

static uint64_t
rate_match_duration(struct rate_match *rm, uint64_t duration, uint32_t rate)
{
    spa_assert_se(isfinite(rm->corr) && rm->corr > 0);
    double d = (double)duration / rm->corr * 1e9 / rate;
    if (d > (double)UINT64_MAX || d < 1)
        return duration;
    return (uint64_t)d;
}

struct impl {
    struct pw_context *context;

    struct pw_properties *props;

    struct pw_impl_module *module;

    struct spa_hook module_listener;

    struct pw_core *core;
    struct spa_loop *data_loop, *main_loop;
    struct spa_system *data_system;
    struct spa_hook core_proxy_listener;
    struct spa_hook core_listener;

    struct qubes_stream stream[2];

    qdb_handle_t qdb;
    struct spa_source qdb_watch_source;

    uint32_t frame_size;
    int domid;

    bool do_disconnect;
#if !QUBES_PW_HAS_SCHEDULE_DESTROY
    bool unloading;
    struct pw_work_queue *work;
#endif

    atomic_uint_fast64_t reference_count;

    const char *args;
};

int get_domid_from_props(struct pw_properties *props) {
    size_t domid;
    const char *peer_domain_prop = NULL;
    if ((peer_domain_prop = pw_properties_get(props, QUBES_AUDIOVM_PW_KEY)) == NULL) {
        return -EINVAL;
    }
    if (parse_number(peer_domain_prop, INT_MAX / 2, &domid, "domain ID")) {
        pw_log_debug("Cannot parse domid");
        return -errno;
    }
    return domid;
}

int get_domid_from_qdb(qdb_handle_t qdb) {
    size_t domid;
    int res;
    char *qdb_entry = qdb_read(qdb, QUBES_AUDIOVM_QUBESDB_ENTRY, NULL);

    if (qdb_entry != NULL) {
        if (parse_number(qdb_entry, INT_MAX / 2, &domid, "domain ID")) {
            pw_log_error("Cannot parse domid");
            res = -errno;
        } else {
            res = (int)domid;
        }
    } else {
        res = -errno;
        if (res == -ENOENT)
            pw_log_error("no %s entry in QubesDB", QUBES_AUDIOVM_QUBESDB_ENTRY);
        else
            pw_log_error("unable to obtain %s entry from QubesDB", QUBES_AUDIOVM_QUBESDB_ENTRY);
    }

    free(qdb_entry);

    return res;
}

static inline void
impl_incref(struct impl *impl)
{
    atomic_fetch_add(&impl->reference_count, 1);
}

#if !QUBES_PW_HAS_SCHEDULE_DESTROY
static void do_unload_module(void *obj, void *data, int res, uint32_t id)
{
    struct impl *impl = data;
    pw_impl_module_destroy(impl->module);
}
#endif

static void unload_module(struct impl *impl)
{
#if QUBES_PW_HAS_SCHEDULE_DESTROY
    pw_impl_module_schedule_destroy(impl->module);
#else
    if (!impl->unloading) {
        impl->unloading = true;
        pw_work_queue_add(impl->work, impl, 0, do_unload_module, impl);
    }
#endif
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
        spa_loop_remove_source(stream->impl->data_loop, &stream->source);
        stream->closed_vchan = stream->vchan;
        stream->vchan = NULL;
    }
    stream->is_open = false;
    stream->source.fd = -1;
    // Update the main-thread state asynchronously, but only if the stream
    // is not being torn down.
    if (!stream->dead) {
        impl_incref(stream->impl);
        spa_loop_invoke(stream->impl->main_loop,
                        vchan_error_callback, 0, NULL, 0, false,
                        stream);
    }
}

#ifndef SPA_SCALE32_UP
#define SPA_SCALE32_UP(val,num,denom)				\
({								\
    uint64_t _val = (val);					\
    uint64_t _denom = (denom);					\
    (uint32_t)(((_val) * (num) + (_denom)-1) / (_denom));	\
})
#endif

/* Set the timer.  Must be called on the realtime thread. */
static void set_timeout(struct qubes_stream *stream, uint64_t time)
{
    struct impl *impl = stream->impl;
    struct itimerspec its;

    spa_zero(its);
    its.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
    its.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
    spa_assert_se(spa_system_timerfd_settime(impl->data_system,
                                             stream->timer.fd,
                                             SPA_FD_TIMER_ABSTIME, &its, NULL) >= 0);
}

static void trigger_process(struct qubes_stream *stream, uint64_t expirations)
{
    uint64_t duration, current_time;
    uint32_t rate;
    int32_t avail;
    if (!stream->driving) {
        return;
    }

    libvchan_t *vchan = stream->vchan;
    struct spa_io_position *pos = stream->position;

    if (SPA_LIKELY(pos)) {
#if QUBES_PW_HAS_TARGET
        duration = pos->clock.target_duration;
        rate = pos->clock.target_rate.denom;
#else
        duration = pos->clock.duration;
        rate = pos->clock.rate.denom;
#endif
    } else {
        duration = 1024;
        rate = QUBES_STREAM_RATE;
    }
    pw_log_trace("timeout %" PRIu64, duration);

    current_time = stream->next_time;
    double d = rate_match_duration(&stream->rm, duration, rate);
    pw_log_debug("direction:%s new duration:%f target duration:%" PRIu64 " target rate:%" PRIu32, stream->direction ? "capture" : "playback", d, duration, rate);
    stream->next_time += d;

    if (qubes_stream_is_capture(stream)) {
        avail = vchan ? SPA_MAX(libvchan_data_ready(vchan), 0) : 0;
    } else {
        avail = vchan ? (int32_t)stream->buffer_size - SPA_MAX(libvchan_buffer_space(vchan), 0) : 0;
    }

    if (SPA_LIKELY(pos)) {
        pos->clock.nsec = current_time;
        pos->clock.position += pos->clock.duration;
#if QUBES_PW_HAS_TARGET
        pos->clock.rate = pos->clock.target_rate;
        pos->clock.duration = pos->clock.target_duration;
#endif
        pos->clock.delay = (int64_t)SPA_SCALE32_UP(avail, rate, QUBES_STREAM_RATE) * (qubes_stream_is_capture(stream) ? 1 : -1);
        pos->clock.rate_diff = 1;
        pos->clock.next_nsec = stream->next_time;
    }
    set_timeout(stream, stream->next_time);
    pw_stream_trigger_process(stream->stream);
}

static void
on_timeout_fd(struct spa_source *source)
{
    struct qubes_stream *stream = SPA_CONTAINER_OF(source, struct qubes_stream, timer);
    struct impl *impl = stream->impl;
    uint64_t expirations;
    int res = spa_system_timerfd_read(impl->data_system, source->fd, &expirations);

    if (SPA_UNLIKELY(res < 0)) {
            if (res != -EAGAIN)
                    pw_log_error("%p: failed to read timer fd:%d: %s",
                                 impl, source->fd, spa_strerror(res));
            return;
    }

    trigger_process(stream, expirations);
}


static int add_timeout_cb(struct spa_loop *loop,
                          bool async,
                          uint32_t seq,
                          const void *data,
                          size_t size,
                          void *user_data)
{
    struct impl *impl = user_data;
    int timerfd, res;

    spa_assert_se(loop == impl->data_loop);
    for (int i = 0; i < 2; ++i)
        impl->stream[i].timer.fd = -1;

    for (int i = 0; i < 2; ++i) {
        timerfd = spa_system_timerfd_create(impl->data_system, CLOCK_MONOTONIC,
                                            SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
        if (timerfd < 0) {
            spa_assert_se(timerfd != -EPIPE);
            return timerfd;
        }
        impl->stream[i].timer = (struct spa_source) {
            .loop = loop,
            .func = on_timeout_fd,
            .fd = timerfd,
            .mask = SPA_IO_IN,
        };
        if ((res = spa_loop_add_source(loop, &impl->stream[i].timer)) < 0)
            return res;
        impl->stream[i].timer.data = &impl->stream[i];
    }
    timerfd = spa_system_timerfd_create(impl->data_system, CLOCK_MONOTONIC,
                                        SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
    if (timerfd < 0) {
        spa_assert_se(timerfd != -EPIPE);
        return timerfd;
    }
    return 0;
}

static int remove_stream_cb(struct spa_loop *loop,
                            bool async,
                            uint32_t seq,
                            const void *data,
                            size_t size,
                            void *user_data)
{
    struct qubes_stream *stream = user_data;

    stop_watching_vchan(stream);
    struct impl *impl = stream->impl;
    if (stream->timer.data != NULL) {
        spa_assert_se(stream->timer.loop == impl->data_loop);
        spa_assert_se(spa_loop_remove_source(impl->data_loop, &stream->timer) >= 0);
        stream->timer.data = NULL;
        spa_assert_se(spa_system_close(impl->data_system, stream->timer.fd) >= 0);
        stream->timer.fd = -1;
    }
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
    spa_assert_se(stream->closed_vchan);
    spa_assert_se(!stream->vchan);
    stream->vchan = stream->closed_vchan;
    stream->closed_vchan = NULL;
    stream->source.loop = stream->impl->data_loop;
    stream->source.func = vchan_ready;
    stream->source.data = stream;
    stream->source.fd = libvchan_fd_for_select(stream->vchan);
    stream->source.mask = SPA_IO_IN;
    set_timeout(stream, 0);
    return spa_loop_add_source(loop, &stream->source);
}

/**
 * Connect a Qubes stream.  Must be called on the main thread.
 *
 * @param stream The stream to connect.
 */
static int connect_stream(struct qubes_stream *stream)
{
    const char *msg = qubes_stream_is_capture(stream) ? "capture" : "playback";
    int32_t domid = (int32_t)stream->impl->domid;
    int status;
    if (domid<0) {
        pw_log_warn("unknown peer domain, cannot create stream");
        return 0;
    }
    pw_log_info("module %p: new (%s), peer id is %" PRIi32, stream->impl, stream->impl->args, domid);

    spa_assert_se(stream->vchan == NULL);
    spa_assert_se(stream->closed_vchan == NULL);
    if (qubes_stream_is_capture(stream)) { // capture
        stream->closed_vchan = libvchan_server_init((int)domid, QUBES_PA_SOURCE_VCHAN_PORT, stream->buffer_size, 128);
    } else { // playback
        stream->closed_vchan = libvchan_server_init((int)domid, QUBES_PA_SINK_VCHAN_PORT, 128, stream->buffer_size);
        if (stream->closed_vchan != NULL) {
            int ready = libvchan_buffer_space(stream->closed_vchan);
            if (ready > 0) {
                stream->buffer_size = (uint32_t)ready;
            }
        }
    }
    if (stream->closed_vchan == NULL) {
        pw_log_error("can't create %s vchan, audio will not work", msg);
        return -EPROTO; // FIXME: better error
    }
    do {
        status = spa_loop_invoke(stream->impl->data_loop, add_stream, 0, NULL, 0, true, stream);
    } while (status == -EPIPE);
    if (status) {
        errno = -status;
        pw_log_error("spa_loop_add_source() failed (%m), audio will not work");
        return status;
    }
    return 0;
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
static void stream_destroy(void *data)
{
    struct qubes_stream *stream = data;
    int status;

    /* Log that the stream is being destroyed, for debugging purposes. */
    pw_log_debug("%p: destroy", data);

    /* Mark the stream as dead, so that future destroy attempts are no-ops. */
    if (stream->dead)
        return;
    stream->dead = true;

    /* Notify the realtime thread of the shutdown.  This ensures that the
     * realtime thread will no longer use any resources that have been torn
     * down. */
    do {
        status = spa_loop_invoke(stream->impl->data_loop,
                                 remove_stream_cb,
                                 0, NULL, 0, true, stream);
    } while (status != 0);

    /* Set the PipeWire stream to NULL, as it is being destroyed and is thus not
     * valid to use any more. */
    stream->stream = NULL;

    stream_shutdown(stream);

    /* Remove the listener hook */
    spa_hook_remove(&stream->stream_listener);

    /* Free the stream properties */
    if (stream->stream_props != NULL)
        pw_properties_free(stream->stream_props);
}

/** Called on the main thread to set the stream state */
static void set_stream_state(struct qubes_stream *stream, bool state)
{
    static_assert(atomic_is_lock_free(&stream->current_state),
                  "PipeWire agent requires lock-free atomic size_t");
    static_assert(atomic_is_lock_free(&stream->driving),
                  "PipeWire agent requires lock-free atomic bool");
    stream->current_state = state;
}

static const struct pw_stream_events capture_stream_events, playback_stream_events;

static void impl_decref(struct impl *impl)
{
    uint_fast64_t old_refcount = atomic_fetch_sub(&impl->reference_count, 1);
    spa_assert_se(old_refcount >= 1);
    if (old_refcount > 1)
        return;
    if (impl->stream[PW_DIRECTION_OUTPUT].stream != NULL)
        pw_stream_destroy(impl->stream[PW_DIRECTION_OUTPUT].stream);
    else
        stream_destroy(&impl->stream[PW_DIRECTION_OUTPUT]);
    if (impl->stream[PW_DIRECTION_INPUT].stream != NULL)
        pw_stream_destroy(impl->stream[PW_DIRECTION_INPUT].stream);
    else
        stream_destroy(&impl->stream[PW_DIRECTION_INPUT]);
    if (impl->core != NULL) {
        spa_hook_remove(&impl->core_proxy_listener);
        spa_hook_remove(&impl->core_listener);
        if (impl->do_disconnect)
            pw_core_disconnect(impl->core);
    }
    pw_properties_free(impl->props);
    pw_log_debug("%p: free", (void *)impl);
#if !QUBES_PW_HAS_SCHEDULE_DESTROY
    if (impl->work != NULL)
        pw_work_queue_cancel(impl->work, impl, SPA_ID_INVALID);
#endif
    free(impl);
}

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

    spa_assert_se(!stream->vchan);
    stream_shutdown(stream);
    if (!stream->dead && stream->stream != NULL)
        connect_stream(stream);
    impl_decref(stream->impl);
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

    struct qubes_stream *stream = user_data;
    uint32_t n_params = 0;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = { 0 };

    if (stream->dead || stream->stream == NULL)
        return 0;
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    params[n_params++] = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &stream->info);

    if (pw_stream_connect(stream->stream,
            qubes_stream_is_capture(stream) ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
            PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS,
            params, n_params) < 0)
        pw_log_error("Could not connect stream: %m");
    impl_decref(stream->impl);
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

static const struct spa_audio_info_raw qubes_audio_format = {
    .format = SPA_AUDIO_FORMAT_S16_LE,
    .flags = 0,
    .rate = QUBES_STREAM_RATE,
    .channels = 2,
    .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
};


static uint64_t get_time_ns(struct impl *impl)
{
    struct timespec now;
    if (spa_system_clock_gettime(impl->data_system, CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return SPA_TIMESPEC_TO_NSEC(&now);
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
        spa_assert_se(!stream->is_open && "no vchan on open stream?");
        pw_log_error("vchan_ready() called with vchan closed???");
        return;
    }

    // 1: Acknowledge vchan event
    pw_log_trace("Waiting for vchan");
    libvchan_wait(stream->vchan);
    pw_log_trace("Vchan awaited");

    // 2: Figure out if stream is open
    bool is_open = libvchan_is_open(stream->vchan);
    if (is_open != stream->is_open) {
        if (is_open) {
            // vchan connected
            impl_incref(stream->impl);
            spa_loop_invoke(stream->impl->main_loop,
                            main_thread_connect, 0, NULL, 0, false, stream);
            if (qubes_stream_is_capture(stream)) {
                // vchan just opened, no need to check for buffer space
                const uint32_t control_commands[3] = {
                    0x00010004,
                    (stream->last_state = stream->current_state) ?
                        QUBES_PA_SOURCE_START_CMD : QUBES_PA_SOURCE_STOP_CMD,
                    (playback_stream->last_state = playback_stream->current_state) ?
                        QUBES_PA_SINK_UNCORK_CMD : QUBES_PA_SINK_CORK_CMD,
                };
                if (libvchan_send(stream->vchan, control_commands, sizeof control_commands) !=
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
    if (qubes_stream_is_capture(stream)) {
        discard_unwanted_recorded_data(stream);
        process_control_commands(stream->impl);
    }
}

static int vchan_reconnect_cb(struct spa_loop *loop,
                                bool async,
                                uint32_t seq,
                                const void *data,
                                size_t size,
                                void *user_data)
{
    struct qubes_stream *stream = user_data;
    stop_watching_vchan(stream);
    return 0;
}

static void qdb_cb(struct spa_source *source)
{
    struct impl *impl = source->data;
    int domid;

    pw_log_debug("Received event from QubesDB");

    domid = get_domid_from_qdb(impl->qdb);

    if (domid != impl->domid) {
        if (domid >= 0)
            pw_log_info("Setting new peer domain ID %d", domid);
        else if (domid == -ENOENT) // not an error, AudioVM unset
            pw_log_info("AudioVM unset, disconnecting from %d", impl->domid);
        else {
            errno = -domid;
            pw_log_error("Cannot obtain new peer domain ID (%m), disconnecting from %d", impl->domid);
        }
        impl->domid = domid;
        for (int i = 0; i < 2; ++i) {
            if (impl->stream[i].source.fd != -1)
                pw_log_info("Closing %d", impl->stream[i].source.fd);
            spa_loop_invoke(impl->data_loop, vchan_reconnect_cb, 0, NULL, 0, true, &impl->stream[i]);
        }
    }
}

static int add_qdb_cb(struct spa_loop *loop, struct impl *impl)
{
    int qdb_fd;
    if (!qdb_watch(impl->qdb, QUBES_AUDIOVM_QUBESDB_ENTRY)) {
        int err = -errno;
        pw_log_error("Failed to setup watch on %s: %m\n", QUBES_AUDIOVM_QUBESDB_ENTRY);
        return err;
    }

    qdb_fd = qdb_watch_fd(impl->qdb);
    if (qdb_fd < 0)
        return -EPIPE;

    impl->qdb_watch_source.loop = loop;
    impl->qdb_watch_source.mask = SPA_IO_IN;
    impl->qdb_watch_source.data = impl;
    impl->qdb_watch_source.fd = qdb_fd;
    impl->qdb_watch_source.func = qdb_cb;

    return spa_loop_add_source(loop, &impl->qdb_watch_source);
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
    pw_log_trace("Discarding %d bytes of unwanted data", ready);
    while (to_read > 0) {
        int res = libvchan_read(stream->vchan, buf, SPA_MIN(to_read, sizeof(buf)));
        if (res <= 0)
            return;
        to_read -= (size_t)res;
    }
}

static void rt_set_stream_state(struct qubes_stream *stream, bool active)
{
    pw_log_trace("Setting %s state to %s", qubes_stream_is_capture(stream) ? "capture" : "playback", active ? "started" : "stopped");
    if (active) {
        bool driving = pw_stream_is_driving(stream->stream);
        if (driving && !stream->driving) {
            stream->next_time = get_time_ns(stream->impl);
            set_timeout(stream, stream->next_time);
        }
        stream->driving = driving;
    } else {
        stream->next_time = 0;
        stream->driving = false;
        set_timeout(stream, 0);
    }

    stream->last_state = active;
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

        int res = libvchan_send(control_vchan, &cmd, sizeof(cmd));
        if (res != (int)sizeof(cmd)) {
            pw_log_error("error writing command to control vchan: got %d, expected %zu",
                         res, sizeof(cmd));
            return -EPROTO;
        }

        pw_log_trace("Audio playback %s", new_state ? "started" : "stopped");
        rt_set_stream_state(playback_stream, new_state);
    }

    new_state = capture_stream->current_state;
    if (new_state != capture_stream->last_state) {
        uint32_t cmd = new_state ? QUBES_PA_SOURCE_START_CMD : QUBES_PA_SOURCE_STOP_CMD;

        if (libvchan_buffer_space(control_vchan) < (int)sizeof(cmd)) {
            pw_log_error("cannot write command to control vchan: no buffer space");
            return -ENOSPC;
        }

        int res = libvchan_send(control_vchan, &cmd, sizeof(cmd));
        if (res != (int)sizeof(cmd)) {
            pw_log_error("error writing command to control vchan: got %d, expected %zu",
                         res, sizeof(cmd));
            return -EPROTO;
        }

        pw_log_trace("Audio capturing %s", new_state ? "started" : "stopped");

        if (pw_stream_is_driving(capture_stream->stream)) {
            if (new_state)
                pw_log_trace("Qubes OS capture node is driving");
        }
        rt_set_stream_state(capture_stream, new_state);
    }

    return 0;
}

struct slice {
    void *data;
    size_t size;
};

static int rt_set_io(struct spa_loop *loop,
                     bool async,
                     uint32_t seq,
                     const void *data,
                     size_t size,
                     void *user_data)
{
    struct qubes_stream *stream = user_data;
    const struct slice *slice;

    spa_assert_se(size == sizeof(*slice));
    slice = data;
    switch (seq) {
        case SPA_IO_Position:
            if (slice->data != NULL) {
                spa_assert_se(slice->size >= sizeof *stream->position);
                spa_assert_se(SPA_IS_ALIGNED(slice->data, alignof(struct spa_io_position)));
            }
            stream->position = slice->data;
            break;
        case SPA_IO_RateMatch:
            if (slice->data != NULL) {
                spa_assert_se(slice->size >= sizeof(*stream->rm.rate_match));
                spa_assert_se(SPA_IS_ALIGNED(slice->data,
                                             alignof(typeof(*stream->rm.rate_match))));
            }
            stream->rm.rate_match = slice->data;
            break;
    }
    return 0;
}

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
    struct qubes_stream *stream = data;
    int status;
    struct slice slice = { .data = area, .size = size };

    do {
        status = spa_loop_invoke(stream->impl->data_loop, rt_set_io,
                id, &slice, sizeof slice, true, stream);
    } while (status == -EPIPE);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
                                 enum pw_stream_state state, const char *error)
{
    struct qubes_stream *stream = data;
    struct impl *impl = stream->impl;
    const char *const name = qubes_stream_is_playback(stream) ? "playback" : "capture";

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        pw_log_error("%s error: %s", name, error ? error : "(null)");
        set_stream_state(stream, false);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        pw_log_debug("%s unconnected", name);
        set_stream_state(stream, false);
        break;
    case PW_STREAM_STATE_CONNECTING:
        pw_log_debug("%s connected", name);
        return;
    case PW_STREAM_STATE_PAUSED:
        pw_log_debug("%s paused", name);
        if (!qubes_stream_is_playback(stream))
            set_stream_state(stream, false);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_log_debug("%s streaming", name);
        set_stream_state(stream, true);
        break;
    default:
        pw_log_error("unknown %s stream state %d", name, state);
        return;
    }
    spa_loop_invoke(impl->data_loop, process_control_commands_cb, 0, NULL, 0, true, impl);
    pw_log_debug("Successfully queued message");
}

static void capture_stream_process(void *d)
{
    struct qubes_stream *stream = d;
    struct pw_buffer *b;
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

    pw_log_debug("space to read %" PRIu32 " bytes, target is %" PRIu32, bytes_ready, stream->rm.target_buffer);

    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas < 1 || (dst = buf->datas[0].data) == NULL)
        goto done;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = 4;
    buf->datas[0].chunk->size = 0;

    spa_assert_se(buf->n_datas == 1 && "wrong number of datas");

    // TODO: handle more data
#if PW_CHECK_VERSION(0, 3, 49)
    if (__builtin_mul_overflow(b->requested ? b->requested : 2048, stream->impl->frame_size, &size)) {
        pw_log_error("Overflow calculating amount of data there is room for????");
        goto done;
    }
    size = SPA_MIN(size, buf->datas[0].maxsize);
#else
    size = buf->datas[0].maxsize;
#endif
    buf->datas[0].chunk->size = size;

    if (size > bytes_ready) {
        pw_log_warn("Underrun: asked to read %" PRIu32 " bytes, but only %d available", size, (int)bytes_ready);
        memset(dst + bytes_ready, 0, size - bytes_ready);
        size = bytes_ready;
    }

    pw_log_trace("reading %" PRIu32 " bytes from vchan", size);
    bool something_to_read = size > 0;
    if (something_to_read) {
        int r = libvchan_recv(stream->vchan, dst, size);
        if (r < 0) {
            pw_log_error("vchan error: got %d", r);
        } else if (r == 0) {
            pw_log_error("premature EOF on vchan or protocol violation by peer");
        } else {
            spa_assert_se(size == (unsigned int)r);
            dst += r;
            size = 0;
            update_rate(&stream->rm, bytes_ready, pw_stream_is_driving(stream->stream), stream->direction);
        }
    }
    // avoid recording uninitialized memory
    memset(dst, 0, size);
done:
    pw_stream_queue_buffer(stream->stream, b);
}

static void playback_stream_process(void *d)
{
    struct qubes_stream *stream = d;
    struct pw_buffer *buf;
    struct impl *impl = stream->impl;
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

    pw_log_debug("space to write %d bytes, target is %" PRIu32, ready, stream->rm.target_buffer);

    if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
        pw_log_error("out of buffers: %m");
        return;
    }

    spa_assert_se(buf->buffer->n_datas == 1 && "wrong number of datas");

    bd = &buf->buffer->datas[0];
    spa_assert_se(bd->chunk->offset == 0);
    data = bd->data + bd->chunk->offset;
    size = bd->chunk->size;

    if (ready < 0) {
        pw_log_error("Negative return value from libvchan_buffer_space()");
        return;
    }

    update_rate(&stream->rm, (uint32_t)ready, pw_stream_is_driving(stream->stream), stream->direction);

    if (size > (uint32_t)ready) {
        pw_log_warn("Overrun: asked to write %" PRIu32 " bytes, but can only write %d", size, ready);
        process_control_commands(impl);
        size = (uint32_t)ready;
    }

    pw_log_trace("writing %" PRIu32 " bytes to vchan", size);
    if (size > 0) {
        errno = 0;
        if (libvchan_send(stream->vchan, data, size) != (int)size)
            pw_log_error("vchan error: %m");
    }
    pw_stream_queue_buffer(stream->stream, buf);
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct qubes_stream *stream = data;
    struct impl *impl = stream->impl;
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
    case SPA_PARAM_Latency:
        /* TODO: latency reporting */
        return;
#if PW_CHECK_VERSION(0, 3, 79)
    case SPA_PARAM_Tag:
        /* TODO: tag reporting */
        return;
#endif
    default:
        pw_log_info("Unknown param ID %" PRIu32, id);
        return;
    }

    if (param == NULL)
        goto doit;

    if ((res = spa_format_parse(param, &media_type, &media_subtype)) < 0) {
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

    if (info.rate != qubes_audio_format.rate) {
        pw_log_error("Unsupported audio rate %" PRIu32, info.rate);
        errno = ENOTSUP;
        return;
    }

    if (info.channels != qubes_audio_format.channels) {
        pw_log_error("Expected %d channels, got %" PRIu32,
                     qubes_audio_format.channels, info.channels);
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
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stream->buffer_size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)impl->frame_size),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr)));
    spa_assert_se(params[0]);

    params[1] = spa_format_audio_raw_build(&b, SPA_PARAM_Format,
            (struct spa_audio_info_raw *)&qubes_audio_format);
    spa_assert_se(params[1]);

    spa_assert_se(b.state.offset <= sizeof params_buffer);

    if ((res = pw_stream_update_params(stream->stream, params, 2)) < 0) {
        errno = -res;
        pw_log_error("Failed to negotiate parameters: %m");
        errno = -res;
    }
}

static void playback_stream_drained(void *data)
{
    set_stream_state(data, false);
}

static const struct pw_stream_events capture_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = stream_destroy,
    .state_changed = stream_state_changed,
    .control_info = NULL,
    .io_changed = stream_io_changed,
    .param_changed = stream_param_changed,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = capture_stream_process,
};

static const struct pw_stream_events playback_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = stream_destroy,
    .state_changed = stream_state_changed,
    .control_info = NULL,
    .io_changed = stream_io_changed,
    .param_changed = stream_param_changed,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = playback_stream_process,
    .drained = playback_stream_drained,
};

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

static void module_destroy(void *data)
{
    struct impl *impl = data;
#if !QUBES_PW_HAS_SCHEDULE_DESTROY
    impl->unloading = true;
#endif
    spa_hook_remove(&impl->module_listener);
    spa_zero(impl->module_listener);
    impl_decref(impl);
}

static const struct pw_impl_module_events module_events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy,
    .free = NULL,
    .initialized = NULL,
    .registered = NULL,
};

static void parse_audio_info(struct impl *impl)
{
    for (uint8_t i = 0; i < 2; ++i) {
        struct spa_audio_info_raw *info = &impl->stream[i].info;

        _Static_assert(SPA_N_ELEMENTS(impl->stream) == 2, "out of bounds bug");
        spa_zero(*info);

        info->format = SPA_AUDIO_FORMAT_S16_LE;
        info->channels = 2;
        info->rate = QUBES_STREAM_RATE;
        _Static_assert(SPA_N_ELEMENTS(info->position) >= 2, "out of bounds bug");
        info->position[0] = SPA_AUDIO_CHANNEL_FL;
        info->position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    impl->frame_size = 4;
}

static const struct spa_dict_item capture_props[] = {
    { PW_KEY_NODE_NAME, "qubes-source" },
    { PW_KEY_NODE_DESCRIPTION, "Qubes Virtual Audio Source" },
    { "node.rate", SPA_STRINGIFY(QUBES_STREAM_RATE) },
    { PW_KEY_NODE_DRIVER, "true" },
    { PW_KEY_PRIORITY_DRIVER, "21500" },
    { PW_KEY_MEDIA_TYPE, "Audio" },
    { PW_KEY_MEDIA_CLASS, "Audio/Source" },
    { PW_KEY_AUDIO_RATE, SPA_STRINGIFY(QUBES_STREAM_RATE) },
    { PW_KEY_AUDIO_CHANNELS, "2" },
    { PW_KEY_AUDIO_FORMAT, "S16LE" },
    { "audio.position", "[ FL FR ]" },
};

static const struct spa_dict capture_dict = SPA_DICT_INIT_ARRAY(capture_props);

static const struct spa_dict_item playback_props[] = {
    { PW_KEY_NODE_NAME, "qubes-sink" },
    { PW_KEY_NODE_DESCRIPTION, "Qubes Virtual Audio Sink" },
    { "node.rate", SPA_STRINGIFY(QUBES_STREAM_RATE) },
    { PW_KEY_NODE_DRIVER, "true" },
    { PW_KEY_PRIORITY_DRIVER, "21499" },
    { PW_KEY_MEDIA_TYPE, "Audio" },
    { PW_KEY_MEDIA_CLASS, "Audio/Sink" },
    { PW_KEY_AUDIO_RATE, SPA_STRINGIFY(QUBES_STREAM_RATE) },
    { PW_KEY_AUDIO_CHANNELS, "2" },
    { PW_KEY_AUDIO_FORMAT, "S16LE" },
    { "audio.position", "[ FL FR ]" },
};

static const struct spa_dict playback_dict = SPA_DICT_INIT_ARRAY(playback_props);



static int
create_stream(struct impl *impl, enum spa_direction direction)
{
    struct qubes_stream *stream = impl->stream + direction;

    int res = rate_buff_init(&stream->rm, stream->rm.target_buffer, QUBES_STREAM_RATE);
    if (res != 0)
        return res;
    stream->stream = pw_stream_new(impl->core,
                                   direction ? "Qubes Source" : "Qubes Sink",
                                   stream->stream_props);
    stream->stream_props = NULL;
    if (stream->stream == NULL)
        return -errno;
    stream->impl = impl;
    stream->direction = direction;
    pw_stream_add_listener(stream->stream,
                           &stream->stream_listener,
                           qubes_stream_is_capture(stream) ? &capture_stream_events : &playback_stream_events,
                           stream);
    return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL, *arg_props = NULL;
    const struct pw_properties *global_props = NULL;
    struct impl *impl;
    const char *str;
    int res = -EFAULT; /* should never be returned, modulo bugs */

#ifdef PW_LOG_TOPIC_INIT
    PW_LOG_TOPIC_INIT(mod_topic);
#endif
    pw_log_info("hello from qubes module");

#if !QUBES_PW_HAS_SCHEDULE_DESTROY
    struct pw_work_queue *queue = pw_context_get_work_queue(context);
    if (queue == NULL)
        return -errno;
#endif
    impl = calloc(1, sizeof(struct impl));
    if (impl == NULL)
        return -errno;

    if (args == NULL)
        args = "";

    impl->args = args;
    impl->module = module;
    impl->context = context;
#if !QUBES_PW_HAS_SCHEDULE_DESTROY
    impl->work = queue;
#endif
    impl->reference_count = 1;
    for (int i = 0; i < 2; ++i) {
        impl->stream[i].timer.fd = -1;
        impl->stream[i].source.fd = -1;
        impl->stream[i].impl = impl;
    }

    {
#if PW_CHECK_VERSION(0, 3, 56)
        struct pw_data_loop *pw_data_loop = pw_context_get_data_loop(context);
        spa_assert_se(pw_data_loop);
        struct pw_loop *data_loop = pw_data_loop_get_loop(pw_data_loop);
        spa_assert_se(data_loop && data_loop->loop && data_loop->system);
        impl->data_loop = data_loop->loop;
        impl->data_system = data_loop->system;
#else
        uint32_t n_support;
        const struct spa_support *support = pw_context_get_support(context, &n_support);
        impl->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
        if (!impl->data_loop) {
            res = -errno;
            pw_log_error("cannot get data loop: %m");
            goto error;
        }
        impl->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
        if (impl->data_system == NULL) {
            res = -errno;
            pw_log_error("could not obtain data system: %m");
            goto error;
        }
#endif
    }

    {
        struct pw_loop *loop = pw_context_get_main_loop(context);
        spa_assert_se(loop && loop->loop);
        impl->main_loop = loop->loop;
    }

    do {
        res = spa_loop_invoke(impl->data_loop, add_timeout_cb, 0, NULL, 0, true, impl);
    } while (res == -EPIPE);
    if (res != 0) {
        errno = -res;
        pw_log_error("can't create timer: %m");
        goto error;
    }

    if (!(global_props = pw_context_get_properties(context))) {
        res = errno ? -errno : -ENOMEM;
        pw_log_error("cannot obtain properties: %m");
        goto error;
    }

    if (!(props = pw_properties_copy(global_props))) {
        res = errno ? -errno : -ENOMEM;
        pw_log_error("cannot clone properties: %m");
        goto error;
    }
    impl->props = props;

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

    {
        size_t read_min = 0x100000, write_min = 0x10000;
        const char *record_size = pw_properties_get(props, QUBES_PW_KEY_RECORD_BUFFER_SPACE);
        const char *record_buffer_fill = pw_properties_get(props, QUBES_PW_KEY_CAPTURE_TARGET_BUFFER_FILL);
        const char *playback_buffer_fill = pw_properties_get(props, QUBES_PW_KEY_PLAYBACK_TARGET_BUFFER_FILL);
        const char *playback_size = pw_properties_get(props, QUBES_PW_KEY_PLAYBACK_BUFFER_SPACE);
        if (!record_size || !playback_size) {
            const char *buffer_size = pw_properties_get(props, QUBES_PW_KEY_BUFFER_SPACE);
            if (!record_size)
                record_size = buffer_size;
            if (!playback_size)
                playback_size = buffer_size;
        }

        if (record_size &&
            (res = parse_number(record_size, 1UL << 20, &read_min, "record buffer size")))
            goto error;

        if (playback_size &&
            (res = parse_number(playback_size, 1UL << 20, &write_min, "playback buffer size")))
            goto error;
        read_min = SPA_UNLIKELY(read_min < 1024) ? 1024 : (size_t)1 << ((sizeof(unsigned long) * 8) - __builtin_clzl(read_min - 1));
        write_min = SPA_UNLIKELY(write_min < 1024) ? 1024 : (size_t)1 << ((sizeof(unsigned long) * 8) - __builtin_clzl(write_min - 1));

        size_t record_target_buffer_fill = SPA_MIN(0x2000U, read_min / 2);
        size_t playback_target_buffer_fill = SPA_MIN(0x2000U, write_min / 2);

        if (record_buffer_fill != NULL) {
            res = parse_number(record_buffer_fill, read_min, &record_target_buffer_fill, "record target buffer fill");
            if (res != 0)
                goto error;
        }

        if (playback_buffer_fill != NULL) {
            res = parse_number(playback_buffer_fill, write_min, &playback_target_buffer_fill, "playback target buffer fill");
            if (res != 0)
                goto error;
        }

        impl->stream[PW_DIRECTION_OUTPUT].buffer_size = read_min;
        impl->stream[PW_DIRECTION_OUTPUT].rm.target_buffer = record_target_buffer_fill;
        impl->stream[PW_DIRECTION_INPUT].buffer_size = write_min;
        impl->stream[PW_DIRECTION_INPUT].rm.target_buffer = write_min - playback_target_buffer_fill;
    }

    pw_log_info("module %p: record buffer size 0x%zx, playback buffer size 0x%zx",
                impl,
                impl->stream[PW_DIRECTION_OUTPUT].buffer_size,
                impl->stream[PW_DIRECTION_INPUT].buffer_size);

    for (uint8_t i = 0; i < 2; ++i) {
        const char *msg = i ? "capture" : "playback";
        // i = 0: playback
        // i = 1: capture
        const struct spa_dict *dict = i ? &capture_dict : &playback_dict;
        struct qubes_stream *stream = impl->stream + i;
        stream->stream_props = pw_properties_new_dict(dict);
        if (stream->stream_props == NULL) {
            res = -errno;
            pw_log_error("can't create %s properties: %m", msg);
            goto error;
        }
    }

    if ((str = pw_properties_get(arg_props, "stream.source.props")) != NULL)
        if (pw_properties_update_string(impl->stream[PW_DIRECTION_OUTPUT].stream_props, str, strlen(str)) < 0)
            goto error;

    if ((str = pw_properties_get(arg_props, "stream.sink.props")) != NULL)
        if (pw_properties_update_string(impl->stream[PW_DIRECTION_INPUT].stream_props, str, strlen(str)) < 0)
            goto error;

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

    if ((res = pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props))) < 0)
        goto error;

    {
        int domid_from_props = get_domid_from_props(props);
        // If domid is not specified from pipewire module context,
        // then start the qubesdb watcher
        if (domid_from_props<0) {
            qdb_handle_t qdb = qdb_open(NULL);
            if (!qdb) {
                res = -errno;
                pw_log_error("Could not open QubesDB");
                goto error;
            }
            impl->qdb = qdb;
            if ((res = add_qdb_cb(impl->main_loop, impl) != 0)) {
                errno = -res;
                pw_log_error("can't create qubesdb watcher: %m");
                goto error;
            }
            impl->domid = get_domid_from_qdb(impl->qdb);
        }
    }

    for (uint8_t i = 0; i < 2; ++i)
        if ((res = connect_stream(&impl->stream[i])) != 0)
            goto error;

    pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

    return 0;

error:
    if (arg_props != NULL)
        pw_properties_free(arg_props);
    impl_decref(impl);
    return res;
}
