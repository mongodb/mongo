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
	int call_evict, call_read, request;

	conn = (CONNECTION *)arg;

	/* Walk the SESSION list and execute requests. */
	while (F_ISSET(conn, WT_WORKQ_RUN)) {
		++conn->api_gen;
		WT_STAT_INCR(conn->stats, workq_passes);

		call_evict = call_read = request = 0;
		for (tp = conn->sessions; (session = *tp) != NULL; ++tp) {
			switch (session->wq_state) {
			case WT_WORKQ_NONE:
				break;
			case WT_WORKQ_FUNC:
				request = 1;
				(void)session->wq_func(session);
				break;
			case WT_WORKQ_READ:
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

				/* Queue the request for the read server. */
				if (session->wq_func(session) != 0)
					break;
				/* FALLTHROUGH */
			case WT_WORKQ_READ_SCHED:
				call_read = 1;
				break;
			case WT_WORKQ_EVICT:
				/*
				 * See comment above regarding read scheduling;
				 * eviction works the same as read, as far as
				 * the workq is concerned.
				 *
				 * We don't have to call a function to contact
				 * the eviction server, currently the eviction
				 * server checks the list of open tables each
				 * time it runs.
				 */
				session->wq_state = WT_WORKQ_EVICT_SCHED;

				/* Queue the request for the eviction server. */
				if (session->wq_func(session) != 0)
					break;
				/* FALLTHROUGH */
			case WT_WORKQ_EVICT_SCHED:
				call_evict = 1;
				break;
			}
		}

		/* If a read is scheduled, check on the read server. */
		if (call_read)
			__wt_workq_read_server(conn, 0);

		/*
		 * If a read or flush is scheduled, check on the eviction
		 * server.
		 */
		if (call_read || call_evict)
			__wt_workq_evict_server(conn, call_evict);

		/* If we didn't find work, yield the processor. */
		if (!request) {
			WT_STAT_INCR(conn->stats, workq_yield);
			__wt_yield();
		}
	}
	return (NULL);
}
