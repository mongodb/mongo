/* MIT License
 *
 * Copyright (c) 1998 Massachusetts Institute of Technology
 * Copyright (c) The c-ares project and its contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ares_setup.h"

#include "ares.h"
#include "ares_private.h"
#include <assert.h>

static void ares__requeue_queries(struct server_connection *conn)
{
  struct query  *query;
  struct timeval now = ares__tvnow();

  while ((query = ares__llist_first_val(conn->queries_to_conn)) != NULL) {
    ares__requeue_query(query, &now);
  }
}

void ares__close_connection(struct server_connection *conn)
{
  struct server_state *server  = conn->server;
  ares_channel_t      *channel = server->channel;

  /* Unlink */
  ares__llist_node_claim(
    ares__htable_asvp_get_direct(channel->connnode_by_socket, conn->fd));
  ares__htable_asvp_remove(channel->connnode_by_socket, conn->fd);

  if (conn->is_tcp) {
    /* Reset any existing input and output buffer. */
    ares__buf_consume(server->tcp_parser, ares__buf_len(server->tcp_parser));
    ares__buf_consume(server->tcp_send, ares__buf_len(server->tcp_send));
    server->tcp_conn = NULL;
  }

  /* Requeue queries to other connections */
  ares__requeue_queries(conn);

  ares__llist_destroy(conn->queries_to_conn);

  SOCK_STATE_CALLBACK(channel, conn->fd, 0, 0);
  ares__close_socket(channel, conn->fd);

  ares_free(conn);
}

void ares__close_sockets(struct server_state *server)
{
  ares__llist_node_t *node;

  while ((node = ares__llist_node_first(server->connections)) != NULL) {
    struct server_connection *conn = ares__llist_node_val(node);
    ares__close_connection(conn);
  }
}

void ares__check_cleanup_conn(const ares_channel_t     *channel,
                              struct server_connection *conn)
{
  ares_bool_t do_cleanup = ARES_FALSE;

  if (channel == NULL || conn == NULL) {
    return;
  }

  if (ares__llist_len(conn->queries_to_conn)) {
    return;
  }

  /* If we are configured not to stay open, close it out */
  if (!(channel->flags & ARES_FLAG_STAYOPEN)) {
    do_cleanup = ARES_TRUE;
  }

  /* If the udp connection hit its max queries, always close it */
  if (!conn->is_tcp && channel->udp_max_queries > 0 &&
      conn->total_queries >= channel->udp_max_queries) {
    do_cleanup = ARES_TRUE;
  }

  if (!do_cleanup) {
    return;
  }

  ares__close_connection(conn);
}
