/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2015  Marek Marczykowski-Górecki
 *                              <marmarek@invisiblethingslab.com>
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


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/select.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/socket-util.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "module-vchan-sink-symdef.h"
#include "qubes-vchan-sink.h"
#include <libvchan.h>
#ifdef HAVE_QUBESDB_CLIENT_H
#include <qubesdb-client.h>
#endif

PA_MODULE_AUTHOR("Marek Marczykowski-Górecki");
PA_MODULE_DESCRIPTION("VCHAN sink/source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE("sink_name=<name for the sink> "
        "sink_desc=<description string for the sink> "
        "sink_properties=<properties for the sink> "
        "source_name=<name for the source> "
        "source_desc=<description string for the source> "
        "source_properties=<properties for the source> "
        "format=<sample format> "
        "rate=<sample rate>"
        "domid=<target domain id>"
        "channels=<number of channels> "
        "channel_map=<channel map>");

#define DEFAULT_DOMID 0
#define DEFAULT_SINK_NAME "vchan_output"
#define DEFAULT_SINK_DESC "Qubes VCHAN sink"
#define DEFAULT_SOURCE_NAME "vchan_input"
#define DEFAULT_SOURCE_DESC "Qubes VCHAN source"
#define DEFAULT_PROFILE_NAME "qubes-vchan-profile"

#define VCHAN_BUF 8192

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_sink *sink;
    pa_source *source;

    int domid;
    libvchan_t *play_ctrl;
    libvchan_t *rec_ctrl;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_memchunk memchunk_sink;
    pa_memchunk memchunk_source;

    pa_rtpoll_item *play_rtpoll_item;
    pa_rtpoll_item *rec_rtpoll_item;

    char *input_port_name;
    char *output_port_name;
};

static const char *const valid_modargs[] = {
    "sink_name",
    "sink_desc",
    "sink_properties",
    "source_name",
    "source_desc",
    "source_properties",
    "format",
    "rate",
    "domid",
    "channels",
    "channel_map",
    NULL
};

static int do_conn(struct userdata *u);

#if PA_CHECK_VERSION(12,0,0)
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state,
        pa_suspend_cause_t suspend_cause __attribute__((unused))) {
    struct userdata *u = s->userdata;
    uint32_t cmd = 0;

    pa_log("sink cork req state =%d, now state=%d\n", new_state,
            (int) (s->state));
    if (s->state == PA_SINK_SUSPENDED && new_state != PA_SINK_SUSPENDED)
        cmd = QUBES_PA_SINK_UNCORK_CMD;
    else if (s->state != PA_SINK_SUSPENDED && new_state == PA_SINK_SUSPENDED)
        cmd = QUBES_PA_SINK_CORK_CMD;
    if (cmd != 0) {
        if (libvchan_send(u->rec_ctrl, (char*)&cmd, sizeof(cmd)) < 0) {
            pa_log("vchan: failed to send sink cork cmd");
        }
    }

    return 0;
}
#endif

static int sink_process_msg(pa_msgobject * o, int code, void *data,
                int64_t offset, pa_memchunk * chunk)
{
    struct userdata *u = PA_SINK(o)->userdata;
    switch (code) {
#if !PA_CHECK_VERSION(12,0,0)
    case PA_SINK_MESSAGE_SET_STATE: {
        int r;
        int state;

        state = PA_PTR_TO_UINT(data);
        r = pa_sink_process_msg(o, code, data, offset, chunk);
        if (r >= 0) {
            uint32_t cmd = 0;
            pa_log("sink cork req state =%d, now state=%d\n", state,
                   (int) (u->sink->state));
            if (u->sink->state == PA_SINK_SUSPENDED && state != PA_SINK_SUSPENDED)
                cmd = QUBES_PA_SINK_UNCORK_CMD;
            else if (u->sink->state != PA_SINK_SUSPENDED && state == PA_SINK_SUSPENDED)
                cmd = QUBES_PA_SINK_CORK_CMD;
            if (cmd != 0) {
                if (libvchan_send(u->rec_ctrl, (char*)&cmd, sizeof(cmd)) < 0) {
                    pa_log("vchan: failed to send sink cork cmd");
                }
            }
        }
        return r;
    }
#endif

    case PA_SINK_MESSAGE_GET_LATENCY:{
            size_t n = 0;
            n += u->memchunk_sink.length;

            *((pa_usec_t *) data) =
                pa_bytes_to_usec(n, &u->sink->sample_spec);
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

#if PA_CHECK_VERSION(12,0,0)
static int source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state,
        pa_suspend_cause_t suspend_cause __attribute__((unused))) {
    struct userdata *u = s->userdata;
    uint32_t cmd = 0;

    pa_log("source cork req state =%d, now state=%d\n", new_state,
            (int) (s->state));
    if (s->state != PA_SOURCE_RUNNING && new_state == PA_SOURCE_RUNNING)
        cmd = QUBES_PA_SOURCE_START_CMD;
    else if (s->state == PA_SOURCE_RUNNING && new_state != PA_SOURCE_RUNNING)
        cmd = QUBES_PA_SOURCE_STOP_CMD;
    if (cmd != 0) {
        if (libvchan_send(u->rec_ctrl, (char*)&cmd, sizeof(cmd)) < 0) {
            pa_log("vchan: failed to send record cmd");
            /* This is a problem in case of enabling recording, in case
             * of QUBES_PA_SOURCE_STOP_CMD it can happen that remote end
             * is already disconnected, so indeed will not send further data.
             * This can happen for example when we terminate the
             * process because of pacat in dom0 has disconnected.
             */
            if (new_state == PA_SOURCE_RUNNING)
                return -1;
            else
                return 0;
        }
    }

    return 0;
}
#endif

static int source_process_msg(pa_msgobject * o, int code, void *data,
                int64_t offset, pa_memchunk * chunk)
{
    struct userdata *u = PA_SOURCE(o)->userdata;
    switch (code) {
#if !PA_CHECK_VERSION(12,0,0)
    case PA_SOURCE_MESSAGE_SET_STATE: {
        int r;
        int state;

        state = PA_PTR_TO_UINT(data);
        r = pa_source_process_msg(o, code, data, offset, chunk);
        if (r >= 0) {
            pa_log("source cork req state =%d, now state=%d\n", state,
                   (int) (u->source->state));
            uint32_t cmd = 0;
            if (u->source->state != PA_SOURCE_RUNNING && state == PA_SOURCE_RUNNING)
                cmd = QUBES_PA_SOURCE_START_CMD;
            else if (u->source->state == PA_SOURCE_RUNNING && state != PA_SOURCE_RUNNING)
                cmd = QUBES_PA_SOURCE_STOP_CMD;
            if (cmd != 0) {
                if (libvchan_send(u->rec_ctrl, (char*)&cmd, sizeof(cmd)) < 0) {
                    pa_log("vchan: failed to send record cmd");
                    /* This is a problem in case of enabling recording, in case
                     * of QUBES_PA_SOURCE_STOP_CMD it can happen that remote end
                     * is already disconnected, so indeed will not send further data.
                     * This can happen for example when we terminate the
                     * process because of pacat in dom0 has disconnected.
                     */
                    if (state == PA_SOURCE_RUNNING)
                        return -1;
                    else
                        return r;
                }
            }
        }
        return r;
    }
#endif

    case PA_SOURCE_MESSAGE_GET_LATENCY:{
            size_t n = 0;
            n += u->memchunk_source.length;

            *((pa_usec_t *) data) =
                pa_bytes_to_usec(n, &u->source->sample_spec);
            return 0;
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int write_to_vchan(libvchan_t *ctrl, char *buf, int size)
{
    ssize_t l;
    fd_set rfds;
    struct timeval tv = { 0, 0 };
    int ret, fd = libvchan_fd_for_select(ctrl);
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret == -1) {
        pa_log("Failed to select() in vchan: %s",
               pa_cstrerror(errno));
        return -1;
    }
    if (ret) {
        if (libvchan_wait(ctrl) < 0) {
            pa_log("Failed libvchan_wait");
            return -1;
        }
    }
    if (libvchan_buffer_space(ctrl)) {
        l = libvchan_write(ctrl, buf, size);
    } else {
        l = -1;
        errno = EAGAIN;
    }

    return l;
}

static int process_sink_render(struct userdata *u)
{
    pa_assert(u);

    if (u->memchunk_sink.length <= 0)
        pa_sink_render(u->sink, libvchan_buffer_space(u->play_ctrl), &u->memchunk_sink);

    pa_assert(u->memchunk_sink.length > 0);

    for (;;) {
        ssize_t l;
        void *p;

        p = pa_memblock_acquire(u->memchunk_sink.memblock);
        l = write_to_vchan(u->play_ctrl, (char *) p +
                   u->memchunk_sink.index, u->memchunk_sink.length);
        pa_memblock_release(u->memchunk_sink.memblock);

        pa_assert(l != 0);

        if (l < 0) {

            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
                return 0;
            else {
                pa_log
                    ("Failed to write data to VCHAN: %s",
                     pa_cstrerror(errno));
                return -1;
            }

        } else {

            u->memchunk_sink.index += (size_t) l;
            u->memchunk_sink.length -= (size_t) l;

            if (u->memchunk_sink.length <= 0) {
                pa_memblock_unref(u->memchunk_sink.memblock);
                pa_memchunk_reset(&u->memchunk_sink);
            }
        }

        return 0;
    }
}

static int process_source_data(struct userdata *u)
{
    ssize_t l;
    void *p;
    if (!u->memchunk_source.memblock) {
        u->memchunk_source.memblock = pa_memblock_new(u->core->mempool, 16*1024); // at least vchan buffer size
        u->memchunk_source.index = u->memchunk_source.length = 0;
    }

    pa_assert(pa_memblock_get_length(u->memchunk_source.memblock) > u->memchunk_source.index);

    p = pa_memblock_acquire(u->memchunk_source.memblock);
    l = libvchan_read(u->rec_ctrl, p + u->memchunk_source.index, pa_memblock_get_length(u->memchunk_source.memblock) - u->memchunk_source.index);
    pa_memblock_release(u->memchunk_source.memblock);
    pa_log_debug("process_source_data %lu", l);

    if (l <= 0) {
        /* vchan disconnected/error */
        pa_log("Failed to read data from vchan");
        return -1;
    } else {

        u->memchunk_source.length = (size_t) l;
        pa_source_post(u->source, &u->memchunk_source);
        u->memchunk_source.index += (size_t) l;

        if (u->memchunk_source.index >= pa_memblock_get_length(u->memchunk_source.memblock)) {
            pa_memblock_unref(u->memchunk_source.memblock);
            pa_memchunk_reset(&u->memchunk_source);
        }
    }
    return 0;
}

static void thread_func(void *userdata)
{
    struct userdata *u = userdata;
    char buf[2048]; // max ring buffer size

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    for (;;) {
        struct pollfd *play_pollfd;
        struct pollfd *rec_pollfd;
        int ret;

        if (!libvchan_is_open(u->play_ctrl) || !libvchan_is_open(u->rec_ctrl)) {
            pa_log("vchan disconnected, restarting server");

            libvchan_close(u->play_ctrl);
            libvchan_close(u->rec_ctrl);
            pa_rtpoll_item_free(u->play_rtpoll_item);
            pa_rtpoll_item_free(u->rec_rtpoll_item);

            u->play_ctrl = NULL;
            u->rec_ctrl = NULL;
            u->play_rtpoll_item = NULL;
            u->rec_rtpoll_item = NULL;

            if (do_conn(u) < 0) {
                pa_log("failed to restart vchan server");
                goto fail;
            }
            pa_log("vchan server restarted");
        }

        play_pollfd = pa_rtpoll_item_get_pollfd(u->play_rtpoll_item, NULL);
        rec_pollfd = pa_rtpoll_item_get_pollfd(u->rec_rtpoll_item, NULL);

        if (play_pollfd->revents & POLLIN) {
            if (libvchan_wait(u->play_ctrl) < 0)
                goto fail;
            play_pollfd->revents = 0;
        }

        if (rec_pollfd->revents & POLLIN) {
            if (libvchan_wait(u->rec_ctrl) < 0)
                goto fail;
            rec_pollfd->revents = 0;
        }

        /* Render some data and write it to the fifo */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {

            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);

            if (libvchan_buffer_space(u->play_ctrl)) {
                if (process_sink_render(u) < 0)
                    goto fail;
            }
        }

        if (u->source->thread_info.state == PA_SOURCE_RUNNING) {
            while (libvchan_data_ready(u->rec_ctrl)) {
                if (process_source_data(u) < 0)
                    goto fail;
            }
        } else {
            /* discard the data */
            if (libvchan_data_ready(u->rec_ctrl))
                if (libvchan_read(u->rec_ctrl, buf, sizeof(buf)) < 0)
                    goto fail;
        }

        /* Hmm, nothing to do. Let's sleep */
        play_pollfd->events = POLLIN;
        rec_pollfd->events = POLLIN;

#if PA_CHECK_VERSION(6,0,0)
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
#else
        if ((ret = pa_rtpoll_run(u->rtpoll, true)) < 0)
#endif
            goto fail;

        if (ret == 0)
            goto finish;
    }

      fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core),
              PA_CORE_MESSAGE_UNLOAD_MODULE, u->module,
              0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

      finish:
    pa_log_debug("Thread shutting down");
}

static int do_conn(struct userdata *u)
{
    struct pollfd *pollfd;

    u->play_ctrl = libvchan_server_init(u->domid, QUBES_PA_SINK_VCHAN_PORT, 128, 2048);
    if (!u->play_ctrl) {
        pa_log("libvchan_server_init play failed\n");
        return -1;
    }
    u->rec_ctrl = libvchan_server_init(u->domid, QUBES_PA_SOURCE_VCHAN_PORT, 2048, 128);
    if (!u->rec_ctrl) {
        pa_log("libvchan_server_init rec failed\n");
        return -1;
    }

    u->play_rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->play_rtpoll_item, NULL);
    pollfd->fd = libvchan_fd_for_select(u->play_ctrl);
    pollfd->events = POLLIN;
    pollfd->revents = 0;
    pa_log("play libvchan_fd_for_select=%d, ctrl=%p\n", pollfd->fd, u->play_ctrl);

    u->rec_rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rec_rtpoll_item, NULL);
    pollfd->fd = libvchan_fd_for_select(u->rec_ctrl);
    pollfd->events = POLLIN;
    pollfd->revents = 0;
    pa_log("rec libvchan_fd_for_select=%d, ctrl=%p\n", pollfd->fd, u->rec_ctrl);

    return 0;
}

static void create_card_ports(struct userdata *u, pa_hashmap *card_ports)
{
    pa_device_port *port;
    pa_device_port_new_data port_data;
    const char *name_prefix, *input_description, *output_description;

    pa_assert(u);
    pa_assert(card_ports);

    name_prefix = "qubes-vchan-io";
    input_description = "Qubes vchan Input";
    output_description = "Qubes vchan Output";

    u->output_port_name = pa_sprintf_malloc("%s-output", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->output_port_name);
    pa_device_port_new_data_set_description(&port_data, output_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_OUTPUT);
    pa_device_port_new_data_set_available(&port_data, PA_AVAILABLE_YES);
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(card_ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);

    u->input_port_name = pa_sprintf_malloc("%s-input", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->input_port_name);
    pa_device_port_new_data_set_description(&port_data, input_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_INPUT);
    pa_device_port_new_data_set_available(&port_data, PA_AVAILABLE_YES);
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(card_ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);
}

static pa_card_profile *create_card_profile(struct userdata *u, pa_hashmap * ports)
{
    pa_device_port *input_port, *output_port;
    pa_card_profile *cp = NULL;

    pa_assert(u->input_port_name);
    pa_assert(u->output_port_name);
    pa_assert_se(input_port  = pa_hashmap_get(ports, u->input_port_name));
    pa_assert_se(output_port = pa_hashmap_get(ports, u->output_port_name));

    cp = pa_card_profile_new(DEFAULT_PROFILE_NAME, "Default Qubes vchan Profile", 0);
    cp->priority = 20;
    cp->n_sinks = 1;
    cp->n_sources = 1;
    cp->max_sink_channels = 1;
    cp->max_source_channels = 1;

    pa_hashmap_put(input_port->profiles,  cp->name, cp);
    pa_hashmap_put(output_port->profiles, cp->name, cp);

    cp->available = PA_AVAILABLE_YES;

    return cp;
}

static int create_card(pa_module *m, struct userdata *u)
{
    pa_card_new_data data;
    pa_card_profile *cp;

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Qubes vchan");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, "Qubes vchan");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");

    pa_card_new_data_set_name(&data, "Qubes vchan");

    create_card_ports(u, data.ports);
        cp = create_card_profile(u, data.ports);
        pa_hashmap_put(data.profiles, cp->name, cp);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log("Failed to allocate card.");
        return -1;
    }

    return 0;
}

/* Copied from module-bluez5-device.c in Pulseaudio 7.1's source tree. */
static void connect_ports(struct userdata *u, void *new_data, pa_direction_t direction) {
    pa_device_port *port;

    if (direction == PA_DIRECTION_OUTPUT) {
        pa_sink_new_data *sink_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->output_port_name));
        pa_assert_se(pa_hashmap_put(sink_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    } else {
        pa_source_new_data *source_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->input_port_name));
        pa_assert_se(pa_hashmap_put(source_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    }
}

int pa__init(pa_module * m)
{
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    pa_sink_new_data data_sink;
    pa_source_new_data data_source;
    int domid = DEFAULT_DOMID;
#ifdef HAVE_QUBESDB_CLIENT_H
    qdb_handle_t qdb;
    char *qdb_entry, *tmp;
    int qdb_domid;

    qdb = qdb_open(NULL);
    if (!qdb) {
        perror("qdb_open");
        exit(1);
    }
    // Read domid from qubesdb
    qdb_entry = qdb_read(qdb, "/qubes-audio-domain-xid", NULL);
    if (qdb_entry) {
        qdb_domid = strtol(qdb_entry, &tmp, 10);
        if ((*tmp == '\0') && (tmp != qdb_entry)) {
            domid = qdb_domid;
        }
        free(qdb_entry);
    }
    qdb_close(qdb);
#endif

    pa_assert(m);

    pa_log("vchan module loading");
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map
        (ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log
            ("Invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    pa_memchunk_reset(&u->memchunk_sink);
    pa_memchunk_reset(&u->memchunk_source);
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);


    pa_log("using domid: %d", domid);
    pa_modargs_get_value_s32(ma, "domid", &domid);
    u->domid = domid;
    if ((do_conn(u)) < 0) {

        pa_log("get_early_allocated_vchan: %s",
               pa_cstrerror(errno));
        goto fail;
    }

    if (create_card(m, u) < 0)
        goto fail;

    /* SINK preparation */
    pa_sink_new_data_init(&data_sink);
    data_sink.driver = __FILE__;
    data_sink.module = m;
    pa_sink_new_data_set_name(&data_sink,
                  pa_modargs_get_value(ma,
                               "sink_name",
                               DEFAULT_SINK_NAME));
    pa_proplist_sets(data_sink.proplist,
             PA_PROP_DEVICE_STRING, DEFAULT_SINK_NAME);
    pa_proplist_sets(data_sink.proplist,
             PA_PROP_DEVICE_DESCRIPTION,
             pa_modargs_get_value(ma,
                          "sink_desc",
                          DEFAULT_SINK_DESC));
    pa_sink_new_data_set_sample_spec(&data_sink, &ss);
    pa_sink_new_data_set_channel_map(&data_sink, &map);

    if (pa_modargs_get_proplist
        (ma, "sink_properties", data_sink.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data_sink);
        goto fail;
    }

    connect_ports(u, &data_sink, PA_DIRECTION_OUTPUT);

    u->sink = pa_sink_new(m->core, &data_sink, PA_SINK_LATENCY);
    pa_sink_new_data_done(&data_sink);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
#if PA_CHECK_VERSION(12,0,0)
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
#endif
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_max_request(u->sink, VCHAN_BUF);
    pa_sink_set_fixed_latency(u->sink,
                  pa_bytes_to_usec
                  (VCHAN_BUF,
                   &u->sink->sample_spec));

    /* SOURCE preparation */
    pa_source_new_data_init(&data_source);
    data_source.driver = __FILE__;
    data_source.module = m;
    pa_source_new_data_set_name(&data_source, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_proplist_sets(data_source.proplist, PA_PROP_DEVICE_STRING, DEFAULT_SOURCE_NAME);
    pa_proplist_sets(data_source.proplist, PA_PROP_DEVICE_DESCRIPTION, 
                        pa_modargs_get_value(ma, "source_desc", DEFAULT_SOURCE_DESC));
    pa_source_new_data_set_sample_spec(&data_source, &ss);
    pa_source_new_data_set_channel_map(&data_source, &map);

    if (pa_modargs_get_proplist(ma, "source_properties", data_source.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&data_source);
        goto fail;
    }

    connect_ports(u, &data_source, PA_DIRECTION_INPUT);

    u->source = pa_source_new(m->core, &data_source, PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data_source);

    if (!u->source) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
#if PA_CHECK_VERSION(12,0,0)
    u->source->set_state_in_io_thread = source_set_state_in_io_thread_cb;
#endif
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(PIPE_BUF, &u->source->sample_spec));

#if PA_CHECK_VERSION(0,9,22)
    if (!(u->thread = pa_thread_new("vchan-sink", thread_func, u))) {
#else
    if (!(u->thread = pa_thread_new(thread_func, u))) {
#endif
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);
    pa_source_put(u->source);

    pa_modargs_free(ma);

    return 0;

      fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module * m)
{
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module * m)
{
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL,
                  PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->source)
        pa_source_unref(u->source);

    if (u->memchunk_sink.memblock)
        pa_memblock_unref(u->memchunk_sink.memblock);

    if (u->memchunk_source.memblock)
        pa_memblock_unref(u->memchunk_source.memblock);

    if (u->play_rtpoll_item)
        pa_rtpoll_item_free(u->play_rtpoll_item);

    if (u->rec_rtpoll_item)
        pa_rtpoll_item_free(u->rec_rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->play_ctrl)
        libvchan_close(u->play_ctrl);

    if (u->rec_ctrl)
        libvchan_close(u->rec_ctrl);

    if (u->card)
        pa_card_free(u->card);

    pa_xfree(u->output_port_name);
    pa_xfree(u->input_port_name);

    pa_xfree(u);
}
