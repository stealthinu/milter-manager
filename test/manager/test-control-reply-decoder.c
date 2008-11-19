/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include <milter/manager/milter-manager-control-reply-decoder.h>

#include <gcutter.h>

void test_decode_success (void);

static MilterDecoder *decoder;
static GString *buffer;

static GError *expected_error;
static GError *actual_error;

static gint n_success_received;

static void
cb_success (MilterManagerControlReplyDecoder *decoder, gpointer user_data)
{
    n_success_received++;
}

static void
setup_signals (MilterDecoder *decoder)
{
#define CONNECT(name)                                                   \
    g_signal_connect(decoder, #name, G_CALLBACK(cb_ ## name), NULL)

    CONNECT(success);

#undef CONNECT
}

void
setup (void)
{
    decoder = milter_manager_control_reply_decoder_new();
    setup_signals(decoder);

    expected_error = NULL;
    actual_error = NULL;

    n_success_received = 0;

    buffer = g_string_new(NULL);
}

void
teardown (void)
{
    if (decoder)
        g_object_unref(decoder);

    if (buffer)
        g_string_free(buffer, TRUE);

    if (expected_error)
        g_error_free(expected_error);
    if (actual_error)
        g_error_free(actual_error);
}

static GError *
decode (void)
{
    guint32 content_size;
    gchar content_string[sizeof(guint32)];
    GError *error = NULL;

    content_size = g_htonl(buffer->len);
    memcpy(content_string, &content_size, sizeof(content_size));
    g_string_prepend_len(buffer, content_string, sizeof(content_size));

    milter_decoder_decode(decoder, buffer->str, buffer->len, &error);
    g_string_truncate(buffer, 0);

    return error;
}

void
test_decode_success (void)
{
    g_string_append_c(buffer, 's');

    gcut_assert_error(decode());
    cut_assert_equal_int(1, n_success_received);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
