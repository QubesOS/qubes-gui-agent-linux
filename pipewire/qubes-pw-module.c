/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2022  Demi Marie Obenour  <demi@invisiblethingslab.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>

#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/pod.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/log.h>

#ifdef NDEBUG
#error "Qubes PipeWire module requires assertions"
#endif
#include <assert.h>

#include <libvchan.h>
#include <qubesdb-client.h>

/** \page page_module_example_sink PipeWire Module: Example Sink
 */

#define NAME "qubes-virtual-audio"

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
                        MODULE_EXTRA_USAGE


static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>, "
                            "Demi Marie Obenour <demi@invisiblethingslab.com>" },
    { PW_KEY_MODULE_DESCRIPTION, "Qubes OS audio device" },
    { PW_KEY_MODULE_USAGE, MODULE_USAGE },
    { PW_KEY_MODULE_VERSION, "4.1.0" },
};

struct qubes_stream {
    struct pw_properties *stream_props;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_audio_info_raw info;
    struct libvchan *vchan;
    _Atomic size_t current_state, last_state;
};

struct impl {
    struct pw_context *context;

    struct pw_properties *props;

    struct pw_impl_module *module;

    struct spa_hook module_listener;

    struct pw_core *core;
    struct spa_loop *data_loop;
    struct spa_hook core_proxy_listener;
    struct spa_hook core_listener;

    struct qubes_stream stream[2];

    uint32_t frame_size;

    unsigned int do_disconnect:1;
    unsigned int unloading:1;
};

static void unload_module(struct impl *impl)
{
    if (!impl->unloading) {
        impl->unloading = true;
        pw_impl_module_destroy(impl->module);
    }
}

_Static_assert(PW_DIRECTION_INPUT == 0, "wrong PW_DIRECTION_INPUT");
_Static_assert(PW_DIRECTION_OUTPUT == 1, "wrong PW_DIRECTION_OUTPUT");

static void stream_destroy(struct impl *impl, enum spa_direction direction)
{
    struct qubes_stream *stream = impl->stream + direction;
    if (stream->stream) {
        spa_hook_remove(&stream->stream_listener);
        pw_stream_destroy(stream->stream);
        stream->stream = NULL;
    }
    if (stream->stream_props)
        pw_properties_free(stream->stream_props);
    stream->stream_props = NULL;
    if (stream->vchan)
        libvchan_close(stream->vchan);
    stream->vchan = NULL;
}

static void capture_stream_destroy(void *d)
{
    stream_destroy(d, PW_DIRECTION_INPUT);
}

static void playback_stream_destroy(void *d)
{
    stream_destroy(d, PW_DIRECTION_OUTPUT);
}

static void set_stream_state(struct qubes_stream *stream, bool state)
{
    stream->current_state = state;
}

static int process_control_commands(struct spa_loop *loop,
                                    bool async,
                                    uint32_t seq,
                                    const void *data,
                                    size_t size,
                                    void *user_data)
{
    struct impl *impl = user_data;
    struct qubes_stream *capture_stream = impl->stream + PW_DIRECTION_OUTPUT,
                       *playback_stream = impl->stream + PW_DIRECTION_INPUT;
    bool new_state = playback_stream->current_state;
    struct libvchan *control_vchan = capture_stream->vchan;
    if (new_state != playback_stream->last_state) {
        uint32_t cmd = new_state ? QUBES_PA_SINK_UNCORK_CMD : QUBES_PA_SINK_CORK_CMD;

        if (libvchan_buffer_space(control_vchan) < (int)sizeof(cmd)) {
            pw_log_error("cannot write command to control vchan: no buffer space");
            return -ENOSPC;
        }

        if (libvchan_send(control_vchan, &cmd, sizeof(cmd)) != sizeof(cmd)) {
            pw_log_error("error writing command to control vchan");
            return -ENOSPC;
        }

        pw_log_error("Audio playback %s", new_state ? "started" : "stopped");

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
        pw_log_error("%s unconnected", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], false);
        break;
    case PW_STREAM_STATE_CONNECTING:
        pw_log_error("%s connected", name);
        return;
    case PW_STREAM_STATE_PAUSED:
        pw_log_error("%s paused", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], false);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_log_error("%s streaming", name);
        set_stream_state(&impl->stream[playback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT], true);
        break;
    default:
        pw_log_error("unknown %s stream state %d", name, state);
        return;
    }
    spa_loop_invoke(impl->data_loop, process_control_commands, 0, NULL, 0, true, impl);
    pw_log_error("Successfully queued message");
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
    int ready = libvchan_data_ready(stream->vchan);

    if (!stream->last_state)
        return; // Nothing to do

    if (ready <= 0) {
        pw_log_error("no data in vchan");
        return;
    }

    if ((b = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
        pw_log_error("out of capture buffers: %m");
        return;
    }

    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas < 1 || (dst = buf->datas[0].data) == NULL)
        return;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = 4;
    buf->datas[0].chunk->size = 0;

    assert(buf->n_datas == 1 && "wrong number of datas");

    uint32_t maxsize = buf->datas[0].maxsize;
#if PW_CHECK_VERSION(0, 3, 52) // Fedora 35
    uint32_t size = b->requested ? b->requested : impl->frame_size * maxsize;
    assert(size <= impl->frame_size * maxsize);
#else
    uint32_t size = impl->frame_size * maxsize;
#endif

    if (ready < 0 || size > (uint32_t)ready) {
        pw_log_error("Underrun: asked to read %" PRIu32 " bytes, but only %d available", size, ready);
        if (ready <= 0)
            return;
        size = ready;
    }

    pw_log_debug("reading %" PRIu32 " bytes from vchan", size);
    if (libvchan_read(stream->vchan, dst, size) != (int)size) {
        pw_log_error("vchan error: %m");
        return;
    }

    buf->datas[0].chunk->size = size;
    pw_stream_queue_buffer(stream->stream, b);
}

static void playback_stream_process(void *d)
{
    struct impl *impl = d;
    struct pw_buffer *buf;
    struct qubes_stream *stream = impl->stream + PW_DIRECTION_INPUT;
    struct spa_data *bd;
    uint8_t *data;
    uint32_t size;
    int ready = libvchan_buffer_space(stream->vchan);

    if (!stream->last_state)
        return; // Nothing to do

    if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
        pw_log_error("out of buffers: %m");
        return;
    }

    assert(buf->buffer->n_datas == 1 && "wrong number of datas");

    bd = &buf->buffer->datas[0];
    assert(bd->chunk->offset == 0);
    data = bd->data + bd->chunk->offset;
    size = bd->chunk->size;

    if (ready <= 0 || size > (uint32_t)ready) {
        pw_log_error("Overrun: asked to write %" PRIu32 " bytes, but can only write %d", size, ready);
        if (ready <= 0)
            return;
        size = ready;
    }

    pw_log_debug("writing %" PRIu32 " bytes to vchan", size);
    if (libvchan_write(stream->vchan, data, size) != (int)size) {
        pw_log_error("vchan error: %m");
        return;
    }
    pw_stream_queue_buffer(stream->stream, buf);
}

static const struct pw_stream_events capture_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = capture_stream_destroy,
    .state_changed = capture_stream_state_changed,
    .control_info = NULL,
    .io_changed = NULL,
    .param_changed = NULL,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = capture_stream_process,
    .drained = NULL,
};

static void playback_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct impl *impl = data;
    uint32_t media_type = UINT32_MAX, media_subtype = UINT32_MAX;
    struct spa_audio_info_raw info = { 0 };
    const struct spa_pod *params = NULL;
    int res;

    if (id != SPA_PARAM_Format) {
        pw_log_error("Unknown id %" PRIu32, id);
        return;
    }

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
    if ((res = pw_stream_update_params(impl->stream[PW_DIRECTION_INPUT].stream, &params, 0)) < 0) {
        errno = -res;
        pw_log_error("Failed to negotiate parameters: %m");
        errno = -res;
    }
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
    int res;
    uint32_t n_params;
    const struct spa_pod *params[1];
    struct qubes_stream *stream = impl->stream + direction;
    uint8_t buffer[1024];
    struct spa_pod_builder b = { 0 };

    stream->stream = pw_stream_new(impl->core, direction == PW_DIRECTION_INPUT ? "Qubes Sink" : "Qubes Source", stream->stream_props);
    stream->stream_props = NULL;

    if (stream->stream == NULL)
        return -errno;

    pw_stream_add_listener(stream->stream,
            &stream->stream_listener,
            direction == PW_DIRECTION_INPUT ? &playback_stream_events : &capture_stream_events,
            impl);

    n_params = 0;
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    params[n_params++] = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &stream->info);

    if ((res = pw_stream_connect(stream->stream,
            direction,
            PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_RT_PROCESS |
            PW_STREAM_FLAG_MAP_BUFFERS |
            0,
            params, n_params)) < 0)
        return res;

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

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL;
    struct impl *impl;
    const char *str;
    const char *peer_domain_prop = NULL;
    uint32_t domid = UINT32_MAX;
    int res = -EFAULT;

#ifdef PW_LOG_TOPIC_INIT
    PW_LOG_TOPIC_INIT(mod_topic);
#endif
    pw_log_error("hello from qubes module");

    impl = calloc(1, sizeof(struct impl));
    if (impl == NULL)
        return -errno;

    if (args == NULL)
        args = "";

    props = pw_properties_new_string(args);
    if (props == NULL) {
        res = -errno;
        pw_log_error( "can't create properties: %m");
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
        char *endptr;
        errno = 0;
        domid = strtoul(peer_domain_prop, &endptr, 10);
        if (*peer_domain_prop < '0' || *peer_domain_prop > '9' ||
            (*peer_domain_prop == '0' && peer_domain_prop[1]) ||
            *endptr)
            errno = EINVAL;
        else if (domid >= UINT16_MAX >> 2)
            errno = ERANGE;
        if (errno) {
            pw_log_error("Invalid domain ID %s", peer_domain_prop);
            res = -errno;
            goto error;
        }
    }

    pw_log_error("module %p: new (%s), peer id is %d", impl, args, (int)domid);

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
        stream->vchan = i ?
            libvchan_server_init((int)domid, QUBES_PA_SOURCE_VCHAN_PORT, 1 << 15, 128) :
            libvchan_server_init((int)domid, QUBES_PA_SINK_VCHAN_PORT, 128, 1 << 15);
        if (stream->vchan == NULL) {
            res = -errno;
            pw_log_error("can't create %s vchan: %m", msg);
            goto error;
        }
    }

    impl->module = module;
    impl->context = context;

#if PW_CHECK_VERSION(0, 3, 30)
    if ((str = pw_properties_get(props, "stream.source.props")) != NULL)
        pw_properties_update_string(impl->stream[PW_DIRECTION_OUTPUT].stream_props, str, strlen(str));

    if ((str = pw_properties_get(props, "stream.sink.props")) != NULL)
        pw_properties_update_string(impl->stream[PW_DIRECTION_INPUT].stream_props, str, strlen(str));
#endif

    parse_audio_info(impl);

    impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
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

    uint32_t n_support;
    const struct spa_support *support = pw_context_get_support(impl->context, &n_support);
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

    if ((res = create_stream(impl, PW_DIRECTION_INPUT)) < 0)
        goto error;

    if ((res = create_stream(impl, PW_DIRECTION_OUTPUT)) < 0)
        goto error;

    pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

    pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

    return 0;

error:
    impl_destroy(impl);
    return res;
}