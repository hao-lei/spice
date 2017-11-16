/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2017 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Test streaming device
 */

#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <spice/protocol.h>
#include <spice/stream-device.h>

#include "test-display-base.h"
#include "test-glib-compat.h"

static SpiceCharDeviceInstance vmc_instance;

// device buffer to read from
static uint8_t message[2048];
// position to read from
static unsigned pos;
// array of limits when the read should return
// the array is defined as [message_sizes_curr, message_sizes_end)
// then the size is reach we move on next one till exausted
static unsigned message_sizes[16];
static unsigned *message_sizes_end, *message_sizes_curr;

// handle writes to the device
static int vmc_write(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                     SPICE_GNUC_UNUSED const uint8_t *buf,
                     int len)
{
    // currently we don't test anything on write so reply we wrote it
    return len;
}

static int vmc_read(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                    uint8_t *buf,
                    int len)
{
    int ret;

    if (pos >= *message_sizes_curr && message_sizes_curr < message_sizes_end) {
        ++message_sizes_curr;
    }
    if (message_sizes_curr >= message_sizes_end || pos >= *message_sizes_curr) {
        return 0;
    }
    ret = MIN(*message_sizes_curr - pos, len);
    memcpy(buf, &message[pos], ret);
    pos += ret;
    // kick off next message read
    // currently Qemu kicks the device so we need to do it manually
    // here
    spice_server_char_device_wakeup(&vmc_instance);
    return ret;
}

static void vmc_state(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                      SPICE_GNUC_UNUSED int connected)
{
}

static SpiceCharDeviceInterface vmc_interface = {
    .base = {
        .type          = SPICE_INTERFACE_CHAR_DEVICE,
        .description   = "test spice virtual channel char device",
        .major_version = SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
        .minor_version = SPICE_INTERFACE_CHAR_DEVICE_MINOR,
    },
    .state              = vmc_state,
    .write              = vmc_write,
    .read               = vmc_read,
};

// this specifically creates a stream device
static SpiceCharDeviceInstance vmc_instance = {
    .subtype = "port",
    .portname = "com.redhat.stream.0",
};

static uint8_t *add_stream_hdr(uint8_t *p, StreamMsgType type, uint32_t size)
{
    StreamDevHeader hdr = {
        .protocol_version = STREAM_DEVICE_PROTOCOL,
        .type = GUINT16_TO_LE(type),
        .size = GUINT32_TO_LE(size),
    };

    memcpy(p, &hdr, sizeof(hdr));
    return p + sizeof(hdr);
}

static uint8_t *add_format(uint8_t *p, uint32_t w, uint32_t h, SpiceVideoCodecType codec)
{
    StreamMsgFormat fmt = {
        .width = GUINT32_TO_LE(w),
        .height = GUINT32_TO_LE(h),
        .codec = codec,
    };

    p = add_stream_hdr(p, STREAM_TYPE_FORMAT, sizeof(fmt));
    memcpy(p, &fmt, sizeof(fmt));
    return p + sizeof(fmt);
}

static void test_stream_device(void)
{
    uint8_t *p = message;
    SpiceCoreInterface *core = basic_event_loop_init();
    Test *test = test_new(core);

    message_sizes_curr = message_sizes;
    message_sizes_end = message_sizes;

    // add some messages into device buffer
    // here we are testing the device is reading at least two
    // consecutive format messages
    // first message part has 2 extra bytes to check for header split
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    *message_sizes_end = p - message + 2;
    ++message_sizes_end;

    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_VP9);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    // add a message to stop data to be read
    p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    // this message should not be read
    p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    vmc_instance.base.sif = &vmc_interface.base;
    spice_server_add_interface(test->server, &vmc_instance.base);

    // device should not have read data before we open it
    spice_server_char_device_wakeup(&vmc_instance);
    g_assert_cmpint(pos, ==, 0);

    // we need to open the device and kick the start
    spice_server_port_event(&vmc_instance, SPICE_PORT_EVENT_OPENED);
    spice_server_char_device_wakeup(&vmc_instance);

    // make sure first 2 parts are read completely
    g_assert(message_sizes_curr - message_sizes >= 2);
    // make sure last part is not read at all
    g_assert(message_sizes_curr - message_sizes < 4);

    test_destroy(test);
    basic_event_loop_destroy();
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/server/stream-device", test_stream_device);

    return g_test_run();
}
