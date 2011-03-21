/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_workq_srvr --
 *      Routine to process the SESSION work queue.
 */
void *
__wt_workq_srvr(void *arg)
{
	CONNECTION *conn;
	SESSION **tp, *session;
	int chk_read, request;

	conn = (CONNECTION *)arg;

	/* Walk the SESSION list and execute requests. */
	while (F_ISSET(conn, WT_WORKQ_RUN)) {
		++conn->api_gen;
		WT_STAT_INCR(conn->stats, WORKQ_PASSES);

		chk_read = request = 0;
		for (tp = conn->sessions; (session = *tp) != NULL; ++tp) {
			switch (session->wq_state) {
			case WT_WORKQ_NONE:
				break;
			case WT_WORKQ_FUNC:
				request = 1;
				(void)session->wq_func(session);
				break;
			case WT_WORKQ_READ:
				request = 1;

				/*
				 * Call a function which makes a request of the
				 * read server.  There are two read states: READ
				 * (the initial request), and READ_SCHED (the
				 * function has been called and we're waiting on
				 * the read to complete).  There are two states
				 * because we can race with the server: if the
				 * called function adds itself to the queue just
				 * as the server is going to sleep, the server
				 * might not see the request.   So, READ_SCHED
				 * means we don't have to call the function, but
				 * we do have check if the server is running.
				 *
				 * The read state is eventually reset by the
				 * read server, so we set it before we call the
				 * function that will contact the server, so we
				 * can't race on that update.
				 */
				session->wq_state = WT_WORKQ_READ_SCHED;

				/*
				 * Call the function (which contacts the read
				 * server).  If that call fails, we're done.
				 */
				if (session->wq_func(session) != 0)
					break;

				/* FALLTHROUGH */
			case WT_WORKQ_READ_SCHED:
				chk_read = 1;
				break;
			}
		}

		/* If a read is scheduled, check on the read server. */
		if (chk_read)
			__wt_workq_read_server(conn, 0);

		/* Check on the cache eviction server. */
		__wt_workq_evict_server(conn, 0);

		/* If we didn't find work, yield the processor. */
		if (!request) {
			WT_STAT_INCR(conn->stats, WORKQ_YIELD);
			__wt_yield();
		}
	}
	return (NULL);
}
