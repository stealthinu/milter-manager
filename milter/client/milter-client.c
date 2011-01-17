/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008-2011  Kouhei Sutou <kou@clear-code.com>
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
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif /* HAVE_CONFIG_H */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>

#include <errno.h>

#include <glib/gstdio.h>

#include <milter/core/milter-marshalers.h>
#include "../client.h"
#include "milter-client-private.h"

enum
{
    PROP_0,
    PROP_EVENT_LOOP_BACKEND,
    PROP_N_WORKERS,
    PROP_CUSTOM_FORK,
    PROP_DEFAULT_PACKET_BUFFER_SIZE
};

enum
{
    CONNECTION_ESTABLISHED,
    LISTEN_STARTED,
    MAINTAIN,
    SESSIONS_FINISHED,
    LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = {0};

#define MILTER_CLIENT_GET_PRIVATE(obj)                  \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj),                 \
                                 MILTER_TYPE_CLIENT,    \
                                 MilterClientPrivate))

typedef struct _MilterClientPrivate	MilterClientPrivate;
struct _MilterClientPrivate
{
    GIOChannel *listening_channel;
    MilterEventLoop *accept_loop;
    MilterEventLoop *process_loop;
    guint accept_watch_id;
    guint accept_error_watch_id;
    gchar *connection_spec;
    GList *processing_data;
    guint n_processing_sessions;
    guint n_processed_sessions;
    guint maintenance_interval;
    guint timeout;
    GIOChannel *listen_channel;
    gint listen_backlog;
    GMutex *quit_mutex;
    gboolean quitting;
    guint unix_socket_mode;
    guint default_unix_socket_mode;
    gchar *unix_socket_group;
    gchar *default_unix_socket_group;
    gboolean default_remove_unix_socket_on_close;
    gboolean remove_unix_socket_on_create;
    guint suspend_time_on_unacceptable;
    guint max_connections;
    gboolean multi_thread_mode;
    GThreadPool *worker_threads;
    struct {
        GIOChannel *control;
        guint n_process;
    } workers;
    struct sockaddr *address;
    socklen_t address_size;
    gchar *effective_user;
    gchar *effective_group;

    guint finisher_id;
    GList *finished_data;

    MilterSyslogLogger *syslog_logger;
    MilterClientEventLoopBackend event_loop_backend;
    MilterClientCustomForkFunc custom_fork;

    guint default_packet_buffer_size;
};

typedef struct _MilterClientProcessData
{
    MilterClientPrivate *priv;
    MilterClient *client;
    MilterClientContext *context;
    gulong finished_handler_id;
} MilterClientProcessData;

typedef gboolean (*AcceptConnectionFunction) (MilterClient *client, gint fd);

#define _milter_client_get_type milter_client_get_type
MILTER_DEFINE_ERROR_EMITTABLE_TYPE(MilterClient, _milter_client, G_TYPE_OBJECT)
#undef _milter_client_get_type

static void dispose        (GObject         *object);
static void set_property   (GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec);
static void get_property   (GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec);

static gchar *get_default_connection_spec
                           (MilterClient    *client);
static void   listen_started
                           (MilterClient    *client,
                            struct sockaddr *address,
                            socklen_t        address_size);
static GPid   default_fork (MilterClient    *client);

static gboolean run_master (MilterClient *client,
                            GError      **error);
static GThread *run_worker (MilterClient *client,
                            GError      **error);

static void
_milter_client_class_init (MilterClientClass *klass)
{
    GObjectClass *gobject_class;
    MilterClientClass *client_class = NULL;
    GParamSpec *spec;

    gobject_class = G_OBJECT_CLASS(klass);
    client_class = MILTER_CLIENT_CLASS(klass);

    gobject_class->dispose      = dispose;
    gobject_class->set_property = set_property;
    gobject_class->get_property = get_property;

    client_class->get_default_connection_spec = get_default_connection_spec;
    client_class->listen_started = listen_started;
    client_class->fork = default_fork;

    spec = g_param_spec_enum("event-loop-backend",
                             "Event loop backend",
                             "The event loop backend of the client",
                             MILTER_TYPE_CLIENT_EVENT_LOOP_BACKEND,
                             MILTER_CLIENT_EVENT_LOOP_BACKEND_GLIB,
                             G_PARAM_READWRITE);
    g_object_class_install_property(gobject_class, PROP_EVENT_LOOP_BACKEND, spec);

    spec = g_param_spec_uint("n-workers",
                             "Number of worker processes",
                             "The Number of worker processes of the client",
                             0, MILTER_CLIENT_MAX_N_WORKERS, 0,
                             G_PARAM_READWRITE);
    g_object_class_install_property(gobject_class, PROP_N_WORKERS, spec);

    spec = g_param_spec_pointer("custom-fork",
                                "Custom fork",
                                "The custom fork",
                                G_PARAM_READWRITE);
    g_object_class_install_property(gobject_class, PROP_CUSTOM_FORK, spec);

    spec = g_param_spec_uint("default-packet-buffer-size",
                             "Default packet buffer size",
                             "The default packet buffer size of each "
                             "client context of the client.",
                             0, G_MAXUINT, 0,
                             G_PARAM_READWRITE);
    g_object_class_install_property(gobject_class,
                                    PROP_DEFAULT_PACKET_BUFFER_SIZE, spec);

    signals[CONNECTION_ESTABLISHED] =
        g_signal_new("connection-established",
                     MILTER_TYPE_CLIENT,
                     G_SIGNAL_RUN_LAST,
                     G_STRUCT_OFFSET(MilterClientClass, connection_established),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__OBJECT,
                     G_TYPE_NONE, 1, MILTER_TYPE_CLIENT_CONTEXT);

    signals[LISTEN_STARTED] =
        g_signal_new("listen-started",
                     MILTER_TYPE_CLIENT,
                     G_SIGNAL_RUN_LAST,
                     G_STRUCT_OFFSET(MilterClientClass, listen_started),
                     NULL, NULL,
                     _milter_marshal_VOID__POINTER_UINT,
                     G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_UINT);

    signals[MAINTAIN] =
        g_signal_new("maintain",
                     MILTER_TYPE_CLIENT,
                     G_SIGNAL_RUN_LAST,
                     G_STRUCT_OFFSET(MilterClientClass, maintain),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__VOID,
                     G_TYPE_NONE, 0);

    signals[SESSIONS_FINISHED] =
        g_signal_new("sessions-finished",
                     MILTER_TYPE_CLIENT,
                     G_SIGNAL_RUN_LAST,
                     G_STRUCT_OFFSET(MilterClientClass, sessions_finished),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__UINT,
                     G_TYPE_NONE, 1, G_TYPE_UINT);

    g_type_class_add_private(gobject_class, sizeof(MilterClientPrivate));
}

MilterEventLoop *
milter_client_create_event_loop (MilterClient *client, gboolean use_default_context)
{
    MilterEventLoop *loop = NULL;

    switch (milter_client_get_event_loop_backend(client)) {
    case MILTER_CLIENT_EVENT_LOOP_BACKEND_GLIB:
        if (use_default_context) {
            loop = milter_glib_event_loop_new(NULL);
        } else {
            GMainContext *context;
            context = g_main_context_new();
            loop = milter_glib_event_loop_new(context);
            g_main_context_unref(context);
        }
        break;
    case MILTER_CLIENT_EVENT_LOOP_BACKEND_LIBEV:
        loop = milter_libev_event_loop_new();
        break;
    }

    return loop;
}

static void
_milter_client_init (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    priv->listening_channel = NULL;
    priv->accept_loop = NULL;
    priv->process_loop = NULL;

    priv->accept_watch_id = 0;
    priv->accept_error_watch_id = 0;
    priv->connection_spec = NULL;
    priv->processing_data = NULL;
    priv->n_processing_sessions = 0;
    priv->n_processed_sessions = 0;
    priv->maintenance_interval = 0;
    priv->timeout = 7210;
    priv->listen_channel = NULL;
    priv->listen_backlog = -1;
    priv->quit_mutex = g_mutex_new();
    priv->quitting = FALSE;
    priv->unix_socket_mode = 0;
    priv->default_unix_socket_mode = 0660;
    priv->unix_socket_group = NULL;
    priv->default_unix_socket_group = NULL;
    priv->default_remove_unix_socket_on_close = TRUE;
    priv->remove_unix_socket_on_create = TRUE;
    priv->suspend_time_on_unacceptable =
        MILTER_CLIENT_DEFAULT_SUSPEND_TIME_ON_UNACCEPTABLE;
    priv->max_connections = MILTER_CLIENT_DEFAULT_MAX_CONNECTIONS;
    priv->multi_thread_mode = FALSE;
    priv->worker_threads = NULL;
    priv->workers.n_process = 0;
    priv->workers.control = NULL;
    priv->address = NULL;
    priv->address_size = 0;
    priv->effective_user = NULL;
    priv->effective_group = NULL;

    priv->finisher_id = 0;
    priv->finished_data = NULL;

    priv->syslog_logger = NULL;

    priv->event_loop_backend = MILTER_CLIENT_EVENT_LOOP_BACKEND_GLIB;

    priv->default_packet_buffer_size = 0;
}

static void
dispose_process_data_finished_handler (MilterClientProcessData *data)
{
    if (data->finished_handler_id > 0) {
        g_signal_handler_disconnect(data->context,
                                    data->finished_handler_id);
        data->finished_handler_id = 0;
    }
}

static void
process_data_free (MilterClientProcessData *data)
{
    dispose_process_data_finished_handler(data);
    g_object_unref(data->context);
    g_free(data);
}

static void
dispose_address (MilterClientPrivate *priv)
{
    if (priv->address) {
        g_free(priv->address);
        priv->address = NULL;
        priv->address_size = 0;
    }
}

static void
dispose_finisher (MilterClientPrivate *priv)
{
    if (priv->finisher_id > 0) {
        milter_event_loop_remove(priv->process_loop, priv->finisher_id);
        priv->finisher_id = 0;
    }
}

static void
finish_processing (MilterClientProcessData *data)
{
    guint n_processing_sessions;
    GString *rest_process;
    guint tag = 0;

    if (milter_need_debug_log()) {
        tag = milter_agent_get_tag(MILTER_AGENT(data->context));
        milter_debug("[%u] [client][finish]", tag);
    }

    data->priv->processing_data =
        g_list_remove(data->priv->processing_data, data);
    milter_client_session_finished(data->client);

    if (data->priv->quitting && data->priv->process_loop) {
        n_processing_sessions = data->priv->n_processing_sessions;
        g_mutex_lock(data->priv->quit_mutex);
        if (data->priv->quitting && n_processing_sessions == 0) {
            milter_debug("[%u] [client][loop][quit]", tag);
            milter_event_loop_quit(data->priv->process_loop);
        }
        g_mutex_unlock(data->priv->quit_mutex);
    }

    if (milter_need_debug_log()) {
        GList *processing_data, *process_data;

        processing_data = g_list_copy(data->priv->processing_data);
        rest_process = g_string_new("[");
        for (process_data = processing_data;
             process_data;
             process_data = g_list_next(process_data)) {
            MilterClientProcessData *_process_data = process_data->data;
            g_string_append_printf(
                rest_process, "<%u>, ",
                milter_agent_get_tag(MILTER_AGENT(_process_data->context)));
        }
        if (processing_data)
            g_string_truncate(rest_process, rest_process->len - 2);
        g_string_append(rest_process, "]");
        g_list_free(processing_data);
        milter_debug("[%u] [client][rest] %s", tag, rest_process->str);
        g_string_free(rest_process, TRUE);
    }

    process_data_free(data);
}

void
milter_client_set_n_processing_sessions (MilterClient *client,
                                         guint n_processing_sessions)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->n_processing_sessions = n_processing_sessions;
}

void
milter_client_set_n_processed_sessions (MilterClient *client,
                                        guint n_processed_sessions)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->n_processed_sessions = n_processed_sessions;
}

gboolean
milter_client_need_maintain (MilterClient *client, guint n_finished_sessions)
{
    MilterClientPrivate *priv;
    guint maintenance_interval, n_finished_sessions_in_interval;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->n_processing_sessions == 0 && n_finished_sessions > 0)
        return TRUE;

    maintenance_interval = milter_client_get_maintenance_interval(client);
    if (maintenance_interval == 0)
        return FALSE;

    n_finished_sessions_in_interval =
        priv->n_processed_sessions % maintenance_interval;
    return n_finished_sessions_in_interval < n_finished_sessions;
}

static void
dispose_finished_data (MilterClient *client)
{
    MilterClientPrivate *priv;
    guint n_processed_sessions_before, n_finished_sessions;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (!priv->finished_data)
        return;

    n_processed_sessions_before = priv->n_processed_sessions;
    g_list_foreach(priv->finished_data, (GFunc)finish_processing, NULL);
    g_list_free(priv->finished_data);
    priv->finished_data = NULL;
    n_finished_sessions =
        priv->n_processed_sessions - n_processed_sessions_before;
    g_signal_emit(client, signals[SESSIONS_FINISHED], 0, n_finished_sessions);

    milter_statistics("[sessions][finished] %u(+%u) %u",
                      priv->n_processed_sessions,
                      n_finished_sessions,
                      priv->n_processing_sessions);
    if (milter_client_need_maintain(client, n_finished_sessions)) {
        g_signal_emit(client, signals[MAINTAIN], 0);
    }
}

static void
watch_worker_process (GPid     pid,
                      gint     status,
                      gpointer data)
{
}

static void
dispose_accept_watchers (MilterClientPrivate *priv)
{
    if (priv->accept_watch_id > 0) {
        milter_event_loop_remove(priv->accept_loop, priv->accept_watch_id);
        priv->accept_watch_id = 0;
    }

    if (priv->accept_error_watch_id > 0) {
        milter_event_loop_remove(priv->accept_loop, priv->accept_error_watch_id);
        priv->accept_error_watch_id = 0;
    }
}

static void
dispose (GObject *object)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(object);

    if (priv->workers.control) {
        g_io_channel_unref(priv->workers.control);
        priv->workers.control = NULL;
    }

    if (priv->listening_channel) {
        g_io_channel_unref(priv->listening_channel);
        priv->listening_channel = NULL;
    }

    dispose_accept_watchers(priv);

    if (priv->accept_loop) {
        g_object_unref(priv->accept_loop);
        priv->accept_loop = NULL;
    }

    if (priv->process_loop) {
        g_object_unref(priv->process_loop);
        priv->process_loop = NULL;
    }

    if (priv->connection_spec) {
        g_free(priv->connection_spec);
        priv->connection_spec = NULL;
    }

    if (priv->processing_data) {
        g_list_foreach(priv->processing_data, (GFunc)process_data_free, NULL);
        g_list_free(priv->processing_data);
        priv->processing_data = NULL;
    }

    if (priv->listen_channel) {
        g_io_channel_unref(priv->listen_channel);
        priv->listen_channel = NULL;
    }

    if (priv->quit_mutex) {
        g_mutex_free(priv->quit_mutex);
        priv->quit_mutex = NULL;
    }

    if (priv->unix_socket_group) {
        g_free(priv->unix_socket_group);
        priv->unix_socket_group = NULL;
    }

    if (priv->default_unix_socket_group) {
        g_free(priv->default_unix_socket_group);
        priv->default_unix_socket_group = NULL;
    }

    if (priv->worker_threads) {
        g_thread_pool_free(priv->worker_threads, TRUE, FALSE);
        priv->worker_threads = NULL;
    }

    dispose_address(priv);

    if (priv->effective_user) {
        g_free(priv->effective_user);
        priv->effective_user = NULL;
    }

    if (priv->effective_group) {
        g_free(priv->effective_group);
        priv->effective_group = NULL;
    }

    dispose_finisher(priv);
    dispose_finished_data(MILTER_CLIENT(object));

    if (priv->syslog_logger) {
        g_object_unref(priv->syslog_logger);
        priv->syslog_logger = NULL;
    }

    G_OBJECT_CLASS(_milter_client_parent_class)->dispose(object);
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
    MilterClient *client;
    MilterClientPrivate *priv;

    client = MILTER_CLIENT(object);
    priv = MILTER_CLIENT_GET_PRIVATE(object);
    switch (prop_id) {
    case PROP_EVENT_LOOP_BACKEND:
        milter_client_set_event_loop_backend(client, g_value_get_enum(value));
        break;
    case PROP_N_WORKERS:
        milter_client_set_n_workers(client, g_value_get_uint(value));
        break;
    case PROP_CUSTOM_FORK:
        priv->custom_fork = g_value_get_pointer(value);
        break;
    case PROP_DEFAULT_PACKET_BUFFER_SIZE:
        milter_client_set_default_packet_buffer_size(client,
                                                     g_value_get_uint(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
    MilterClient *client;
    MilterClientPrivate *priv;

    client = MILTER_CLIENT(object);
    priv = MILTER_CLIENT_GET_PRIVATE(object);
    switch (prop_id) {
    case PROP_EVENT_LOOP_BACKEND:
        g_value_set_enum(value, milter_client_get_event_loop_backend(client));
        break;
    case PROP_N_WORKERS:
        g_value_set_uint(value, milter_client_get_n_workers(client));
        break;
    case PROP_CUSTOM_FORK:
        g_value_set_pointer(value, priv->custom_fork);
        break;
    case PROP_DEFAULT_PACKET_BUFFER_SIZE:
        g_value_set_uint(value,
                         milter_client_get_default_packet_buffer_size(client));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

GQuark
milter_client_error_quark (void)
{
    return g_quark_from_static_string("milter-client-error-quark");
}

MilterClient *
milter_client_new (void)
{
    return g_object_new(MILTER_TYPE_CLIENT,
                        NULL);
}

static void
change_unix_socket_group (MilterClient *client, struct sockaddr_un *address_un)
{
    const gchar *socket_group;
    struct group *group;

    socket_group = milter_client_get_unix_socket_group(client);
    if (!socket_group)
        return;

    errno = 0;
    group = getgrnam(socket_group);
    if (!group) {
        if (errno == 0) {
            milter_error(
                "[client][error][unix] "
                "failed to find group entry for UNIX socket group: <%s>: <%s>",
                address_un->sun_path, socket_group);
        } else {
            milter_error(
                "[client][error][unix] "
                "failed to get group entry for UNIX socket group: "
                "<%s>: <%s>: %s",
                address_un->sun_path, socket_group, g_strerror(errno));
        }
        return;
    }

    if (chown(address_un->sun_path, -1, group->gr_gid) == -1) {
        milter_error(
            "[client][error][unix] "
            "failed to change UNIX socket group: <%s>: <%s>: %s",
            address_un->sun_path, socket_group, g_strerror(errno));
    }
}

static void
change_unix_socket_mode (MilterClient *client, struct sockaddr_un *address_un)
{
    guint mode;

    mode = milter_client_get_unix_socket_mode(client);
    if (g_chmod(address_un->sun_path, mode) == -1) {
        GError *error;

        error = g_error_new(MILTER_CLIENT_ERROR,
                            MILTER_CLIENT_ERROR_UNIX_SOCKET,
                            "failed to change the mode of UNIX socket: "
                            "%s(%o): %s",
                            address_un->sun_path, mode, g_strerror(errno));
        milter_error("[client][error][unix] %s", error->message);
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client), error);
        g_error_free(error);
    }
}

static void
listen_started (MilterClient *client,
                struct sockaddr *address, socklen_t address_size)
{
    struct sockaddr_un *address_un;

    if (address->sa_family != AF_UNIX)
        return;

    address_un = (struct sockaddr_un *)address;

    change_unix_socket_group(client, address_un);
    change_unix_socket_mode(client, address_un);
}

static gchar *
get_default_connection_spec (MilterClient *client)
{
    return g_strdup("inet:10025@[127.0.0.1]");
}

gchar *
milter_client_get_default_connection_spec (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_default_connection_spec)
        return klass->get_default_connection_spec(client);
    else
        return NULL;
}

const gchar *
milter_client_get_connection_spec (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->connection_spec;
}

gboolean
milter_client_set_connection_spec (MilterClient *client, const gchar *spec,
                                   GError **error)
{
    MilterClientPrivate *priv;
    gboolean success;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->connection_spec) {
        g_free(priv->connection_spec);
        priv->connection_spec = NULL;
    }

    if (!spec)
        return TRUE;

    success = milter_connection_parse_spec(spec, NULL, NULL, NULL, error);
    if (success)
        priv->connection_spec = g_strdup(spec);

    return success;
}

GIOChannel *
milter_client_get_listen_channel (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->listen_channel;
}

void
milter_client_set_listen_channel (MilterClient *client, GIOChannel *channel)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->listen_channel)
        g_io_channel_unref(priv->listen_channel);
    priv->listen_channel = channel;
    if (priv->listen_channel)
        g_io_channel_ref(priv->listen_channel);
}

static gboolean
single_thread_finisher (gpointer _data)
{
    MilterClientProcessData *data = _data;

    milter_debug("[client][finisher][run]");

    data->priv->finisher_id = 0;
    dispose_finished_data(data->client);

    return FALSE;
}

static void
single_thread_cb_finished (MilterClientContext *context, gpointer _data)
{
    MilterClientPrivate *priv;
    MilterClientProcessData *data = _data;

    dispose_process_data_finished_handler(data);
    priv = data->priv;
    priv->finished_data = g_list_prepend(priv->finished_data, data);
    if (priv->finisher_id == 0) {
        priv->finisher_id =
            milter_event_loop_add_idle_full(priv->process_loop,
                                            G_PRIORITY_DEFAULT,
                                            single_thread_finisher,
                                            data,
                                            NULL);
    }
}

MilterClientContext *
milter_client_create_context (MilterClient *client)
{
    MilterClientPrivate *priv;
    MilterClientContext *context;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    context = milter_client_context_new(client);
    milter_client_context_set_packet_buffer_size(
        context,
        priv->default_packet_buffer_size);

    return context;
}

typedef struct _ClientChannelSetupData
{
    MilterClient *client;
    GIOChannel *channel;
    MilterGenericSocketAddress address;
} ClientChannelSetupData;

static void
single_thread_client_channel_setup (MilterClient *client,
                                    GIOChannel *channel,
                                    MilterGenericSocketAddress *address)
{
    MilterClientPrivate *priv;
    MilterClientContext *context;
    MilterAgent *agent;
    MilterWriter *writer;
    MilterReader *reader;
    MilterClientProcessData *data;
    GError *error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    context = milter_client_create_context(client);
    agent = MILTER_AGENT(context);

    milter_agent_set_event_loop(agent, priv->process_loop);

    writer = milter_writer_io_channel_new(channel);
    milter_agent_set_writer(agent, writer);
    g_object_unref(writer);

    reader = milter_reader_io_channel_new(channel);
    milter_agent_set_reader(agent, reader);
    g_object_unref(reader);

    milter_client_context_set_timeout(context, priv->timeout);

    milter_client_context_set_socket_address(context, address);

    data = g_new(MilterClientProcessData, 1);
    data->priv = priv;
    data->client = client;
    data->context = context;

    milter_debug("[%u] [client][single-thread][start]",
                 milter_agent_get_tag(agent));

    data->finished_handler_id =
        g_signal_connect(context, "finished",
                         G_CALLBACK(single_thread_cb_finished), data);

    priv->processing_data = g_list_prepend(priv->processing_data, data);

    if (milter_agent_start(agent, &error)) {
        g_signal_emit(client, signals[CONNECTION_ESTABLISHED], 0, context);
    } else {
        milter_error("[%u] [client][single-thread][start][error] %s",
                     milter_agent_get_tag(agent), error->message);
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(agent), error);
        g_error_free(error);
        milter_finished_emittable_emit(MILTER_FINISHED_EMITTABLE(context));
    }

    g_io_channel_unref(channel);
}

static gboolean
single_thread_cb_idle_client_channel_setup (gpointer user_data)
{
    ClientChannelSetupData *setup_data = user_data;

    single_thread_client_channel_setup(setup_data->client,
                                       setup_data->channel,
                                       &setup_data->address);
    g_free(setup_data);

    return FALSE;
}

static void
single_thread_process_client_channel (MilterClient *client, GIOChannel *channel,
                                      MilterGenericSocketAddress *address,
                                      socklen_t address_size)
{
    MilterClientPrivate *priv;
    ClientChannelSetupData *data;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    data = g_new(ClientChannelSetupData, 1);
    data->client = client;
    data->channel = channel;
    memcpy(&(data->address), address, address_size);
    g_io_channel_ref(channel);

    milter_event_loop_add_idle_full(priv->process_loop,
                                    G_PRIORITY_DEFAULT,
                                    single_thread_cb_idle_client_channel_setup,
                                    data,
                                    NULL);
}

static gint
accept_connection_fd (MilterClient *client, gint server_fd,
                      MilterGenericSocketAddress *address,
                      socklen_t *address_size)
{
    MilterClientPrivate *priv;
    gint client_fd;
    guint n_suspend, suspend_time, max_connections;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    suspend_time = milter_client_get_suspend_time_on_unacceptable(client);
    max_connections = milter_client_get_max_connections(client);
    for (n_suspend = 0;
         0 < max_connections && max_connections <= priv->n_processing_sessions;
         n_suspend++) {
        milter_warning("[client][accept][suspend] "
                       "too many processing connection: %u, max: %u; "
                       "suspend accepting connection in %d seconds: #%u",
                       priv->n_processing_sessions,
                       max_connections,
                       suspend_time,
                       n_suspend);
        g_usleep(suspend_time * G_USEC_PER_SEC);
        milter_warning("[client][accept][resume] "
                       "resume accepting connection: #%u", n_suspend);
    }

    *address_size = sizeof(*address);
    memset(address, '\0', *address_size);
    client_fd = accept(server_fd, (struct sockaddr *)(address), address_size);
    if (client_fd == -1) {
        GError *error = NULL;
        g_set_error(&error,
                    MILTER_CONNECTION_ERROR,
                    MILTER_CONNECTION_ERROR_ACCEPT_FAILURE,
                    "failed to accept(): %s", g_strerror(errno));
        milter_error("[client][error][accept] %s", g_strerror(errno));
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client),
                                    error);
        g_error_free(error);

        if (errno == EMFILE) {
            milter_warning("[client][accept][suspend] "
                           "too many file is opened. "
                           "suspend accepting connection in %d seconds",
                           suspend_time);
            g_usleep(suspend_time * G_USEC_PER_SEC);
            milter_warning("[client][accept][resume] "
                           "resume accepting connection.");
        }

        return client_fd;
    }

    milter_client_session_started(client);
    if (milter_need_debug_log()) {
        gchar *spec;
        spec = milter_connection_address_to_spec(&(address->address.base));
        milter_debug("[client][accept] %d:%s", client_fd, spec);
        g_free(spec);
    }

    return client_fd;
}

static GIOChannel *
setup_client_channel(gint client_fd)
{
    GIOChannel *client_channel = g_io_channel_unix_new(client_fd);
    g_io_channel_set_encoding(client_channel, NULL, NULL);
    g_io_channel_set_flags(client_channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_close_on_unref(client_channel, TRUE);
    return client_channel;
}

static gboolean
accept_connection (MilterClient *client, gint server_fd,
                   GIOChannel **client_channel,
                   MilterGenericSocketAddress *address,
                   socklen_t *address_size)
{
    gint client_fd;

    client_fd = accept_connection_fd(client, server_fd, address, address_size);
    if (client_fd == -1)
        return FALSE;

    *client_channel = setup_client_channel(client_fd);

    return TRUE;
}

static gboolean
single_thread_accept_connection (MilterClient *client, gint server_fd)
{
    gboolean accepted;
    GIOChannel *client_channel;
    MilterGenericSocketAddress address;
    socklen_t address_size;

    accepted = accept_connection(client, server_fd, &client_channel,
                                 &address, &address_size);
    if (accepted) {
        single_thread_process_client_channel(client, client_channel,
                                             &address, address_size);
        g_io_channel_unref(client_channel);
    }

    return TRUE;
}

static gboolean
single_thread_accept_watch_func (GIOChannel *channel, GIOCondition condition,
                                 gpointer data)
{
    MilterClient *client = data;
    gboolean keep_callback = TRUE;
    gint fd;

    fd = g_io_channel_unix_get_fd(channel);
    keep_callback = single_thread_accept_connection(client, fd);
    if (!keep_callback)
        milter_client_shutdown(client);
    return keep_callback;
}

static gboolean
single_thread_cb_idle_unlock_quit_mutex (gpointer data)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(data);

    g_mutex_unlock(priv->quit_mutex);

    return FALSE;
}

static void
single_thread_accept_loop_run (MilterClient *client,
                               MilterEventLoop *accept_loop)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    milter_event_loop_add_idle_full(accept_loop, G_PRIORITY_DEFAULT,
                                    single_thread_cb_idle_unlock_quit_mutex,
                                    client, NULL);

    g_mutex_lock(priv->quit_mutex);
    if (priv->quitting) {
        g_mutex_unlock(priv->quit_mutex);
    } else {
        milter_event_loop_run(accept_loop);
    }
}

static gpointer
single_thread_accept_thread (gpointer data)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(data);
    single_thread_accept_loop_run(data, priv->accept_loop);

    return NULL;
}

static gboolean
single_thread_start_accept (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    GThread *thread;
    GError *local_error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    thread = g_thread_create(single_thread_accept_thread, client, TRUE,
                             &local_error);
    if (!thread) {
        milter_error("[client][single-thread][accept][start][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    g_mutex_lock(priv->quit_mutex);
    if (priv->quitting) {
        g_mutex_unlock(priv->quit_mutex);
    } else {
        MilterEventLoop *loop;
        loop = milter_client_get_process_loop(client);
        milter_event_loop_add_idle_full(loop,
                                        G_PRIORITY_DEFAULT,
                                        single_thread_cb_idle_unlock_quit_mutex,
                                        client,
                                        NULL);
        milter_event_loop_run(loop);
    }
    g_thread_join(thread);

    return TRUE;
}

static void
multi_thread_process_client_channel (MilterClient *client, GIOChannel *channel,
                                     MilterGenericSocketAddress *address,
                                     socklen_t address_size)
{
    MilterClientPrivate *priv;
    MilterClientContext *context;
    MilterClientProcessData *data;
    MilterWriter *writer;
    MilterReader *reader;
    GError *error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    context = milter_client_create_context(client);

    writer = milter_writer_io_channel_new(channel);
    milter_agent_set_writer(MILTER_AGENT(context), writer);
    g_object_unref(writer);

    reader = milter_reader_io_channel_new(channel);
    milter_agent_set_reader(MILTER_AGENT(context), reader);
    g_object_unref(reader);

    milter_client_context_set_timeout(context, priv->timeout);

    milter_client_context_set_socket_address(context, address);

    data = g_new(MilterClientProcessData, 1);
    data->priv = priv;
    data->client = client;
    data->context = context;
    data->finished_handler_id = 0;

    priv->processing_data = g_list_prepend(priv->processing_data, data);
    g_thread_pool_push(priv->worker_threads, data, &error);
    if (error) {
        GError *client_error;

        client_error = g_error_new(MILTER_CLIENT_ERROR,
                                   MILTER_CLIENT_ERROR_THREAD,
                                   "failed to push a data to thread pool: %s",
                                   error->message);
        g_error_free(error);
        milter_error("[%u] [client][multi-thread][error] %s",
                     milter_agent_get_tag(MILTER_AGENT(context)),
                     client_error->message);
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client),
                                    client_error);
        g_error_free(client_error);

        process_data_free(data);
        priv->processing_data = g_list_remove(priv->processing_data, data);
    }
}

static gboolean
multi_thread_accept_connection (MilterClient *client, gint server_fd)
{
    MilterClientPrivate *priv;
    gboolean accepted;
    GIOChannel *client_channel;
    MilterGenericSocketAddress address;
    socklen_t address_size;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    accepted = accept_connection(client, server_fd, &client_channel,
                                 &address, &address_size);
    if (accepted) {
        multi_thread_process_client_channel(client, client_channel,
                                            &address, address_size);
        g_io_channel_unref(client_channel);
    }

    return TRUE;
}

static gboolean
multi_thread_accept_watch_func (GIOChannel *channel, GIOCondition condition,
                                gpointer data)
{
    MilterClient *client = data;
    gboolean keep_callback = TRUE;
    gint fd;

    fd = g_io_channel_unix_get_fd(channel);
    keep_callback = multi_thread_accept_connection(client, fd);
    if (!keep_callback)
        milter_client_shutdown(client);
    return keep_callback;
}

static void
multi_thread_cb_finished (MilterClientContext *context, gpointer _data)
{
    MilterClientProcessData *data = _data;

    finish_processing(data);
}

static void
multi_thread_process_client_channel_thread (gpointer data_, gpointer user_data)
{
    MilterAgent *agent;
    MilterClientContext *context;
    MilterClient *client = data_;
    MilterClientProcessData *data = user_data;
    MilterEventLoop *process_loop;
    GError *error = NULL;

    process_loop = milter_client_create_event_loop(client, FALSE);

    context = MILTER_CLIENT_CONTEXT(data->context);
    agent = MILTER_AGENT(context);
    milter_debug("[%u] [client][multi-thread][start]",
                 milter_agent_get_tag(agent));

    data->finished_handler_id =
        g_signal_connect(data->context, "finished",
                         G_CALLBACK(multi_thread_cb_finished), data);

    milter_agent_set_event_loop(agent, process_loop);
    if (milter_agent_start(agent, &error)) {
        g_signal_emit(client, signals[CONNECTION_ESTABLISHED], 0, context);
        milter_event_loop_run(process_loop);
    } else {
        milter_error("[%u] [client][multi-thread][start][error] %s",
                     milter_agent_get_tag(agent), error->message);
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client),
                                    error);
        g_error_free(error);
        milter_finished_emittable_emit(MILTER_FINISHED_EMITTABLE(context));
    }

    g_object_unref(process_loop);
}

static gboolean
multi_thread_start_accept (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    gint max_threads = 10;
    GError *local_error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->worker_threads =
        g_thread_pool_new(multi_thread_process_client_channel_thread,
                          client,
                          max_threads,
                          FALSE,
                          &local_error);
    if (!priv->worker_threads) {
        GError *client_error;
        client_error = g_error_new(MILTER_CLIENT_ERROR,
                                   MILTER_CLIENT_ERROR_THREAD,
                                   "failed to create a thread pool "
                                   "for processing accepted connection: %s",
                                   local_error->message);
        g_error_free(local_error);
        milter_error("[client][multi-thread][accept][error] %s",
                     client_error->message);
        g_propagate_error(error, client_error);
        return FALSE;
    }

    milter_event_loop_run(priv->accept_loop);

    g_thread_pool_free(priv->worker_threads, TRUE, TRUE);
    priv->worker_threads = NULL;

    return TRUE;
}


static GIOChannel *
milter_client_listen_channel (MilterClient  *client, GError **error)
{
    MilterClientPrivate *priv;
    GIOChannel *channel;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    dispose_address(priv);

    if (!priv->connection_spec) {
        gchar *default_connection_spec;

        default_connection_spec =
            milter_client_get_default_connection_spec(client);
        milter_client_set_connection_spec(client, default_connection_spec,
                                          NULL);
        g_free(default_connection_spec);
    }

    channel = milter_connection_listen(priv->connection_spec,
                                       priv->listen_backlog,
                                       &(priv->address),
                                       &(priv->address_size),
                                       priv->remove_unix_socket_on_create,
                                       error);
    if (priv->address_size > 0) {
        g_signal_emit(client, signals[LISTEN_STARTED], 0,
                      priv->address, priv->address_size);
    }

    return channel;
}

gboolean
milter_client_listen (MilterClient  *client, GError **error)
{
    GIOChannel *channel;

    channel = milter_client_listen_channel(client, error);
    if (!channel)
        return FALSE;

    milter_client_set_listen_channel(client, channel);
    g_io_channel_unref(channel);
    return TRUE;
}

static struct passwd *
find_password (const gchar *effective_user, GError **error)
{
    struct passwd *password;

    if (!effective_user)
        effective_user = "nobody";

    errno = 0;
    password = getpwnam(effective_user);
    if (!password) {
        if (errno == 0) {
            g_set_error(
                error,
                MILTER_CLIENT_ERROR,
                MILTER_CLIENT_ERROR_PASSWORD_ENTRY,
                "failed to find password entry for effective user: %s",
                effective_user);
        } else {
            g_set_error(
                error,
                MILTER_CLIENT_ERROR,
                MILTER_CLIENT_ERROR_PASSWORD_ENTRY,
                "failed to get password entry for effective user: %s: %s",
                effective_user, g_strerror(errno));
        }
    }

    return password;
}

static gboolean
switch_user (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    const gchar *effective_user;
    struct passwd *password;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    effective_user = milter_client_get_effective_user(client);
    password = find_password(effective_user, error);
    if (!password)
        return FALSE;

    if (priv->address && priv->address->sa_family == AF_UNIX) {
        struct sockaddr_un *address_un;
        address_un = (struct sockaddr_un *)priv->address;
        if (chown(address_un->sun_path, password->pw_uid, -1) == -1) {
            g_set_error(error,
                        MILTER_CLIENT_ERROR,
                        MILTER_CLIENT_ERROR_DROP_PRIVILEGE,
                        "failed to change UNIX socket owner: "
                        "<%s>: <%s>: %s",
                        address_un->sun_path, password->pw_name,
                        g_strerror(errno));
        }
    }

    if (setuid(password->pw_uid) == -1) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DROP_PRIVILEGE,
                    "failed to change effective user: %s: %s",
                    password->pw_name, g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

static gboolean
switch_group (MilterClient *client, GError **error)
{
    const gchar *effective_user;
    const gchar *effective_group;
    struct passwd *password;
    struct group *group;

    effective_group = milter_client_get_effective_group(client);
    if (!effective_group)
        return TRUE;

    errno = 0;
    group = getgrnam(effective_group);
    if (!group) {
        if (errno == 0) {
            g_set_error(
                error,
                MILTER_CLIENT_ERROR,
                MILTER_CLIENT_ERROR_GROUP_ENTRY,
                "failed to find group entry for effective group: %s",
                effective_group);
        } else {
            g_set_error(
                error,
                MILTER_CLIENT_ERROR,
                MILTER_CLIENT_ERROR_GROUP_ENTRY,
                "failed to get group entry for effective group: %s: %s",
                effective_group, g_strerror(errno));
        }
        return FALSE;
    }


    if (setgid(group->gr_gid) == -1) {
            g_set_error(error,
                        MILTER_CLIENT_ERROR,
                        MILTER_CLIENT_ERROR_GROUP_ENTRY,
                        "failed to change effective group: %s: %s",
                        effective_group, g_strerror(errno));
        return FALSE;
    }

    effective_user = milter_client_get_effective_user(client);
    password = find_password(effective_user, error);
    if (!password)
        return FALSE;

    if (initgroups(password->pw_name, group->gr_gid) == -1) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_GROUP_ENTRY,
                    "failed to initialize groups: %s: %s: %s",
                    password->pw_name, group->gr_name,
                    g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

gboolean
milter_client_drop_privilege (MilterClient *client, GError **error)
{
    if (geteuid() != 0)
        return TRUE;

    return switch_group(client, error) && switch_user(client, error);
}

gboolean
milter_client_daemonize (MilterClient *client, GError **error)
{
    gchar *error_message = NULL;

    switch (milter_client_fork(client)) {
    case 0:
        break;
    case -1:
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DAEMONIZE,
                    "failed to fork child process: %s",
                    g_strerror(errno));
        return FALSE;
    default:
        _exit(EXIT_SUCCESS);
        break;
    }

    if (setsid() == -1) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DAEMONIZE,
                    "failed to create session: %s",
                    g_strerror(errno));
        return FALSE;
    }

    switch (milter_client_fork(client)) {
    case 0:
        break;
    case -1:
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DAEMONIZE,
                    "failed to fork grandchild process: %s",
                    g_strerror(errno));
        return FALSE;
    default:
        _exit(EXIT_SUCCESS);
        break;
    }

    if (g_chdir("/") == -1) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DAEMONIZE,
                    "failed to change working directory to '/': %s",
                    g_strerror(errno));
        return FALSE;
    }

    if (!milter_utils_detach_io(&error_message)) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_DETACH_IO,
                    "%s",
                    error_message);
        g_free(error_message);
        return FALSE;
    }

    return TRUE;
}

static gboolean
accept_error_watch_func (GIOChannel *channel, GIOCondition condition,
                         gpointer user_data)
{
    MilterClient *client = user_data;
    MilterClientPrivate *priv;
    gchar *message;
    GError *error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    message = milter_utils_inspect_io_condition_error(condition);
    error = g_error_new(MILTER_CLIENT_ERROR,
                        MILTER_CLIENT_ERROR_IO_ERROR,
                        "IO error on waiting MTA connection socket: %s",
                        message);
    milter_error("[client][watch][error] %s", error->message);
    milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client),
                                error);
    g_error_free(error);

    milter_client_shutdown(client);

    return FALSE;
}

static gboolean
milter_client_prepare (MilterClient *client, GIOFunc accept_func, GError **error)
{
    MilterClientPrivate *priv;
    GError *local_error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    if (!priv->accept_loop)
        priv->accept_loop = milter_client_create_event_loop(client, FALSE);

    if (priv->listening_channel || priv->n_processing_sessions > 0) {
        local_error = g_error_new(MILTER_CLIENT_ERROR,
                                  MILTER_CLIENT_ERROR_RUNNING,
                                  "The milter client is already running: <%p>",
                                  client);
        milter_error("[client][prepare][error] %s", local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    priv->quitting = FALSE;

    if (priv->listen_channel) {
        g_io_channel_ref(priv->listen_channel);
        priv->listening_channel = priv->listen_channel;
    } else {
        priv->listening_channel = milter_client_listen_channel(client,
                                                               &local_error);
    }

    if (!priv->listening_channel) {
        milter_error("[client][prepare][listen][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    priv->accept_watch_id =
        milter_event_loop_watch_io(priv->accept_loop,
                                   priv->listening_channel,
                                   G_IO_IN | G_IO_PRI,
                                   accept_func, client);
    priv->accept_error_watch_id =
        milter_event_loop_watch_io(priv->accept_loop,
                                   priv->listening_channel,
                                   G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                   accept_error_watch_func,
                                   client);

    return TRUE;
}

static void
milter_client_cleanup (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    if (priv->listening_channel) {
        g_io_channel_unref(priv->listening_channel);
        priv->listening_channel = NULL;
    }

    if (priv->address) {
        if (priv->address->sa_family == AF_UNIX &&
            milter_client_is_remove_unix_socket_on_close(client)) {
            struct sockaddr_un *address_un;

            address_un = (struct sockaddr_un *)priv->address;
            if (g_unlink(address_un->sun_path) == -1) {
                GError *error;

                error = g_error_new(MILTER_CLIENT_ERROR,
                                    MILTER_CLIENT_ERROR_UNIX_SOCKET,
                                    "failed to remove used UNIX socket: %s: %s",
                                    address_un->sun_path, g_strerror(errno));
                milter_error("[client][unix][error] %s", error->message);
                milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client),
                                            error);
                g_error_free(error);
            }
        }
    }
}

static gboolean
worker_watch_master (GIOChannel   *source,
                     GIOCondition  condition,
                     gpointer      data)
{
    gchar buf[1];
    gsize count;

    if (g_io_channel_read_chars(source, buf, 1, &count, NULL) == G_IO_STATUS_EOF) {
        MilterClient *client = data;
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        if (priv->listening_channel) {
            GIOChannel *listening_channel = priv->listening_channel;
            priv->listening_channel = NULL;
            g_io_channel_unref(listening_channel);
        }
        if (priv->listen_channel) {
            GIOChannel *listen_channel = priv->listen_channel;
            priv->listen_channel = NULL;
            g_io_channel_unref(listen_channel);
        }
        milter_client_shutdown(client);
        return FALSE;
    }

    return TRUE;
}

static gboolean
client_run_workers (MilterClient *client, guint n_workers, GError **error)
{
    guint i;
    int pipe_fds[2];
    MilterClientPrivate *priv;
    MilterEventLoop *loop;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    loop = milter_client_get_process_loop(client);
    if (!priv->listen_channel) {
        GError *local_error = NULL;
        if (!milter_client_listen(client, &local_error)) {
            milter_error("[client][workers][run][listen][error] %s",
                         local_error->message);
            g_propagate_error(error, local_error);
            return FALSE;
        }
    }

    if (pipe(pipe_fds) == -1) {
        g_set_error(error,
                    MILTER_CLIENT_ERROR,
                    MILTER_CLIENT_ERROR_PROCESS,
                    "%s",
                    g_strerror(errno));
        return FALSE;
    }
    priv->workers.control = setup_client_channel(pipe_fds[MILTER_UTILS_READ_PIPE]);
    for (i = 0; i < n_workers; ++i) {
        GPid pid = milter_client_fork(client);
        switch (pid) {
        case 0:
            close(pipe_fds[MILTER_UTILS_WRITE_PIPE]);
            milter_event_loop_watch_io(loop, priv->workers.control,
                                       G_IO_IN | G_IO_PRI | G_IO_ERR,
                                       worker_watch_master, client);
            run_worker(client, error);
            milter_client_shutdown(client);
            _exit(EXIT_SUCCESS);
        default:
            milter_event_loop_watch_child(loop, pid, watch_worker_process, NULL);
            break;
        case -1:
            g_set_error(error,
                        MILTER_CLIENT_ERROR,
                        MILTER_CLIENT_ERROR_PROCESS,
                        "%s",
                        g_strerror(errno));
            close(pipe_fds[MILTER_UTILS_WRITE_PIPE]);
            g_io_channel_unref(priv->workers.control);
            return FALSE;
        }
    }
    g_io_channel_unref(priv->workers.control);
    priv->workers.control = setup_client_channel(pipe_fds[MILTER_UTILS_WRITE_PIPE]);
    return TRUE;
}

gboolean
milter_client_run (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    gboolean success;
    guint n_workers;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    n_workers = milter_client_get_n_workers(client);
    if (n_workers > 0) {
        if (!client_run_workers(client, n_workers, error)) {
            return FALSE;
        }
        success = run_master(client, error);
    } else if (priv->multi_thread_mode) {
        if (!milter_client_prepare(client, multi_thread_accept_watch_func,
                                   error))
            return FALSE;
        success = multi_thread_start_accept(client, error);
    } else {
        if (!milter_client_prepare(client, single_thread_accept_watch_func,
                                   error))
            return FALSE;
        success = single_thread_start_accept(client, error);
    }

    milter_client_cleanup(client);

    return success;
}

gboolean
milter_client_main (MilterClient *client)
{
    gboolean success;
    GError *error = NULL;

    success = milter_client_run(client, &error);
    if (error) {
        milter_error("[client][main][error] %s", error->message);
        milter_error_emittable_emit(MILTER_ERROR_EMITTABLE(client), error);
        g_error_free(error);
    }
    return success;
}

gboolean
milter_client_run_master (MilterClient *client, GError **error)
{
    if (!run_master(client, error)) {
        /* TODO: report error */
        return FALSE;
    }
    milter_client_cleanup(client);
    return TRUE;
}

static gboolean
run_master (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    GError *local_error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    if (priv->accept_loop) {
        local_error = g_error_new(MILTER_CLIENT_ERROR,
                                  MILTER_CLIENT_ERROR_RUNNING,
                                  "The milter client is already running: <%p>",
                                  client);
        milter_error("[client][master][run][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    priv->accept_loop = milter_client_create_event_loop(client, TRUE);
    priv->quitting = FALSE;

    single_thread_accept_loop_run(client, priv->accept_loop);

    return TRUE;
}

static gpointer
worker_accept_thread (gpointer data)
{
    MilterClient *client = data;
    gint server_fd, client_fd;
    MilterGenericSocketAddress address;
    socklen_t address_size;
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    server_fd = g_io_channel_unix_get_fd(priv->listening_channel);
    while ((client_fd = accept_connection_fd(client, server_fd,
                                             &address, &address_size)) != -1) {
        GIOChannel *client_channel;
        client_channel = setup_client_channel(client_fd);
        single_thread_process_client_channel(client, client_channel,
                                             &address, (socklen_t)address_size);
        g_io_channel_unref(client_channel);
    }

    return NULL;
}

gboolean
milter_client_run_worker (MilterClient *client, GError **error)
{
    GThread *thread = run_worker(client, error);

    if (!thread) {
        /* TODO: report error. */
        return FALSE;
    }

    g_thread_join(thread);

    return TRUE;
}

static GThread *
run_worker (MilterClient *client, GError **error)
{
    MilterClientPrivate *priv;
    GThread *thread;
    GError *local_error = NULL;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->listening_channel || priv->n_processing_sessions > 0) {
        local_error = g_error_new(MILTER_CLIENT_ERROR,
                                  MILTER_CLIENT_ERROR_RUNNING,
                                  "The milter client worker is already running"
                                  ": <%p>",
                                  client);
        milter_error("[client][worker][run][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return NULL;
    }

    if (!priv->listen_channel) {
        local_error = g_error_new(MILTER_CLIENT_ERROR,
                                  MILTER_CLIENT_ERROR_NOT_LISTENED_YET,
                                  "worker client should listen "
                                  "before running: <%p>",
                                  client);
        milter_error("[client][worker][run][listen][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return NULL;
    }

    g_io_channel_ref(priv->listen_channel);
    priv->listening_channel = priv->listen_channel;

    thread = g_thread_create(worker_accept_thread, client, TRUE, &local_error);
    if (!thread) {
        milter_error("[client][worker][run][accept][thread][error] %s",
                     local_error->message);
        g_propagate_error(error, local_error);
        return NULL;
    }

    g_mutex_lock(priv->quit_mutex);
    if (priv->quitting) {
        g_mutex_unlock(priv->quit_mutex);
    } else {
        MilterEventLoop *loop;
        loop = milter_client_get_process_loop(client);
        milter_event_loop_add_idle_full(loop,
                                        G_PRIORITY_DEFAULT,
                                        single_thread_cb_idle_unlock_quit_mutex,
                                        client,
                                        NULL);
        milter_event_loop_run(loop);
    }

    return thread;
}

void
milter_client_shutdown (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    g_mutex_lock(priv->quit_mutex);
    if (!priv->quitting) {
        priv->quitting = TRUE;

        if (priv->accept_loop)
            milter_event_loop_quit(priv->accept_loop);
        dispose_accept_watchers(priv);
        if (priv->workers.control) {
            g_io_channel_unref(priv->workers.control);
            priv->workers.control = NULL;
        }
        if (priv->listening_channel) {
            g_io_channel_unref(priv->listening_channel);
            priv->listening_channel = NULL;
        }

        if (priv->n_processing_sessions == 0)
            milter_event_loop_quit(priv->process_loop);
    }
    g_mutex_unlock(priv->quit_mutex);
}

void
milter_client_set_listen_backlog (MilterClient *client, gint backlog)
{
    MILTER_CLIENT_GET_PRIVATE(client)->listen_backlog = backlog;
}

gboolean
milter_client_is_remove_unix_socket_on_create (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->remove_unix_socket_on_create;
}

void
milter_client_set_remove_unix_socket_on_create (MilterClient *client,
                                                gboolean remove)
{
    MILTER_CLIENT_GET_PRIVATE(client)->remove_unix_socket_on_create = remove;
}

void
milter_client_set_timeout (MilterClient *client, guint timeout)
{
    MILTER_CLIENT_GET_PRIVATE(client)->timeout = timeout;
}

guint
milter_client_get_unix_socket_mode (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_unix_socket_mode) {
        return klass->get_unix_socket_mode(client);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        if (priv->unix_socket_mode != 0) {
            return priv->unix_socket_mode;
        } else {
            return milter_client_get_default_unix_socket_mode(client);
        }
    }
}

void
milter_client_set_unix_socket_mode (MilterClient *client, guint mode)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_unix_socket_mode) {
        klass->set_unix_socket_mode(client, mode);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        priv->unix_socket_mode = mode;
    }
}

guint
milter_client_get_default_unix_socket_mode (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->default_unix_socket_mode;
}

void
milter_client_set_default_unix_socket_mode (MilterClient *client, guint mode)
{
    MILTER_CLIENT_GET_PRIVATE(client)->default_unix_socket_mode = mode;
}

const gchar *
milter_client_get_unix_socket_group (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_unix_socket_group) {
        return klass->get_unix_socket_group(client);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        if (priv->unix_socket_group) {
            return priv->unix_socket_group;
        } else {
            return milter_client_get_default_unix_socket_group(client);
        }
    }
}

void
milter_client_set_unix_socket_group (MilterClient *client, const gchar *group)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_unix_socket_group) {
        klass->set_unix_socket_group(client, group);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        g_free(priv->unix_socket_group);
        priv->unix_socket_group = g_strdup(group);
    }
}

const gchar *
milter_client_get_default_unix_socket_group (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->default_unix_socket_group;
}

void
milter_client_set_default_unix_socket_group (MilterClient *client,
                                             const gchar *group)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->default_unix_socket_group)
        g_free(priv->default_unix_socket_group);
    priv->default_unix_socket_group = g_strdup(group);
}

gboolean
milter_client_is_remove_unix_socket_on_close (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->is_remove_unix_socket_on_close)
        return klass->is_remove_unix_socket_on_close(client);
    else
        return milter_client_get_default_remove_unix_socket_on_close(client);
}

gboolean
milter_client_get_default_remove_unix_socket_on_close (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->default_remove_unix_socket_on_close;
}

void
milter_client_set_default_remove_unix_socket_on_close (MilterClient *client,
                                                       gboolean remove)
{
    MILTER_CLIENT_GET_PRIVATE(client)->default_remove_unix_socket_on_close = remove;
}

guint
milter_client_get_suspend_time_on_unacceptable (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_suspend_time_on_unacceptable)
        return klass->get_suspend_time_on_unacceptable(client);
    else
        return MILTER_CLIENT_GET_PRIVATE(client)->suspend_time_on_unacceptable;
}

void
milter_client_set_suspend_time_on_unacceptable (MilterClient *client,
                                                guint suspend_time)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_suspend_time_on_unacceptable)
        klass->set_suspend_time_on_unacceptable(client, suspend_time);
    else
        MILTER_CLIENT_GET_PRIVATE(client)->suspend_time_on_unacceptable = suspend_time;
}

guint
milter_client_get_max_connections (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_max_connections)
        return klass->get_max_connections(client);
    else
        return MILTER_CLIENT_GET_PRIVATE(client)->max_connections;
}

void
milter_client_set_max_connections (MilterClient *client, guint max_connections)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_max_connections)
        klass->set_max_connections(client, max_connections);
    else
        MILTER_CLIENT_GET_PRIVATE(client)->max_connections = max_connections;
}

const gchar *
milter_client_get_effective_user (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_effective_user)
        return klass->get_effective_user(client);
    else
        return MILTER_CLIENT_GET_PRIVATE(client)->effective_user;
}

void
milter_client_set_effective_user (MilterClient *client,
                                  const gchar *effective_user)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_effective_user) {
        klass->set_effective_user(client, effective_user);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        g_free(priv->effective_user);
        priv->effective_user = g_strdup(effective_user);
    }
}

const gchar *
milter_client_get_effective_group (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_effective_group)
        return klass->get_effective_group(client);
    else
        return MILTER_CLIENT_GET_PRIVATE(client)->effective_group;
}

void
milter_client_set_effective_group (MilterClient *client,
                                   const gchar *effective_group)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_effective_group) {
        klass->set_effective_group(client, effective_group);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        g_free(priv->effective_group);
        priv->effective_group = g_strdup(effective_group);
    }
}

guint
milter_client_get_maintenance_interval (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_maintenance_interval)
        return klass->get_maintenance_interval(client);
    else
        return MILTER_CLIENT_GET_PRIVATE(client)->maintenance_interval;
}

void
milter_client_set_maintenance_interval (MilterClient *client, guint n_sessions)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_maintenance_interval)
        klass->set_maintenance_interval(client, n_sessions);
    else
        MILTER_CLIENT_GET_PRIVATE(client)->maintenance_interval = n_sessions;
}

void
milter_client_processing_context_foreach (MilterClient *client,
                                          GFunc func, gpointer user_data)
{
    MilterClientPrivate *priv;
    GList *node;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    for (node = priv->processing_data; node; node = g_list_next(node)) {
        MilterClientProcessData *data = node->data;
        func(data->context, user_data);
    }
}

void
milter_client_session_started (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->n_processing_sessions++;
}

void
milter_client_session_finished (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->n_processing_sessions--;
    priv->n_processed_sessions++;
}

guint
milter_client_get_n_processing_sessions (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->n_processing_sessions;
}

gboolean
milter_client_is_processing (MilterClient *client)
{
    return MILTER_CLIENT_GET_PRIVATE(client)->n_processing_sessions > 0;
}

void
milter_client_start_syslog (MilterClient *client, const gchar *identify)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    if (priv->syslog_logger) {
        g_object_unref(priv->syslog_logger);
    }
    priv->syslog_logger = milter_syslog_logger_new(identify);
}

MilterEventLoop *
milter_client_get_process_loop (MilterClient *client)
{
    MilterClientPrivate *priv;

    priv = MILTER_CLIENT_GET_PRIVATE(client);

    if (priv->multi_thread_mode) {
        return NULL;
    } else {
        if (!priv->process_loop)
            priv->process_loop = milter_client_create_event_loop(client, TRUE);
        return priv->process_loop;
    }
}

MilterClientEventLoopBackend
milter_client_get_event_loop_backend (MilterClient  *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_event_loop_backend)
        return klass->get_event_loop_backend(client);
    return MILTER_CLIENT_GET_PRIVATE(client)->event_loop_backend;
}

void
milter_client_set_event_loop_backend (MilterClient  *client,
                                      MilterClientEventLoopBackend backend)
{
    MilterClientPrivate *priv;
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_event_loop_backend) {
        klass->set_event_loop_backend(client, backend);
        return;
    }

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->event_loop_backend = backend;
}

guint
milter_client_get_n_workers (MilterClient  *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_n_workers)
        return klass->get_n_workers(client);
    return MILTER_CLIENT_GET_PRIVATE(client)->workers.n_process;
}

void
milter_client_set_n_workers (MilterClient  *client,
                             guint          n_workers)
{
    MilterClientPrivate *priv;
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_n_workers) {
        klass->set_n_workers(client, n_workers);
        return;
    }

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->workers.n_process = n_workers;
}

static GPid
default_fork (MilterClient    *client)
{
    return fork();
}

GPid
milter_client_fork (MilterClient *client)
{
    MilterClientClass *client_class;
    MilterClientPrivate *priv;

    g_return_val_if_fail(client != NULL, -1);

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    if (priv->custom_fork)
        return priv->custom_fork(client);

    client_class = MILTER_CLIENT_GET_CLASS(client);
    return client_class->fork(client);
}

GPid
milter_client_fork_without_custom (MilterClient *client)
{
    MilterClientClass *client_class;

    g_return_val_if_fail(client != NULL, -1);

    client_class = MILTER_CLIENT_GET_CLASS(client);
    return client_class->fork(client);
}

void
milter_client_set_custom_fork_func (MilterClient              *client,
                                    MilterClientCustomForkFunc custom_fork)
{
    MilterClientPrivate *priv;

    g_return_if_fail(client != NULL);

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    priv->custom_fork = custom_fork;
}

MilterClientCustomForkFunc
milter_client_get_custom_fork_func (MilterClient *client)
{
    MilterClientPrivate *priv;

    g_return_val_if_fail(client != NULL, NULL);

    priv = MILTER_CLIENT_GET_PRIVATE(client);
    return priv->custom_fork;
}

void
milter_client_set_default_packet_buffer_size (MilterClient *client, guint size)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->set_default_packet_buffer_size) {
        klass->set_default_packet_buffer_size(client, size);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        priv->default_packet_buffer_size = size;
    }
}

guint
milter_client_get_default_packet_buffer_size (MilterClient *client)
{
    MilterClientClass *klass;

    klass = MILTER_CLIENT_GET_CLASS(client);
    if (klass->get_default_packet_buffer_size) {
        return klass->get_default_packet_buffer_size(client);
    } else {
        MilterClientPrivate *priv;

        priv = MILTER_CLIENT_GET_PRIVATE(client);
        return priv->default_packet_buffer_size;
    }
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
