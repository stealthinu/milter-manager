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

#ifndef __MILTER_CONNECTION_H__
#define __MILTER_CONNECTION_H__

#include <sys/types.h>
#include <sys/socket.h>

#include <glib-object.h>

G_BEGIN_DECLS

#define MILTER_CONNECTION_ERROR           (milter_connection_error_quark())

typedef enum
{
    MILTER_CONNECTION_ERROR_INVALID_FORMAT,
    MILTER_CONNECTION_ERROR_SOCKET_FAILURE,
    MILTER_CONNECTION_ERROR_SET_SOCKET_OPTION_FAILURE,
    MILTER_CONNECTION_ERROR_ACCEPT_FAILURE,
    MILTER_CONNECTION_ERROR_BIND_FAILURE,
    MILTER_CONNECTION_ERROR_LISTEN_FAILURE,
    MILTER_CONNECTION_ERROR_IO_ERROR
} MilterConnectionError;

GQuark           milter_connection_error_quark (void);
gboolean         milter_connection_parse_spec  (const gchar      *spec,
                                                gint             *domain,
                                                struct sockaddr **address,
                                                socklen_t        *address_size,
                                                GError          **error);
GIOChannel      *milter_connection_listen      (const gchar      *spec,
                                                gint              backlog,
                                                GError          **error);
gchar           *milter_connection_address_to_spec
                                               (const struct sockaddr *address);

G_END_DECLS

#endif /* __MILTER_CONNECTION_H__ */

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
