/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"

//  On AIX, poll.h has to be included before zmq.h to get consistent
//  definition of pollfd structure (AIX uses 'reqevents' and 'retnevents'
//  instead of 'events' and 'revents' and defines macros to map from POSIX-y
//  names to AIX-specific names).
#if defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_FREEBSD ||\
    defined ZMQ_HAVE_OPENBSD || defined ZMQ_HAVE_SOLARIS ||\
    defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_QNXNTO ||\
    defined ZMQ_HAVE_HPUX || defined ZMQ_HAVE_AIX ||\
    defined ZMQ_HAVE_NETBSD
#include <poll.h>
#endif

#include "../include/zmq.h"
#include "../include/zmq_utils.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <new>

#include "device.hpp"
#include "socket_base.hpp"
#include "msg_content.hpp"
#include "stdint.hpp"
#include "config.hpp"
#include "likely.hpp"
#include "clock.hpp"
#include "ctx.hpp"
#include "err.hpp"
#include "fd.hpp"

#if !defined ZMQ_HAVE_WINDOWS
#include <unistd.h>
#endif

#if defined ZMQ_HAVE_OPENPGM
#define __PGM_WININT_H__
#include <pgm/pgm.h>

//  TODO: OpenPGM redefines bool -- remove this once OpenPGM is fixed.
#if defined bool
#undef bool
#endif

#endif

void zmq_version (int *major_, int *minor_, int *patch_)
{
    *major_ = ZMQ_VERSION_MAJOR;
    *minor_ = ZMQ_VERSION_MINOR;
    *patch_ = ZMQ_VERSION_PATCH;
}

const char *zmq_strerror (int errnum_)
{
    return zmq::errno_to_string (errnum_);
}

int zmq_msg_init (zmq_msg_t *msg_)
{
    msg_->content = (zmq::msg_content_t*) ZMQ_VSM;
    msg_->flags = (unsigned char) ~ZMQ_MSG_MASK;
    msg_->vsm_size = 0;
    return 0;
}

int zmq_msg_init_size (zmq_msg_t *msg_, size_t size_)
{
    if (size_ <= ZMQ_MAX_VSM_SIZE) {
        msg_->content = (zmq::msg_content_t*) ZMQ_VSM;
        msg_->flags = (unsigned char) ~ZMQ_MSG_MASK;
        msg_->vsm_size = (uint8_t) size_;
    }
    else {
        msg_->content =
            (zmq::msg_content_t*) malloc (sizeof (zmq::msg_content_t) + size_);
        if (!msg_->content) {
            errno = ENOMEM;
            return -1;
        }
        msg_->flags = (unsigned char) ~ZMQ_MSG_MASK;

        zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
        content->data = (void*) (content + 1);
        content->size = size_;
        content->ffn = NULL;
        content->hint = NULL;
        new (&content->refcnt) zmq::atomic_counter_t ();
    }
    return 0;
}

int zmq_msg_init_data (zmq_msg_t *msg_, void *data_, size_t size_,
    zmq_free_fn *ffn_, void *hint_)
{
    msg_->content = (zmq::msg_content_t*) malloc (sizeof (zmq::msg_content_t));
    alloc_assert (msg_->content);
    msg_->flags = (unsigned char) ~ZMQ_MSG_MASK;
    zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
    content->data = data_;
    content->size = size_;
    content->ffn = ffn_;
    content->hint = hint_;
    new (&content->refcnt) zmq::atomic_counter_t ();
    return 0;
}

int zmq_msg_close (zmq_msg_t *msg_)
{
    //  Check the validity tag.
    if (unlikely (msg_->flags | ZMQ_MSG_MASK) != 0xff) {
        errno = EFAULT;
        return -1;
    }

    //  For VSMs and delimiters there are no resources to free.
    if (msg_->content != (zmq::msg_content_t*) ZMQ_DELIMITER &&
          msg_->content != (zmq::msg_content_t*) ZMQ_VSM) {

        //  If the content is not shared, or if it is shared and the reference.
        //  count has dropped to zero, deallocate it.
        zmq::msg_content_t *content = (zmq::msg_content_t*) msg_->content;
        if (!(msg_->flags & ZMQ_MSG_SHARED) || !content->refcnt.sub (1)) {

            //  We used "placement new" operator to initialize the reference.
            //  counter so we call its destructor now.
            content->refcnt.~atomic_counter_t ();

            if (content->ffn)
                content->ffn (content->data, content->hint);
            free (content);
        }
    }

    //  Remove the validity tag from the message.
    msg_->flags = 0;

    return 0;
}

int zmq_msg_move (zmq_msg_t *dest_, zmq_msg_t *src_)
{
#if 0
    //  Check the validity tags.
    if (unlikely ((dest_->flags | ZMQ_MSG_MASK) != 0xff ||
          (src_->flags | ZMQ_MSG_MASK) != 0xff)) {
        errno = EFAULT;
        return -1;
    }
#endif
    zmq_msg_close (dest_);
    *dest_ = *src_;
    zmq_msg_init (src_);
    return 0;
}

int zmq_msg_copy (zmq_msg_t *dest_, zmq_msg_t *src_)
{
    //  Check the validity tags.
    if (unlikely ((dest_->flags | ZMQ_MSG_MASK) != 0xff ||
          (src_->flags | ZMQ_MSG_MASK) != 0xff)) {
        errno = EFAULT;
        return -1;
    }

    zmq_msg_close (dest_);

    //  VSMs and delimiters require no special handling.
    if (src_->content != (zmq::msg_content_t*) ZMQ_DELIMITER &&
          src_->content != (zmq::msg_content_t*) ZMQ_VSM) {

        //  One reference is added to shared messages. Non-shared messages
        //  are turned into shared messages and reference count is set to 2.
        zmq::msg_content_t *content = (zmq::msg_content_t*) src_->content;
        if (src_->flags & ZMQ_MSG_SHARED)
            content->refcnt.add (1);
        else {
            src_->flags |= ZMQ_MSG_SHARED;
            content->refcnt.set (2);
        }
    }

    *dest_ = *src_;
    return 0;
}

void *zmq_msg_data (zmq_msg_t *msg_)
{
    zmq_assert ((msg_->flags | ZMQ_MSG_MASK) == 0xff);

    if (msg_->content == (zmq::msg_content_t*) ZMQ_VSM)
        return msg_->vsm_data;
    if (msg_->content == (zmq::msg_content_t*) ZMQ_DELIMITER)
        return NULL;

    return ((zmq::msg_content_t*) msg_->content)->data;
}

size_t zmq_msg_size (zmq_msg_t *msg_)
{
    zmq_assert ((msg_->flags | ZMQ_MSG_MASK) == 0xff);

    if (msg_->content == (zmq::msg_content_t*) ZMQ_VSM)
        return msg_->vsm_size;
    if (msg_->content == (zmq::msg_content_t*) ZMQ_DELIMITER)
        return 0;

    return ((zmq::msg_content_t*) msg_->content)->size;
}

void *zmq_init (int io_threads_)
{
    if (io_threads_ < 0) {
        errno = EINVAL;
        return NULL;
    }

#if defined ZMQ_HAVE_OPENPGM

    //  Init PGM transport. Ensure threading and timer are enabled. Find PGM
    //  protocol ID. Note that if you want to use gettimeofday and sleep for
    //  openPGM timing, set environment variables PGM_TIMER to "GTOD" and
    //  PGM_SLEEP to "USLEEP".
    pgm_error_t *pgm_error = NULL;
    const bool ok = pgm_init (&pgm_error);
    if (ok != TRUE) {

        //  Invalid parameters don't set pgm_error_t
        zmq_assert (pgm_error != NULL);
        if (pgm_error->domain == PGM_ERROR_DOMAIN_TIME && (
              pgm_error->code == PGM_ERROR_FAILED)) {

            //  Failed to access RTC or HPET device.
            pgm_error_free (pgm_error);
            errno = EINVAL;
            return NULL;
        }

        //  PGM_ERROR_DOMAIN_ENGINE: WSAStartup errors or missing WSARecvMsg.
        zmq_assert (false);
    }
#endif

#ifdef ZMQ_HAVE_WINDOWS
    //  Intialise Windows sockets. Note that WSAStartup can be called multiple
    //  times given that WSACleanup will be called for each WSAStartup.
   //  We do this before the ctx constructor since its embedded mailbox_t
   //  object needs Winsock to be up and running.
    WORD version_requested = MAKEWORD (2, 2);
    WSADATA wsa_data;
    int rc = WSAStartup (version_requested, &wsa_data);
    zmq_assert (rc == 0);
    zmq_assert (LOBYTE (wsa_data.wVersion) == 2 &&
        HIBYTE (wsa_data.wVersion) == 2);
#endif

    //  Create 0MQ context.
    zmq::ctx_t *ctx = new (std::nothrow) zmq::ctx_t ((uint32_t) io_threads_);
    alloc_assert (ctx);
    return (void*) ctx;
}

int zmq_term (void *ctx_)
{
    if (!ctx_ || !((zmq::ctx_t*) ctx_)->check_tag ()) {
        errno = EFAULT;
        return -1;
    }

    int rc = ((zmq::ctx_t*) ctx_)->terminate ();
    int en = errno;

#ifdef ZMQ_HAVE_WINDOWS
    //  On Windows, uninitialise socket layer.
    rc = WSACleanup ();
    wsa_assert (rc != SOCKET_ERROR);
#endif

#if defined ZMQ_HAVE_OPENPGM
    //  Shut down the OpenPGM library.
    if (pgm_shutdown () != TRUE)
        zmq_assert (false);
#endif

    errno = en;
    return rc;
}

void *zmq_socket (void *ctx_, int type_)
{
    if (!ctx_ || !((zmq::ctx_t*) ctx_)->check_tag ()) {
        errno = EFAULT;
        return NULL;
    }
    return (void*) (((zmq::ctx_t*) ctx_)->create_socket (type_));
}

int zmq_close (void *s_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    ((zmq::socket_base_t*) s_)->close ();
    return 0;
}

int zmq_setsockopt (void *s_, int option_, const void *optval_,
    size_t optvallen_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->setsockopt (option_, optval_,
        optvallen_));
}

int zmq_getsockopt (void *s_, int option_, void *optval_, size_t *optvallen_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->getsockopt (option_, optval_,
        optvallen_));
}

int zmq_bind (void *s_, const char *addr_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->bind (addr_));
}

int zmq_connect (void *s_, const char *addr_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->connect (addr_));
}

int zmq_send (void *s_, zmq_msg_t *msg_, int flags_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->send (msg_, flags_));
}

int zmq_recv (void *s_, zmq_msg_t *msg_, int flags_)
{
    if (!s_ || !((zmq::socket_base_t*) s_)->check_tag ()) {
        errno = ENOTSOCK;
        return -1;
    }
    return (((zmq::socket_base_t*) s_)->recv (msg_, flags_));
}

#if defined ZMQ_FORCE_SELECT
#define ZMQ_POLL_BASED_ON_SELECT
#elif defined ZMQ_FORCE_POLL
#define ZMQ_POLL_BASED_ON_POLL
#elif defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_FREEBSD ||\
    defined ZMQ_HAVE_OPENBSD || defined ZMQ_HAVE_SOLARIS ||\
    defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_QNXNTO ||\
    defined ZMQ_HAVE_HPUX || defined ZMQ_HAVE_AIX ||\
    defined ZMQ_HAVE_NETBSD
#define ZMQ_POLL_BASED_ON_POLL
#elif defined ZMQ_HAVE_WINDOWS || defined ZMQ_HAVE_OPENVMS
#define ZMQ_POLL_BASED_ON_SELECT
#endif

int zmq_poll (zmq_pollitem_t *items_, int nitems_, long timeout_)
{
#if defined ZMQ_POLL_BASED_ON_POLL
    if (unlikely (nitems_ < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (unlikely (nitems_ == 0)) {
        if (timeout_ == 0)
            return 0;
#if defined ZMQ_HAVE_WINDOWS
        Sleep (timeout_ > 0 ? timeout_ / 1000 : INFINITE);
        return 0;
#else
        usleep (timeout_);
	return 0;
#endif
    }

    if (!items_) {
        errno = EFAULT;
        return -1;
    }

    zmq::clock_t clock;
    uint64_t now = 0;
    uint64_t end = 0;

    pollfd *pollfds = (pollfd*) malloc (nitems_ * sizeof (pollfd));
    alloc_assert (pollfds);

    //  Build pollset for poll () system call.
    for (int i = 0; i != nitems_; i++) {

        //  If the poll item is a 0MQ socket, we poll on the file descriptor
        //  retrieved by the ZMQ_FD socket option.
        if (items_ [i].socket) {
            size_t zmq_fd_size = sizeof (zmq::fd_t);
            if (zmq_getsockopt (items_ [i].socket, ZMQ_FD, &pollfds [i].fd,
                &zmq_fd_size) == -1) {
                free (pollfds);
                return -1;
            }
            pollfds [i].events = items_ [i].events ? POLLIN : 0;
        }
        //  Else, the poll item is a raw file descriptor. Just convert the
        //  events to normal POLLIN/POLLOUT for poll ().
        else {
            pollfds [i].fd = items_ [i].fd;
            pollfds [i].events =
                (items_ [i].events & ZMQ_POLLIN ? POLLIN : 0) |
                (items_ [i].events & ZMQ_POLLOUT ? POLLOUT : 0);
        }
    }

    bool first_pass = true;
    int nevents = 0;

    while (true) {

         //  Compute the timeout for the subsequent poll.
         int timeout;
         if (first_pass)
             timeout = 0;
         else if (timeout_ < 0)
             timeout = -1;
         else
             timeout = end - now;

        //  Wait for events.
        while (true) {
            int rc = poll (pollfds, nitems_, timeout);
            if (rc == -1 && errno == EINTR) {
                free (pollfds);
                return -1;
            }
            errno_assert (rc >= 0);
            break;
        }

        //  Check for the events.
        for (int i = 0; i != nitems_; i++) {

            items_ [i].revents = 0;

            //  The poll item is a 0MQ socket. Retrieve pending events
            //  using the ZMQ_EVENTS socket option.
            if (items_ [i].socket) {
                size_t zmq_events_size = sizeof (uint32_t);
                uint32_t zmq_events;
                if (zmq_getsockopt (items_ [i].socket, ZMQ_EVENTS, &zmq_events,
                    &zmq_events_size) == -1) {
                    free (pollfds);
                    return -1;
                }
                if ((items_ [i].events & ZMQ_POLLOUT) &&
                      (zmq_events & ZMQ_POLLOUT))
                    items_ [i].revents |= ZMQ_POLLOUT;
                if ((items_ [i].events & ZMQ_POLLIN) &&
                      (zmq_events & ZMQ_POLLIN))
                    items_ [i].revents |= ZMQ_POLLIN;
            }
            //  Else, the poll item is a raw file descriptor, simply convert
            //  the events to zmq_pollitem_t-style format.
            else {
                if (pollfds [i].revents & POLLIN)
                    items_ [i].revents |= ZMQ_POLLIN;
                if (pollfds [i].revents & POLLOUT)
                    items_ [i].revents |= ZMQ_POLLOUT;
                if (pollfds [i].revents & ~(POLLIN | POLLOUT))
                    items_ [i].revents |= ZMQ_POLLERR;
            }

            if (items_ [i].revents)
                nevents++;
        }

        //  If timout is zero, exit immediately whether there are events or not.
        if (timeout_ == 0)
            break;

        //  If there are events to return, we can exit immediately.
        if (nevents)
            break;

        //  At this point we are meant to wait for events but there are none.
        //  If timeout is infinite we can just loop until we get some events.
        if (timeout_ < 0) {
            if (first_pass)
                first_pass = false;
            continue;
        }

        //  The timeout is finite and there are no events. In the first pass
        //  we get a timestamp of when the polling have begun. (We assume that
        //  first pass have taken negligible time). We also compute the time
        //  when the polling should time out.
        if (first_pass) {
            now = clock.now_ms ();
            end = now + (timeout_ / 1000);
            if (now == end)
                break;
            first_pass = false;
            continue;
        }

        //  Find out whether timeout have expired.
        now = clock.now_ms ();
        if (now >= end)
            break;
    }

    free (pollfds);
    return nevents;

#elif defined ZMQ_POLL_BASED_ON_SELECT

    if (unlikely (nitems_ < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (unlikely (nitems_ == 0)) {
        if (timeout_ == 0)
            return 0;
#if defined ZMQ_HAVE_WINDOWS
        Sleep (timeout_ > 0 ? timeout_ / 1000 : INFINITE);
        return 0;
#else
        usleep (timeout_);
        return 0;
#endif
    }

    if (!items_) {
        errno = EFAULT;
        return -1;
    }

    zmq::clock_t clock;
    uint64_t now = 0;
    uint64_t end = 0;

    //  Ensure we do not attempt to select () on more than FD_SETSIZE
    //  file descriptors.
    zmq_assert (nitems_ <= FD_SETSIZE);

    fd_set pollset_in;
    FD_ZERO (&pollset_in);
    fd_set pollset_out;
    FD_ZERO (&pollset_out);
    fd_set pollset_err;
    FD_ZERO (&pollset_err);

    zmq::fd_t maxfd = 0;

    //  Build the fd_sets for passing to select ().
    for (int i = 0; i != nitems_; i++) {

        //  If the poll item is a 0MQ socket we are interested in input on the
        //  notification file descriptor retrieved by the ZMQ_FD socket option.
        if (items_ [i].socket) {
            size_t zmq_fd_size = sizeof (zmq::fd_t);
            zmq::fd_t notify_fd;
            if (zmq_getsockopt (items_ [i].socket, ZMQ_FD, &notify_fd,
                &zmq_fd_size) == -1)
                return -1;
            if (items_ [i].events) {
                FD_SET (notify_fd, &pollset_in);
                if (maxfd < notify_fd)
                    maxfd = notify_fd;
            }
        }
        //  Else, the poll item is a raw file descriptor. Convert the poll item
        //  events to the appropriate fd_sets.
        else {
            if (items_ [i].events & ZMQ_POLLIN)
                FD_SET (items_ [i].fd, &pollset_in);
            if (items_ [i].events & ZMQ_POLLOUT)
                FD_SET (items_ [i].fd, &pollset_out);
            if (items_ [i].events & ZMQ_POLLERR)
                FD_SET (items_ [i].fd, &pollset_err);
            if (maxfd < items_ [i].fd)
                maxfd = items_ [i].fd;
        }
    }

    bool first_pass = true;
    int nevents = 0;
    fd_set inset, outset, errset;

    while (true) {

        //  Compute the timeout for the subsequent poll.
        timeval timeout;
        timeval *ptimeout;
        if (first_pass) {
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
            ptimeout = &timeout;
        }
        else if (timeout_ < 0)
            ptimeout = NULL;
        else {
            timeout.tv_sec = (long) ((end - now) / 1000);
            timeout.tv_usec = (long) ((end - now) % 1000 * 1000);
            ptimeout = &timeout;
        }

        //  Wait for events. Ignore interrupts if there's infinite timeout.
        while (true) {
            memcpy (&inset, &pollset_in, sizeof (fd_set));
            memcpy (&outset, &pollset_out, sizeof (fd_set));
            memcpy (&errset, &pollset_err, sizeof (fd_set));
#if defined ZMQ_HAVE_WINDOWS
            int rc = select (0, &inset, &outset, &errset, ptimeout);
            if (unlikely (rc == SOCKET_ERROR)) {
                zmq::wsa_error_to_errno ();
                if (errno == ENOTSOCK)
                    return -1;
                wsa_assert (false);
            }
#else
            int rc = select (maxfd + 1, &inset, &outset, &errset, ptimeout);
            if (unlikely (rc == -1)) {
                if (errno == EINTR || errno == EBADF)
                    return -1;
                errno_assert (false);
            }
#endif
            break;
        }

        //  Check for the events.
        for (int i = 0; i != nitems_; i++) {

            items_ [i].revents = 0;

            //  The poll item is a 0MQ socket. Retrieve pending events
            //  using the ZMQ_EVENTS socket option.
            if (items_ [i].socket) {
                size_t zmq_events_size = sizeof (uint32_t);
                uint32_t zmq_events;
                if (zmq_getsockopt (items_ [i].socket, ZMQ_EVENTS, &zmq_events,
                      &zmq_events_size) == -1)
                    return -1;
                if ((items_ [i].events & ZMQ_POLLOUT) &&
                      (zmq_events & ZMQ_POLLOUT))
                    items_ [i].revents |= ZMQ_POLLOUT;
                if ((items_ [i].events & ZMQ_POLLIN) &&
                      (zmq_events & ZMQ_POLLIN))
                    items_ [i].revents |= ZMQ_POLLIN;
            }
            //  Else, the poll item is a raw file descriptor, simply convert
            //  the events to zmq_pollitem_t-style format.
            else {
                if (FD_ISSET (items_ [i].fd, &inset))
                    items_ [i].revents |= ZMQ_POLLIN;
                if (FD_ISSET (items_ [i].fd, &outset))
                    items_ [i].revents |= ZMQ_POLLOUT;
                if (FD_ISSET (items_ [i].fd, &errset))
                    items_ [i].revents |= ZMQ_POLLERR;
            }

            if (items_ [i].revents)
                nevents++;
        }

        //  If timout is zero, exit immediately whether there are events or not.
        if (timeout_ == 0)
            break;

        //  If there are events to return, we can exit immediately.
        if (nevents)
            break;

        //  At this point we are meant to wait for events but there are none.
        //  If timeout is infinite we can just loop until we get some events.
        if (timeout_ < 0) {
            if (first_pass)
                first_pass = false;
            continue;
        }

        //  The timeout is finite and there are no events. In the first pass
        //  we get a timestamp of when the polling have begun. (We assume that
        //  first pass have taken negligible time). We also compute the time
        //  when the polling should time out.
        if (first_pass) {
            now = clock.now_ms ();
            end = now + (timeout_ / 1000);
            if (now == end)
                break;
            first_pass = false;
            continue;
        }

        //  Find out whether timeout have expired.
        now = clock.now_ms ();
        if (now >= end)
            break;
    }

    return nevents;

#else
    //  Exotic platforms that support neither poll() nor select().
    errno = ENOTSUP;
    return -1;
#endif
}

#if defined ZMQ_POLL_BASED_ON_SELECT
#undef ZMQ_POLL_BASED_ON_SELECT
#endif
#if defined ZMQ_POLL_BASED_ON_POLL
#undef ZMQ_POLL_BASED_ON_POLL
#endif

int zmq_errno ()
{
    return errno;
}

int zmq_device (int device_, void *insocket_, void *outsocket_)
{
    if (!insocket_ || !outsocket_) {
        errno = EFAULT;
        return -1;
    }

    if (device_ != ZMQ_FORWARDER && device_ != ZMQ_QUEUE &&
          device_ != ZMQ_STREAMER) {
       errno = EINVAL;
       return -1;
    }

    return zmq::device ((zmq::socket_base_t*) insocket_,
        (zmq::socket_base_t*) outsocket_);
}

////////////////////////////////////////////////////////////////////////////////
//  0MQ utils - to be used by perf tests
////////////////////////////////////////////////////////////////////////////////

void zmq_sleep (int seconds_)
{
#if defined ZMQ_HAVE_WINDOWS
    Sleep (seconds_ * 1000);
#else
    sleep (seconds_);
#endif
}

void *zmq_stopwatch_start ()
{
    uint64_t *watch = (uint64_t*) malloc (sizeof (uint64_t));
    alloc_assert (watch);
    *watch = zmq::clock_t::now_us ();
    return (void*) watch;
}

unsigned long zmq_stopwatch_stop (void *watch_)
{
    uint64_t end = zmq::clock_t::now_us ();
    uint64_t start = *(uint64_t*) watch_;
    free (watch_);
    return (unsigned long) (end - start);
}
