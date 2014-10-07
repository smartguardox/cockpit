/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013-2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitwebservice.h"

#include <string.h>

#include <json-glib/json-glib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "common/cockpitjson.h"
#include "common/cockpitlog.h"
#include "common/cockpitpipetransport.h"

#include "cockpitauth.h"
#include "cockpitws.h"

#include "cockpitsshtransport.h"
#include "cockpitwebresponse.h"

#include <gsystem-local-alloc.h>

#include "websocket/websocket.h"

#include "reauthorize/reauthorize.h"

/* Some tunables that can be set from tests */
const gchar *cockpit_ws_session_program =
    PACKAGE_LIBEXEC_DIR "/cockpit-session";

const gchar *cockpit_ws_agent_program =
    PACKAGE_LIBEXEC_DIR "/cockpit-agent";

const gchar *cockpit_ws_known_hosts =
    PACKAGE_LOCALSTATE_DIR "/lib/cockpit/known_hosts";

const gchar *cockpit_ws_default_host_header =
    "0.0.0.0:0"; /* Must be something invalid */

gint cockpit_ws_specific_ssh_port = 0;

guint cockpit_ws_ping_interval = 5;

gint cockpit_ws_agent_timeout = 30;

/* ----------------------------------------------------------------------------
 * CockpitSession
 */

typedef struct
{
  gchar *host;
  gboolean primary;
  gboolean private;
  GHashTable *channels;
  CockpitTransport *transport;
  gboolean sent_eof;
  guint timeout;
  CockpitCreds *creds;
  GHashTable *checksums;
} CockpitSession;

typedef struct
{
  GHashTable *by_host;
  GHashTable *by_channel;
  GHashTable *by_transport;
} CockpitSessions;

/* Should only called as a hash table GDestroyNotify */
static void
cockpit_session_free (gpointer data)
{
  CockpitSession *session = data;

  g_debug ("%s: freeing session", session->host);

  if (session->timeout)
    g_source_remove (session->timeout);
  g_hash_table_unref (session->channels);
  g_object_unref (session->transport);
  g_hash_table_unref (session->checksums);
  cockpit_creds_unref (session->creds);
  g_free (session->host);
  g_free (session);
}

static void
cockpit_sessions_init (CockpitSessions *sessions)
{
  sessions->by_channel = g_hash_table_new (g_str_hash, g_str_equal);
  sessions->by_host = g_hash_table_new (g_str_hash, g_str_equal);

  /* This owns the session */
  sessions->by_transport = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, cockpit_session_free);
}

inline static CockpitSession *
cockpit_session_by_channel (CockpitSessions *sessions,
                            const gchar *channel)
{
  return g_hash_table_lookup (sessions->by_channel, channel);
}

inline static CockpitSession *
cockpit_session_by_transport (CockpitSessions *sessions,
                              CockpitTransport *transport)
{
  return g_hash_table_lookup (sessions->by_transport, transport);
}

inline static CockpitSession *
cockpit_session_by_host (CockpitSessions *sessions,
                         const gchar *host)
{
  return g_hash_table_lookup (sessions->by_host, host);
}

static gboolean
on_timeout_cleanup_session (gpointer user_data)
{
  CockpitSession *session = user_data;

  session->timeout = 0;
  if (g_hash_table_size (session->channels) == 0)
    {
      /*
       * This should cause the transport to immediately be closed
       * and on_session_closed() will react and remove it from
       * the main session lookup tables.
       */
      g_debug ("%s: session timed out without channels", session->host);
      cockpit_transport_close (session->transport, "timeout");
    }

  return FALSE;
}

static void
cockpit_session_remove_channel (CockpitSessions *sessions,
                                CockpitSession *session,
                                const gchar *channel)
{
  g_debug ("%s: remove channel %s for session", session->host, channel);

  g_hash_table_remove (sessions->by_channel, channel);
  g_hash_table_remove (session->channels, channel);

  if (g_hash_table_size (session->channels) == 0)
    {
      /*
       * Close sessions that are no longer in use after N seconds
       * of them being that way.
       */
      g_debug ("%s: removed last channel %s for session", session->host, channel);
      session->timeout = g_timeout_add_seconds (cockpit_ws_agent_timeout,
                                                on_timeout_cleanup_session, session);
    }
  else
    {
      g_debug ("%s: removed channel %s for session", session->host, channel);
    }
}

static void
cockpit_session_add_channel (CockpitSessions *sessions,
                             CockpitSession *session,
                             const gchar *channel)
{
  gchar *chan;

  chan = g_strdup (channel);
  g_hash_table_insert (sessions->by_channel, chan, session);
  g_hash_table_add (session->channels, chan);

  g_debug ("%s: added channel %s to session", session->host, channel);

  if (session->timeout)
    {
      g_source_remove (session->timeout);
      session->timeout = 0;
    }
}

static CockpitSession *
cockpit_session_track (CockpitSessions *sessions,
                       const gchar *host,
                       gboolean private,
                       CockpitCreds *creds,
                       CockpitTransport *transport)
{
  CockpitSession *session;

  g_debug ("%s: new session", host);

  session = g_new0 (CockpitSession, 1);
  session->channels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  session->transport = g_object_ref (transport);
  session->host = g_strdup (host);
  session->private = private;
  session->creds = cockpit_creds_ref (creds);
  session->checksums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (!private)
    g_hash_table_insert (sessions->by_host, session->host, session);

  /* This owns the session */
  g_hash_table_insert (sessions->by_transport, transport, session);

  return session;
}

static void
cockpit_session_destroy (CockpitSessions *sessions,
                         CockpitSession *session)
{
  GHashTableIter iter;
  const gchar *chan;

  g_debug ("%s: destroy %ssession", session->host, session->primary ? "primary " : "");

  g_hash_table_iter_init (&iter, session->channels);
  while (g_hash_table_iter_next (&iter, (gpointer *)&chan, NULL))
    g_hash_table_remove (sessions->by_channel, chan);
  g_hash_table_remove_all (session->channels);

  if (!session->private)
    g_hash_table_remove (sessions->by_host, session->host);

  /* This owns the session */
  g_hash_table_remove (sessions->by_transport, session->transport);
}

static void
cockpit_sessions_cleanup (CockpitSessions *sessions)
{
  g_hash_table_destroy (sessions->by_channel);
  g_hash_table_destroy (sessions->by_host);
  g_hash_table_destroy (sessions->by_transport);
}

/* ----------------------------------------------------------------------------
 * Web Socket Info
 */

typedef struct {
  gchar *scope;
  WebSocketConnection *connection;
} CockpitSocket;

typedef struct {
  GHashTable *by_scope;
  GHashTable *by_connection;
  guint next_scope_id;
} CockpitSockets;

static guint
channel_scope_hash (gconstpointer v)
{
  /* from g_str_hash */
  const signed char *p;
  guint32 h = 5381;
  for (p = v; *p != '\0' && *p != ':'; p++)
    h = (h << 5) + h + *p;
  return h;
}

static gboolean
channel_scope_equal (gconstpointer v1,
                     gconstpointer v2)
{
  const gchar *s1 = strchr (v1, ':');
  const gchar *s2 = strchr (v2, ':');
  gsize l1, l2;

  if (!s1 || !s2)
    return FALSE;

  l1 = s1 - (const gchar *)v1;
  l2 = s2 - (const gchar *)v2;
  return l1 != 0 && l2 != 0 && l1 == l2 &&
          memcmp (v1, v2, l1) == 0;
}

static void
cockpit_socket_free (gpointer data)
{
  CockpitSocket *socket = data;
  g_object_unref (socket->connection);
  g_free (socket->scope);
  g_free (socket);
}

static void
cockpit_sockets_init (CockpitSockets *sockets)
{
  sockets->next_scope_id = 1;

  sockets->by_scope = g_hash_table_new (channel_scope_hash, channel_scope_equal);

  /* This owns the socket */
  sockets->by_connection = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, cockpit_socket_free);
}

inline static gchar *
cockpit_socket_add_channel_scope (CockpitSocket *socket,
                                  const gchar *socket_channel)
{
  return g_strdup_printf ("%s%s", socket->scope, socket_channel);
}

static const gchar *
cockpit_socket_remove_channel_scope (const gchar *scoped_channel)
{
  const gchar *channel = strchr (scoped_channel, ':');
  if (channel != NULL)
    channel++;
  return channel;
}

inline static CockpitSocket *
cockpit_socket_lookup_by_connection (CockpitSockets *sockets,
                                     WebSocketConnection *connection)
{
  return g_hash_table_lookup (sockets->by_connection, connection);
}

inline static CockpitSocket *
cockpit_socket_lookup_by_channel (CockpitSockets *sockets,
                                  const gchar *scoped_channel)
{
  /* Only uses the scope part of scoped_channel */
  return g_hash_table_lookup (sockets->by_scope, scoped_channel);
}

static CockpitSocket *
cockpit_socket_track (CockpitSockets *sockets,
                      WebSocketConnection *connection)
{
  CockpitSocket *socket;

  socket = g_new0 (CockpitSocket, 1);
  socket->scope = g_strdup_printf ("%u:", sockets->next_scope_id++);
  socket->connection = g_object_ref (connection);

  g_debug ("%s new socket", socket->scope);

  g_hash_table_insert (sockets->by_scope, socket->scope, socket);

  /* This owns the session */
  g_hash_table_insert (sockets->by_connection, connection, socket);

  return socket;
}

static void
cockpit_socket_destroy (CockpitSockets *sockets,
                        CockpitSocket *socket)
{
  g_debug ("%s destroy socket", socket->scope);

  g_hash_table_remove (sockets->by_scope, socket->scope);

  /* This owns the session */
  g_hash_table_remove (sockets->by_connection, socket->connection);
}

static void
cockpit_sockets_cleanup (CockpitSockets *sockets)
{
  g_hash_table_destroy (sockets->by_connection);
  g_hash_table_destroy (sockets->by_scope);
}

/* ----------------------------------------------------------------------------
 * Web Socket Routing
 */

struct _CockpitWebService {
  GObject parent;

  CockpitCreds *creds;
  CockpitSockets sockets;
  CockpitSessions sessions;
  gboolean closing;
  GBytes *control_prefix;
  guint ping_timeout;
  gint callers;
  guint next_resource_id;
};

typedef struct {
  GObjectClass parent;
} CockpitWebServiceClass;

static guint sig_idling = 0;
static guint sig_destroy = 0;

G_DEFINE_TYPE (CockpitWebService, cockpit_web_service, G_TYPE_OBJECT);

static void
cockpit_web_service_dispose (GObject *object)
{
  CockpitWebService *self = COCKPIT_WEB_SERVICE (object);
  CockpitSocket *socket;
  CockpitSession *session;
  GHashTableIter iter;
  gboolean emit = FALSE;

  if (!self->closing)
    {
      g_debug ("web service closing");
      emit = TRUE;
    }
  self->closing = TRUE;

  g_hash_table_iter_init (&iter, self->sockets.by_scope);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&socket))
    {
      if (web_socket_connection_get_ready_state (socket->connection) < WEB_SOCKET_STATE_CLOSING)
        web_socket_connection_close (socket->connection, WEB_SOCKET_CLOSE_GOING_AWAY, "terminated");
    }

  g_hash_table_iter_init (&iter, self->sessions.by_transport);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&session))
    {
      if (!session->sent_eof)
        {
          session->sent_eof = TRUE;
          cockpit_transport_close (session->transport, NULL);
        }
    }

  if (emit)
    g_signal_emit (self, sig_destroy, 0);

  G_OBJECT_CLASS (cockpit_web_service_parent_class)->dispose (object);
}

static void
cockpit_web_service_finalize (GObject *object)
{
  CockpitWebService *self = COCKPIT_WEB_SERVICE (object);

  cockpit_sessions_cleanup (&self->sessions);
  cockpit_sockets_cleanup (&self->sockets);
  g_bytes_unref (self->control_prefix);
  cockpit_creds_unref (self->creds);
  if (self->ping_timeout)
    g_source_remove (self->ping_timeout);

  G_OBJECT_CLASS (cockpit_web_service_parent_class)->finalize (object);
}

static GBytes *
build_control (const gchar *name,
               ...) G_GNUC_NULL_TERMINATED;

static GBytes *
build_control (const gchar *name,
               ...)
{
  JsonObject *object;
  GBytes *message;
  const gchar *value;
  va_list va;

  object = json_object_new ();

  va_start (va, name);
  while (name)
    {
      value = va_arg (va, const gchar *);
      if (value)
        json_object_set_string_member (object, name, value);
      name = va_arg (va, const gchar *);
    }
  va_end (va);

  message = cockpit_json_write_bytes (object);
  json_object_unref (object);
  return message;
}

static void
caller_begin (CockpitWebService *self)
{
  g_object_ref (self);
  self->callers++;
}

static void
caller_end (CockpitWebService *self)
{
  g_return_if_fail (self->callers > 0);
  self->callers--;
  if (self->callers == 0)
    g_signal_emit (self, sig_idling, 0);
  g_object_unref (self);
}

static void
outbound_protocol_error (CockpitWebService *self,
                         CockpitTransport *transport)
{
  cockpit_transport_close (transport, "protocol-error");
}

static void
process_resources (JsonObject *resources,
                   const gchar *logname,
                   GHashTable *checksums)
{
  const gchar *checksum;
  JsonObject *details;
  GList *modules;
  GList *l;

  g_hash_table_remove_all (checksums);

  /* Build a table mapping checksum to module for resources on this session */
  modules = json_object_get_members (resources);
  for (l = modules; l != NULL; l = g_list_next (l))
    {
      details = json_object_get_object_member (resources, l->data);
      if (details)
        {
          if (cockpit_json_get_string (details, "checksum", NULL, &checksum) && checksum)
            {
              g_debug ("%s: module %s has checksum %s", logname, (gchar *)l->data, checksum);
              g_hash_table_insert (checksums, g_strdup (checksum), g_strdup (l->data));
            }
        }
    }
  g_list_free (modules);
}

static gboolean
process_close (CockpitWebService *self,
               CockpitSession *session,
               const gchar *channel,
               JsonObject *options)
{
  JsonNode *node;

  node = json_object_get_member (options, "resources");
  if (node != NULL && json_node_get_node_type (node) == JSON_NODE_OBJECT)
    process_resources (json_node_get_object (node), session->host, session->checksums);

  cockpit_session_remove_channel (&self->sessions, session, channel);
  return TRUE;
}

static gboolean
process_authorize (CockpitWebService *self,
                   CockpitSession *session,
                   JsonObject *options)
{
  const gchar *cookie = NULL;
  GBytes *payload;
  const gchar *host;
  char *user = NULL;
  char *type = NULL;
  char *response = NULL;
  const gchar *challenge;
  const gchar *password;
  gboolean ret = FALSE;
  int rc;

  host = session->host;

  if (!cockpit_json_get_string (options, "challenge", NULL, &challenge) ||
      !cockpit_json_get_string (options, "cookie", NULL, &cookie) ||
      challenge == NULL ||
      reauthorize_type (challenge, &type) < 0 ||
      reauthorize_user (challenge, &user) < 0)
    {
      g_warning ("%s: received invalid authorize command", host);
      goto out;
    }

  if (!g_str_equal (cockpit_creds_get_user (session->creds), user))
    {
      g_warning ("%s: received authorize command for wrong user: %s", host, user);
    }
  else if (g_str_equal (type, "crypt1"))
    {
      password = cockpit_creds_get_password (session->creds);
      if (!password)
        {
          g_debug ("%s: received authorize crypt1 challenge, but no password to reauthenticate", host);
        }
      else
        {
          rc = reauthorize_crypt1 (challenge, password, &response);
          if (rc < 0)
            g_warning ("%s: failed to reauthorize crypt1 challenge", host);
        }
    }

  /*
   * TODO: So the missing piece is that something needs to unauthorize
   * the user. This needs to be coordinated with the web service.
   *
   * For now we assume that since this is an admin tool, as long as the
   * user has it open, he/she is authorized.
   */

  if (!session->sent_eof)
    {
      payload = build_control ("command", "authorize",
                               "cookie", cookie,
                               "response", response ? response : "",
                               NULL);
      cockpit_transport_send (session->transport, NULL, payload);
      g_bytes_unref (payload);
    }
  ret = TRUE;

out:
  free (user);
  free (type);
  free (response);
  return ret;
}

static gboolean
on_session_control (CockpitTransport *transport,
                    const gchar *command,
                    const gchar *channel,
                    JsonObject *options,
                    gpointer user_data)
{
  CockpitWebService *self = user_data;
  CockpitSession *session = NULL;
  CockpitSocket *socket = NULL;
  const gchar *socket_channel;
  gboolean valid = FALSE;
  GBytes *payload;

  if (!channel)
    {
      session = cockpit_session_by_transport (&self->sessions, transport);
      if (!session)
        {
          g_critical ("received control command for transport that isn't present");
          valid = FALSE;
        }
      else if (g_strcmp0 (command, "authorize") == 0)
        {
          valid = process_authorize (self, session, options);
        }
      else if (g_strcmp0 (command, "ping") == 0)
        {
          valid = TRUE;
        }
      else
        {
          g_warning ("received a %s control command without a channel", command);
          valid = FALSE;
        }
    }
  else
    {
      /*
       * To prevent one host from messing with another, outbound commands
       * must have a channel, and it must match one of the channels opened
       * to that particular session.
       */
      session = cockpit_session_by_channel (&self->sessions, channel);
      if (!session)
        {
          g_warning ("channel %s does not exist", channel);
          valid = FALSE;
        }
      else if (session->transport != transport)
        {
          g_warning ("received a command with wrong channel %s from session", channel);
          valid = FALSE;
        }
      else if (g_strcmp0 (command, "close") == 0)
        {
          valid = process_close (self, session, channel, options);
        }
      else
        {
          g_debug ("forwarding a '%s' control command", command);
          valid = TRUE; /* forward other messages */
        }

      if (valid)
        {
          /*
           * Forward this message to the right websocket, removing the web socket specific
           * channel scope as we do so and modifying the message to reflect that.
           */
          socket = cockpit_socket_lookup_by_channel (&self->sockets, channel);
          if (socket && web_socket_connection_get_ready_state (socket->connection) == WEB_SOCKET_STATE_OPEN)
            {
              socket_channel = cockpit_socket_remove_channel_scope (channel);
              json_object_set_string_member (options, "channel", socket_channel);
              payload = cockpit_json_write_bytes (options);
              web_socket_connection_send (socket->connection, WEB_SOCKET_DATA_TEXT,
                                          self->control_prefix, payload);
              g_bytes_unref (payload);
            }
        }
    }

  if (!valid)
    {
      outbound_protocol_error (self, transport);
    }

  return TRUE; /* handled */
}

static gboolean
on_session_recv (CockpitTransport *transport,
                 const gchar *channel,
                 GBytes *payload,
                 gpointer user_data)
{
  CockpitWebService *self = user_data;
  CockpitSession *session;
  CockpitSocket *socket;
  gchar *string;
  GBytes *prefix;

  if (!channel)
    return FALSE;

  session = cockpit_session_by_channel (&self->sessions, channel);
  if (session == NULL)
    {
      g_warning ("received message with unknown channel %s from session", channel);
      outbound_protocol_error (self, transport);
      return FALSE;
    }
  else if (session->transport != transport)
    {
      g_warning ("received message with wrong channel %s from session", channel);
      outbound_protocol_error (self, transport);
      return FALSE;
    }

  /* We update the channel and remove the web socket specific channel scope here. */
  socket = cockpit_socket_lookup_by_channel (&self->sockets, channel);
  if (socket && web_socket_connection_get_ready_state (socket->connection) == WEB_SOCKET_STATE_OPEN)
    {
      string = g_strdup_printf ("%s\n", cockpit_socket_remove_channel_scope (channel));
      prefix = g_bytes_new_take (string, strlen (string));
      web_socket_connection_send (socket->connection, WEB_SOCKET_DATA_TEXT, prefix, payload);
      g_bytes_unref (prefix);
    }

  return FALSE;
}

static void
on_session_closed (CockpitTransport *transport,
                   const gchar *problem,
                   gpointer user_data)
{
  CockpitWebService *self = user_data;
  const gchar *channel = NULL;
  CockpitSession *session;
  CockpitSshTransport *ssh;
  GHashTableIter iter;
  CockpitSocket *socket;
  const gchar *key = NULL;
  const gchar *fp = NULL;
  GBytes *payload;
  gboolean primary;

  session = cockpit_session_by_transport (&self->sessions, transport);
  if (session != NULL)
    {
      if (g_strcmp0 (problem, "unknown-hostkey") == 0 &&
          COCKPIT_IS_SSH_TRANSPORT (transport))
        {
          ssh = COCKPIT_SSH_TRANSPORT (transport);
          key = cockpit_ssh_transport_get_host_key (ssh);
          fp = cockpit_ssh_transport_get_host_fingerprint (ssh);
        }

      g_hash_table_iter_init (&iter, session->channels);
      while (g_hash_table_iter_next (&iter, (gpointer *)&channel, NULL))
        {
          /* Note that we use the web socket channel here, removing the channel scope */
          socket = cockpit_socket_lookup_by_channel (&self->sockets, channel);
          if (socket)
            {
              if (web_socket_connection_get_ready_state (socket->connection) == WEB_SOCKET_STATE_OPEN)
                {
                  payload = build_control ("command", "close",
                                           "channel", cockpit_socket_remove_channel_scope (channel),
                                           "reason", problem,
                                           "host-key", key,
                                           "host-fingerprint", fp,
                                           NULL);
                  web_socket_connection_send (socket->connection, WEB_SOCKET_DATA_TEXT,
                                              self->control_prefix, payload);
                  g_bytes_unref (payload);
                }
            }
        }

      g_signal_handlers_disconnect_by_func (transport, on_session_control, self);
      g_signal_handlers_disconnect_by_func (transport, on_session_recv, self);
      g_signal_handlers_disconnect_by_func (transport, on_session_closed, self);

      primary = session->primary;
      cockpit_session_destroy (&self->sessions, session);

      /* If this is the primary session, log the user out */
      if (primary)
        g_object_run_dispose (G_OBJECT (self));
    }
}

static CockpitSession *
lookup_or_open_session_for_host (CockpitWebService *self,
                                 const gchar *host,
                                 const gchar *host_key,
                                 CockpitCreds *creds,
                                 gboolean private)
{
  CockpitSession *session = NULL;
  CockpitTransport *transport;

  if (!private)
    session = cockpit_session_by_host (&self->sessions, host);
  if (!session)
    {
      /* Used during testing */
      if (g_strcmp0 (host, "localhost") == 0)
        {
          if (cockpit_ws_specific_ssh_port != 0)
            host = "127.0.0.1";
        }

      if (host == NULL || g_strcmp0 (host, "") == 0)
        host = "localhost";

      transport = g_object_new (COCKPIT_TYPE_SSH_TRANSPORT,
                                "host", host,
                                "port", cockpit_ws_specific_ssh_port,
                                "command", cockpit_ws_agent_program,
                                "creds", creds,
                                "known-hosts", cockpit_ws_known_hosts,
                                "host-key", host_key,
                                NULL);

      g_signal_connect_after (transport, "control", G_CALLBACK (on_session_control), self);
      g_signal_connect_after (transport, "recv", G_CALLBACK (on_session_recv), self);
      g_signal_connect_after (transport, "closed", G_CALLBACK (on_session_closed), self);
      session = cockpit_session_track (&self->sessions, host, private, creds, transport);
      g_object_unref (transport);
    }

  return session;
}

static gboolean
process_open (CockpitWebService *self,
              const gchar *channel,
              JsonObject *options)
{
  CockpitSession *session = NULL;
  CockpitCreds *creds;
  const gchar *specific_user;
  const gchar *password;
  const gchar *host;
  const gchar *host_key;
  gboolean private;

  if (self->closing)
    {
      g_debug ("Ignoring open command while web socket is closing");
      return TRUE;
    }

  if (cockpit_session_by_channel (&self->sessions, channel))
    {
      g_warning ("cannot open a channel %s with the same id as another channel", channel);
      return FALSE;
    }

  if (!cockpit_json_get_string (options, "host", "localhost", &host))
    host = "localhost";

  /*
   * Some sessions shouldn't be shared by multiple channels, such as those that
   * explicitly specify a host-key or specific user.
   *
   * In the future we'd like to get away from having these sorts of channels, but
   * for now we force them to have their own session, started with those specific
   * arguments.
   *
   * This means the session doesn't show up in the by_host table.
   */
  private = FALSE;

  if (cockpit_json_get_string (options, "user", NULL, &specific_user) && specific_user && !g_str_equal (specific_user, ""))
    {
      if (!cockpit_json_get_string (options, "password", NULL, &password))
        password = NULL;
      creds = cockpit_creds_new (specific_user,
                                 COCKPIT_CRED_PASSWORD, password,
                                 COCKPIT_CRED_RHOST, cockpit_creds_get_rhost (self->creds),
                                 NULL);

      /* A private session for this host */
      private = TRUE;
    }
  else
    {
      creds = cockpit_creds_ref (self->creds);
    }

  if (!cockpit_json_get_string (options, "host-key", NULL, &host_key))
    host_key = NULL;
  if (host_key)
    private = TRUE;

  session = lookup_or_open_session_for_host (self, host, host_key, creds, private);

  cockpit_creds_unref (creds);
  cockpit_session_add_channel (&self->sessions, session, channel);
  return TRUE;
}

static gboolean
process_logout (CockpitWebService *self,
                JsonObject *options)
{
  gboolean disconnect;

  if (!cockpit_json_get_bool (options, "disconnect", FALSE, &disconnect))
    {
      g_warning ("received 'logout' command with invalid 'disconnect' field");
      return FALSE;
    }

  /* Makes the credentials unusable */
  cockpit_creds_poison (self->creds);

  /* Destroys our web service, disconnects everything */
  if (disconnect)
    {
      g_info ("Logging out user %s from %s",
              cockpit_creds_get_user (self->creds),
              cockpit_creds_get_rhost (self->creds));
      g_object_run_dispose (G_OBJECT (self));
    }
  else
    {
      g_info ("Deauthorizing user %s",
              cockpit_creds_get_rhost (self->creds));
    }

  return TRUE;
}

static void
inbound_protocol_error (CockpitWebService *self,
                        WebSocketConnection *connection)
{
  GBytes *payload;

  if (web_socket_connection_get_ready_state (connection) == WEB_SOCKET_STATE_OPEN)
    {
      payload = build_control ("command", "close", "reason", "protocol-error", NULL);
      web_socket_connection_send (connection, WEB_SOCKET_DATA_TEXT, self->control_prefix, payload);
      g_bytes_unref (payload);
      web_socket_connection_close (connection, WEB_SOCKET_CLOSE_SERVER_ERROR, "protocol-error");
    }
}

static void
dispatch_inbound_command (CockpitWebService *self,
                          CockpitSocket *socket,
                          GBytes *payload)
{
  const gchar *command;
  const gchar *channel;
  gchar *agent_channel = NULL;
  JsonObject *options = NULL;
  gboolean valid = FALSE;
  gboolean forward = TRUE;
  CockpitSession *session;
  GHashTableIter iter;
  GBytes *bytes;

  valid = cockpit_transport_parse_command (payload, &command, &channel, &options);
  if (!valid)
    goto out;

  /* Add scope to the channel before sending it to the agent */
  if (channel)
    {
      agent_channel = cockpit_socket_add_channel_scope (socket, channel);
      channel = agent_channel;
    }

  if (g_strcmp0 (command, "open") == 0)
    valid = process_open (self, channel, options);
  else if (g_strcmp0 (command, "logout") == 0)
    {
      valid = process_logout (self, options);
      forward = FALSE;
    }
  else if (g_strcmp0 (command, "close") == 0)
    valid = TRUE;
  else if (g_strcmp0 (command, "ping") == 0)
    {
      valid = TRUE;
      forward = FALSE;
    }
  else
    valid = TRUE; /* forward other messages */

  if (!valid)
    goto out;

  if (forward && channel == 0)
    {
      /* Control messages without a channel get sent to all sessions */
      g_hash_table_iter_init (&iter, self->sessions.by_transport);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&session))
        {
          if (!session->sent_eof)
            cockpit_transport_send (session->transport, NULL, payload);
        }
    }
  else if (forward)
    {
      /* Control messages with a channel get forward to that session */
      session = cockpit_session_by_channel (&self->sessions, channel);
      if (session)
        {
          /* We have to update the channel with the scope */
          if (!session->sent_eof)
            {
              json_object_set_string_member (options, "channel", channel);
              bytes = cockpit_json_write_bytes (options);
              cockpit_transport_send (session->transport, NULL, bytes);
              g_bytes_unref (bytes);
            }
        }
      else
        g_debug ("dropping control message with unknown channel %s", channel);
    }

out:
  if (!valid)
    inbound_protocol_error (self, socket->connection);
  if (options)
    json_object_unref (options);
  g_free (agent_channel);
}

static void
on_web_socket_message (WebSocketConnection *connection,
                       WebSocketDataType type,
                       GBytes *message,
                       CockpitWebService *self)
{
  CockpitSession *session;
  CockpitSocket *socket;
  gchar *socket_channel;
  GBytes *payload;
  gchar *channel;

  socket = cockpit_socket_lookup_by_connection (&self->sockets, connection);
  g_return_if_fail (socket != NULL);

  payload = cockpit_transport_parse_frame (message, &socket_channel);
  if (!payload)
    return;

  /* A control channel command */
  if (!socket_channel)
    {
      dispatch_inbound_command (self, socket, payload);
    }

  /* An actual payload message */
  else if (!self->closing)
    {
      /* Qualify the received channel with a scope for the web socket */
      channel = cockpit_socket_add_channel_scope (socket, socket_channel);

      session = cockpit_session_by_channel (&self->sessions, channel);
      if (session)
        {
          if (!session->sent_eof)
            cockpit_transport_send (session->transport, channel, payload);
        }
      else
        {
          g_debug ("received message for unknown channel %s", channel);
        }

      g_free (channel);
    }

  g_free (socket_channel);
  g_bytes_unref (payload);
}

static void
on_web_socket_open (WebSocketConnection *connection,
                    CockpitWebService *self)
{
  g_info ("New connection from %s for %s",
          cockpit_creds_get_rhost (self->creds),
          cockpit_creds_get_user (self->creds));
  g_signal_connect (connection, "message",
                    G_CALLBACK (on_web_socket_message), self);
}

static void
on_web_socket_error (WebSocketConnection *connection,
                     GError *error,
                     gpointer unused)
{
  if (g_error_matches (error, G_TLS_ERROR, G_TLS_ERROR_EOF))
    g_debug ("web socket error: %s", error->message);
  else
    g_message ("%s", error->message);
}

static gboolean
on_web_socket_closing (WebSocketConnection *connection,
                       CockpitWebService *self)
{
  CockpitSession *session;
  CockpitSocket *socket;
  GHashTable *snapshot;
  GHashTableIter iter;
  const gchar *channel;
  GBytes *payload;

  g_debug ("web socket closing");

  /* Close any channels that were opened by this web socket */
  snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_iter_init (&iter, self->sessions.by_channel);
  while (g_hash_table_iter_next (&iter, (gpointer *)&channel, (gpointer *)&session))
    {
      socket = cockpit_socket_lookup_by_channel (&self->sockets, channel);
      if (socket && socket->connection == connection)
        g_hash_table_insert (snapshot, g_strdup (channel), session);
    }

  g_hash_table_iter_init (&iter, snapshot);
  while (g_hash_table_iter_next (&iter, (gpointer *)&channel, (gpointer *)&session))
    {
      payload = build_control ("command", "close",
                               "channel", channel,
                               "reason", "disconnected",
                               NULL);
      cockpit_transport_send (session->transport, NULL, payload);
      g_bytes_unref (payload);
    }
  g_hash_table_destroy (snapshot);

  return TRUE;
}

static void
on_web_socket_close (WebSocketConnection *connection,
                     CockpitWebService *self)
{
  CockpitSocket *socket;

  g_info ("WebSocket from %s for %s closed",
          cockpit_creds_get_rhost (self->creds),
          cockpit_creds_get_user (self->creds));

  g_signal_handlers_disconnect_by_func (connection, on_web_socket_open, self);
  g_signal_handlers_disconnect_by_func (connection, on_web_socket_closing, self);
  g_signal_handlers_disconnect_by_func (connection, on_web_socket_close, self);
  g_signal_handlers_disconnect_by_func (connection, on_web_socket_error, self);

  socket = cockpit_socket_lookup_by_connection (&self->sockets, connection);
  g_return_if_fail (socket != NULL);

  cockpit_socket_destroy (&self->sockets, socket);

  caller_end (self);
}

static gboolean
on_ping_time (gpointer user_data)
{
  CockpitWebService *self = user_data;
  WebSocketConnection *connection;
  GHashTableIter iter;
  GBytes *payload;

  payload = build_control ("command", "ping", NULL);

  g_hash_table_iter_init (&iter, self->sockets.by_connection);
  while (g_hash_table_iter_next (&iter, (gpointer *)&connection, NULL))
    {
      if (web_socket_connection_get_ready_state (connection) == WEB_SOCKET_STATE_OPEN)
        web_socket_connection_send (connection, WEB_SOCKET_DATA_TEXT, self->control_prefix, payload);
    }

  g_bytes_unref (payload);
  return TRUE;
}

static void
cockpit_web_service_init (CockpitWebService *self)
{
  self->control_prefix = g_bytes_new_static ("\n", 1);
  cockpit_sessions_init (&self->sessions);
  cockpit_sockets_init (&self->sockets);
  self->ping_timeout = g_timeout_add_seconds (cockpit_ws_ping_interval, on_ping_time, self);
}

static void
cockpit_web_service_class_init (CockpitWebServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = cockpit_web_service_dispose;
  object_class->finalize = cockpit_web_service_finalize;

  sig_idling = g_signal_new ("idling", COCKPIT_TYPE_WEB_SERVICE,
                             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                             G_TYPE_NONE, 0);
  sig_destroy = g_signal_new ("destroy", COCKPIT_TYPE_WEB_SERVICE,
                              G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                              G_TYPE_NONE, 0);
}

/**
 * cockpit_web_service_new:
 * @creds: credentials of user
 * @session: optionally an open cockpit-session master process, or NULL
 *
 * Creates a new web service to serve web sockets and connect
 * to agents for the given user.
 *
 * Returns: (transfer full): the new web service
 */
CockpitWebService *
cockpit_web_service_new (CockpitCreds *creds,
                         CockpitPipe *pipe)
{
  CockpitWebService *self;
  CockpitTransport *transport;
  CockpitSession *session;

  g_return_val_if_fail (creds != NULL, NULL);

  self = g_object_new (COCKPIT_TYPE_WEB_SERVICE, NULL);
  self->creds = cockpit_creds_ref (creds);

  if (pipe)
    {
      /* Any failures happen asyncronously */
      transport = cockpit_pipe_transport_new (pipe);
      g_signal_connect_after (transport, "control", G_CALLBACK (on_session_control), self);
      g_signal_connect_after (transport, "recv", G_CALLBACK (on_session_recv), self);
      g_signal_connect_after (transport, "closed", G_CALLBACK (on_session_closed), self);
      session = cockpit_session_track (&self->sessions, "localhost", FALSE, creds, transport);
      session->primary = TRUE;
      g_object_unref (transport);
    }

  return self;
}

static WebSocketConnection *
create_web_socket_server_for_stream (GIOStream *io_stream,
                                     GHashTable *headers,
                                     GByteArray *input_buffer)
{
  const gchar *protocols[] = { "cockpit1", NULL };
  WebSocketConnection *connection;
  const gchar *host = NULL;
  gboolean secure;
  gchar *origin;
  gchar *url;

  if (headers)
    host = g_hash_table_lookup (headers, "Host");
  if (!host)
    host = cockpit_ws_default_host_header;

  secure = G_IS_TLS_CONNECTION (io_stream);

  url = g_strdup_printf ("%s://%s/socket", secure ? "wss" : "ws",
                         host ? host : "localhost");
  origin = g_strdup_printf ("%s://%s", secure ? "https" : "http", host);

  connection = web_socket_server_new_for_stream (url, origin, protocols,
                                                 io_stream, headers,
                                                 input_buffer);
  g_free (origin);
  g_free (url);

  return connection;
}

/**
 * cockpit_web_service_socket:
 * @io_stream: the stream to talk on
 * @headers: optional headers already parsed
 * @input_buffer: optional bytes already parsed after headers
 * @auth: authentication object
 * @creds: credentials of user or NULL for failed auth
 *
 * Serves the WebSocket on the given web service. Holds an extra
 * reference to the web service until the socket is closed.
 */
void
cockpit_web_service_socket (CockpitWebService *self,
                            GIOStream *io_stream,
                            GHashTable *headers,
                            GByteArray *input_buffer)
{
  WebSocketConnection *connection;

  connection = create_web_socket_server_for_stream (io_stream, headers, input_buffer);

  g_signal_connect (connection, "open", G_CALLBACK (on_web_socket_open), self);
  g_signal_connect (connection, "closing", G_CALLBACK (on_web_socket_closing), self);
  g_signal_connect (connection, "close", G_CALLBACK (on_web_socket_close), self);
  g_signal_connect (connection, "error", G_CALLBACK (on_web_socket_error), NULL);

  cockpit_socket_track (&self->sockets, connection);
  g_object_unref (connection);

  caller_begin (self);
}

/**
 * cockpit_web_service_get_creds:
 * @self: the service
 *
 * Returns: (transfer none): the credentials for which this service was opened.
 */
CockpitCreds *
cockpit_web_service_get_creds (CockpitWebService *self)
{
  g_return_val_if_fail (COCKPIT_IS_WEB_SERVICE (self), NULL);
  return self->creds;
}

/**
 * cockpit_web_service_disconnect:
 * @self: the service
 *
 * Close all sessions and sockets that are running in this web
 * service.
 */
void
cockpit_web_service_disconnect (CockpitWebService *self)
{
  g_object_run_dispose (G_OBJECT (self));
}

static void
on_web_socket_noauth (WebSocketConnection *connection,
                      gpointer unused)
{
  GBytes *payload;
  GBytes *prefix;

  g_debug ("closing unauthenticated web socket");

  payload = build_control ("command", "close", "reason", "no-session", NULL);
  prefix = g_bytes_new_static ("\n", 1);

  web_socket_connection_send (connection, WEB_SOCKET_DATA_TEXT, prefix, payload);
  web_socket_connection_close (connection, WEB_SOCKET_CLOSE_GOING_AWAY, "no-session");

  g_bytes_unref (prefix);
  g_bytes_unref (payload);
}

void
cockpit_web_service_noauth (GIOStream *io_stream,
                            GHashTable *headers,
                            GByteArray *input_buffer)
{
  WebSocketConnection *connection;

  connection = create_web_socket_server_for_stream (io_stream, headers, input_buffer);

  g_signal_connect (connection, "open", G_CALLBACK (on_web_socket_noauth), NULL);
  g_signal_connect (connection, "error", G_CALLBACK (on_web_socket_error), NULL);

  /* Unreferences connection when it closes */
  g_signal_connect (connection, "close", G_CALLBACK (g_object_unref), NULL);
}

gboolean
cockpit_web_service_get_idling (CockpitWebService *self)
{
  g_return_val_if_fail (COCKPIT_IS_WEB_SERVICE (self), TRUE);
  return (self->callers == 0);
}

typedef struct {
  const gchar *logname;
  CockpitWebResponse *response;
  CockpitTransport *transport;
  gchar *channel;
  gulong recv_sig;
  gulong closed_sig;
  gulong control_sig;
  gboolean cache_forever;
} ResourceResponse;

static void
resource_response_done (ResourceResponse *rr,
                        const gchar *problem)
{
  CockpitWebResponding state;

  /* Ensure no more signals arrive about our response */
  g_signal_handler_disconnect (rr->transport, rr->recv_sig);
  g_signal_handler_disconnect (rr->transport, rr->closed_sig);
  g_signal_handler_disconnect (rr->transport, rr->control_sig);

  /* The web response should not yet be complete */
  state = cockpit_web_response_get_state (rr->response);
  g_return_if_fail (state < COCKPIT_WEB_RESPONSE_COMPLETE);

  if (problem == NULL)
    {
      g_debug ("%s: completed serving resource", rr->logname);
      if (state == COCKPIT_WEB_RESPONSE_READY)
        cockpit_web_response_headers (rr->response, 200, "OK", 0, NULL);
      cockpit_web_response_complete (rr->response);
    }
  else if (state == COCKPIT_WEB_RESPONSE_READY)
    {
      if (g_str_equal (problem, "not-found"))
        {
          g_debug ("%s: resource not found", rr->logname);
          cockpit_web_response_error (rr->response, 404, NULL, NULL);
        }
      else
        {
          g_message ("%s: failed to retrieve resource: %s", rr->logname, problem);
          cockpit_web_response_error (rr->response, 500, NULL, NULL);
        }
    }
  else
    {
      g_message ("%s: failure while serving resource: %s", rr->logname, problem);
      cockpit_web_response_abort (rr->response);
    }

  g_object_unref (rr->response);
  g_object_unref (rr->transport);
  g_free (rr->channel);
  g_free (rr);
}

static gboolean
on_resource_recv (CockpitTransport *transport,
                  const gchar *channel,
                  GBytes *payload,
                  gpointer user_data)
{
  ResourceResponse *rr = user_data;
  const gchar *cache_control;

  if (g_strcmp0 (channel, rr->channel) != 0)
    return FALSE;

  if (cockpit_web_response_get_state (rr->response) == COCKPIT_WEB_RESPONSE_READY)
    {
      cache_control = rr->cache_forever ? "max-age=31556926, public" : NULL;
      cockpit_web_response_headers (rr->response, 200, "OK", -1,
                                    "Cache-Control", cache_control,
                                    NULL);
    }

  cockpit_web_response_queue (rr->response, payload);
  return TRUE;
}

static gboolean
on_resource_control (CockpitTransport *transport,
                     const gchar *command,
                     const gchar *channel,
                     JsonObject *options,
                     gpointer user_data)
{
  ResourceResponse *rr = user_data;
  const gchar *problem = NULL;

  if (g_strcmp0 (channel, rr->channel) != 0)
    return FALSE; /* not handled */

  if (!g_str_equal (command, "close"))
    {
      g_message ("%s: received unknown command on resource channel: %s",
                 rr->logname, command);
      return TRUE; /* but handled */
    }

  if (!cockpit_json_get_string (options, "reason", NULL, &problem))
    {
      g_message ("%s: received close command with invalid reason", rr->logname);
      problem = "unknown";
    }

  if (g_strcmp0 (problem, "") == 0)
    problem = NULL;

  resource_response_done (rr, problem);
  return TRUE; /* handled */
}

static void
on_resource_closed (CockpitTransport *transport,
                    const gchar *problem,
                    gpointer user_data)
{
  ResourceResponse *rr = user_data;

  g_debug ("%s: transport closed while serving resource: %s", rr->logname, problem);

  if (problem == NULL || g_strcmp0 (problem, "") == 0)
    problem = "terminated";

  resource_response_done (rr, problem);
}

static ResourceResponse *
resource_response_new (CockpitWebService *self,
                       CockpitSession *session,
                       CockpitWebResponse *response)
{
  ResourceResponse *rr;

  rr = g_new0 (ResourceResponse, 1);
  rr->response = g_object_ref (response);
  rr->transport = g_object_ref (session->transport);
  rr->channel = g_strdup_printf ("0:%d", self->next_resource_id++);
  rr->logname = cockpit_web_response_get_path (response);

  rr->recv_sig = g_signal_connect (rr->transport, "recv", G_CALLBACK (on_resource_recv), rr);
  rr->closed_sig = g_signal_connect (rr->transport, "closed", G_CALLBACK (on_resource_closed), rr);
  rr->control_sig = g_signal_connect (rr->transport, "control", G_CALLBACK (on_resource_control), rr);

  return rr;
}

static gboolean
resource_respond_normal (CockpitWebService *self,
                         CockpitWebResponse *response,
                         const gchar *remaining_path)
{
  ResourceResponse *rr;
  CockpitSession *session;
  gboolean ret = FALSE;
  GBytes *command;
  gchar **parts;

  parts = g_strsplit (remaining_path, "/", 3);
  if (!parts[0] || !parts[1] || !parts[2])
    {
      g_debug ("invalid resource path: %s", remaining_path);
      goto out;
    }

  session = lookup_or_open_session_for_host (self, parts[0], NULL, self->creds, FALSE);
  rr = resource_response_new (self, session, response);

  command = build_control ("command", "open",
                           "channel", rr->channel,
                           "payload", "resource1",
                           "module", parts[1],
                           "path", parts[2],
                           NULL);

  cockpit_transport_send (rr->transport, NULL, command);
  g_bytes_unref (command);
  ret = TRUE;

out:
  g_strfreev (parts);
  return ret;
}


static gboolean
resource_respond_checksum (CockpitWebService *self,
                           CockpitWebResponse *response,
                           const gchar *remaining_path)
{
  ResourceResponse *rr;
  CockpitSession *session;
  CockpitSession *found = NULL;
  const gchar *module = NULL;
  gboolean ret = FALSE;
  GHashTableIter iter;
  GBytes *command;
  gchar **parts;

  parts = g_strsplit (remaining_path, "/", 2);
  if (!parts[0] || !parts[1])
    {
      g_debug ("invalid checksum path: %s", remaining_path);
      goto out;
    }

  g_hash_table_iter_init (&iter, self->sessions.by_transport);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&session))
    {
      if (session->checksums)
        {
          module = g_hash_table_lookup (session->checksums, parts[0]);
          if (module != NULL)
            {
              found = session;
              break;
            }
        }
    }

  if (!found)
    {
      g_debug ("no session found for resource checksum: %s", parts[0]);
      goto out;
    }

  rr = resource_response_new (self, session, response);
  rr->cache_forever = TRUE;

  command = build_control ("command", "open",
                           "channel", rr->channel,
                           "payload", "resource1",
                           "module", module,
                           "path", parts[1],
                           NULL);

  cockpit_transport_send (rr->transport, NULL, command);
  g_bytes_unref (command);
  ret = TRUE;

out:
  g_strfreev (parts);
  return ret;
}

void
cockpit_web_service_resource (CockpitWebService *self,
                              CockpitWebResponse *response)
{
  gboolean handled = FALSE;
  const gchar *path;

  path = cockpit_web_response_get_path (response);

  if (g_str_has_prefix (path, "/res/"))
    handled = resource_respond_normal (self, response, path + 5);
  if (g_str_has_prefix (path, "/cache/"))
    handled = resource_respond_checksum (self, response, path + 7);

  if (!handled)
    cockpit_web_response_error (response, 404, NULL, NULL);
}

typedef struct {
  gchar *logname;
  gchar *channel;
  CockpitTransport *transport;
  gulong closed_sig;
  gulong control_sig;
  JsonObject *modules;
  GHashTable *checksums;
} ListModules;

static void
list_modules_free (gpointer data)
{
  ListModules *lm = data;
  g_free (lm->logname);
  g_free (lm->channel);

  g_signal_handler_disconnect (lm->transport, lm->closed_sig);
  g_signal_handler_disconnect (lm->transport, lm->control_sig);
  g_hash_table_unref (lm->checksums);
  g_object_unref (lm->transport);
  if (lm->modules)
    json_object_unref (lm->modules);
  g_free (lm);
}

static gboolean
on_listing_control (CockpitTransport *transport,
                    const gchar *command,
                    const gchar *channel,
                    JsonObject *options,
                    gpointer user_data)
{
  GSimpleAsyncResult *async = user_data;
  const gchar *problem = NULL;
  ListModules *lm;

  lm = g_simple_async_result_get_op_res_gpointer (async);

  if (g_strcmp0 (channel, lm->channel) != 0)
    return FALSE; /* not handled */

  if (!g_str_equal (command, "close"))
    {
      g_message ("%s: received unknown command on resource channel: %s",
                 lm->logname, command);
      return TRUE; /* but handled */
    }

  if (!cockpit_json_get_string (options, "reason", NULL, &problem))
    {
      g_message ("%s: received close command with invalid reason", lm->logname);
    }
  if (problem && problem[0])
    {
      g_message ("%s: couldn't list cockpit modules: %s", lm->logname, problem);
    }
  else
    {
      lm->modules = json_object_get_object_member (options, "resources");
      if (lm->modules)
        {
          json_object_ref (lm->modules);
          process_resources (lm->modules, lm->logname, lm->checksums);
        }
    }

  g_simple_async_result_complete (async);
  g_object_unref (async);
  return TRUE; /* handled */
}

static void
on_listing_closed (CockpitTransport *transport,
                   const gchar *problem,
                   gpointer user_data)
{
  GSimpleAsyncResult *async = user_data;
  ListModules *lm;

  lm = g_simple_async_result_get_op_res_gpointer (async);
  g_message ("%s: transport closed while listing cockpit modules: %s", lm->logname, problem);

  g_simple_async_result_complete (async);
  g_object_unref (async);
}

void
cockpit_web_service_modules (CockpitWebService *self,
                             const gchar *host,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  GSimpleAsyncResult *async;
  CockpitSession *session;
  ListModules *lm;
  GBytes *command;

  async = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
                                     cockpit_web_service_modules);

  session = lookup_or_open_session_for_host (self, host, NULL, self->creds, FALSE);

  lm = g_new0 (ListModules, 1);
  lm->logname = g_strdup (host);
  lm->transport = g_object_ref (session->transport);
  lm->checksums = g_hash_table_ref (session->checksums);
  lm->channel = g_strdup_printf ("0:%d", self->next_resource_id++);
  lm->closed_sig = g_signal_connect (lm->transport, "closed", G_CALLBACK (on_listing_closed), async);
  lm->control_sig = g_signal_connect (lm->transport, "control", G_CALLBACK (on_listing_control), async);
  g_simple_async_result_set_op_res_gpointer (async, lm, list_modules_free);

  command = build_control ("command", "open",
                           "channel", lm->channel,
                           "payload", "resource1",
                           NULL);
  cockpit_transport_send (lm->transport, NULL, command);
  g_bytes_unref (command);
}

JsonObject *
cockpit_web_service_modules_finish (CockpitWebService *self,
                                    GAsyncResult *result)
{
  ListModules *lm;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
                                                        cockpit_web_service_modules), NULL);

  lm = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
  if (lm->modules)
    return json_object_ref (lm->modules);

  return NULL;
}
