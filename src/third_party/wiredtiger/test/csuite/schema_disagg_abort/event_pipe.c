/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The event framing every pipe shares. A write() of sizeof(SCHEMA_EVENT) is atomic, so writers need
 * no locking; only a dead writer's truncated tail has to be reassembled on read.
 *
 * Two write disciplines: the self-pipe must not lose an event, so a failed write fails the test;
 * the peer's pipe dies with the peer, so a failed write there is reported to the caller.
 */

#include "schema_disagg_abort.h"

#include <sys/select.h>

/*
 * node_event_send --
 *     Relay one event to the peer over the node's out-pipe. Returns false without failing when
 *     there is no peer (no pipe, or the peer is gone), leaving the caller to decide whether
 *     delivery is optional (the workload relay) or mandatory (the hand-over).
 */
bool
node_event_send(TEST_CONFIG *cfg, const SCHEMA_EVENT *ev)
{
    if (cfg->pipe_write_fd < 0)
        return (false);

    const ssize_t nw = write(cfg->pipe_write_fd, ev, sizeof(*ev));
    if (nw < 0) {
        if (errno != EPIPE && errno != EBADF)
            testutil_die(errno, "write event pipe");
        close(cfg->pipe_write_fd);
        cfg->pipe_write_fd = -1;
        cfg->peer_alive = false;
        return (false);
    }
    testutil_assert(nw == (ssize_t)sizeof(*ev));
    return (true);
}

/*
 * pipe_event_write --
 *     Write one event to a pipe whose writer lives in this process - the node's self-pipe. Blocks
 *     while the pipe is full: the consumption rate downstream backpressures the writer.
 */
void
pipe_event_write(int fd, const SCHEMA_EVENT *ev)
{
    ssize_t nw;
    while ((nw = write(fd, ev, sizeof(*ev))) < 0)
        if (errno != EINTR)
            testutil_die(errno, "write event pipe");
    testutil_assert(nw == (ssize_t)sizeof(*ev));
}

/*
 * pipe_wait_readable --
 *     Wait up to a second for a pipe to become readable, so the reader can notice a stop request
 *     even when the source is silent.
 */
bool
pipe_wait_readable(int fd)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv = {1, 0};
    const int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR)
            return (false);
        testutil_die(errno, "reader select pipe");
    }
    return (ret > 0);
}

/*
 * pipe_event_read --
 *     Read one complete event from a pipe. Returns false on EOF (the writer died). The writer's
 *     death can truncate the final write, so reassemble the event from partial reads.
 */
bool
pipe_event_read(int fd, SCHEMA_EVENT *ev)
{
    size_t have = 0;
    while (have < sizeof(*ev)) {
        const ssize_t nr = read(fd, (uint8_t *)ev + have, sizeof(*ev) - have);
        if (nr < 0) {
            if (errno == EINTR)
                continue;
            testutil_die(errno, "reader read pipe");
        }
        if (nr == 0)
            return (false);
        have += (size_t)nr;
    }
    return (true);
}
