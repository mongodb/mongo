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

#include <new>

#include "platform.hpp"
#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "zmq_connecter.hpp"
#include "zmq_engine.hpp"
#include "zmq_init.hpp"
#include "io_thread.hpp"
#include "err.hpp"

zmq::zmq_connecter_t::zmq_connecter_t (class io_thread_t *io_thread_,
      class session_t *session_, const options_t &options_,
      const char *protocol_, const char *address_, bool wait_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    handle_valid (false),
    wait (wait_),
    session (session_),
    current_reconnect_ivl(options.reconnect_ivl)
{
    int rc = tcp_connecter.set_address (protocol_, address_);
    zmq_assert (rc == 0);
}

zmq::zmq_connecter_t::~zmq_connecter_t ()
{
    if (wait)
        cancel_timer (reconnect_timer_id);
    if (handle_valid)
        rm_fd (handle);
}

void zmq::zmq_connecter_t::process_plug ()
{
    if (wait)
        add_reconnect_timer();
    else
        start_connecting ();
}

void zmq::zmq_connecter_t::in_event ()
{
    //  We are not polling for incomming data, so we are actually called
    //  because of error here. However, we can get error on out event as well
    //  on some platforms, so we'll simply handle both events in the same way.
    out_event ();
}

void zmq::zmq_connecter_t::out_event ()
{
    fd_t fd = tcp_connecter.connect ();
    rm_fd (handle);
    handle_valid = false;

    //  Handle the error condition by attempt to reconnect.
    if (fd == retired_fd) {
        tcp_connecter.close ();
        wait = true;
        add_reconnect_timer();
        return;
    }

    //  Choose I/O thread to run connecter in. Given that we are already
    //  running in an I/O thread, there must be at least one available.
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zmq_assert (io_thread);

    //  Create an init object. 
    zmq_init_t *init = new (std::nothrow) zmq_init_t (io_thread, NULL,
        session, fd, options);
    alloc_assert (init);
    launch_sibling (init);

    //  Shut the connecter down.
    terminate ();
}

void zmq::zmq_connecter_t::timer_event (int id_)
{
    zmq_assert (id_ == reconnect_timer_id);
    wait = false;
    start_connecting ();
}

void zmq::zmq_connecter_t::start_connecting ()
{
    //  Open the connecting socket.
    int rc = tcp_connecter.open ();

    //  Connect may succeed in synchronous manner.
    if (rc == 0) {
        handle = add_fd (tcp_connecter.get_fd ());
        handle_valid = true;
        out_event ();
        return;
    }

    //  Connection establishment may be dealyed. Poll for its completion.
    else if (rc == -1 && errno == EAGAIN) {
        handle = add_fd (tcp_connecter.get_fd ());
        handle_valid = true;
        set_pollout (handle);
        return;
    }

    //  Handle any other error condition by eventual reconnect.
    wait = true;
    add_reconnect_timer();
}

void zmq::zmq_connecter_t::add_reconnect_timer()
{
    add_timer (get_new_reconnect_ivl(), reconnect_timer_id);
}

int zmq::zmq_connecter_t::get_new_reconnect_ivl ()
{
#if defined ZMQ_HAVE_WINDOWS
    int pid = (int) GetCurrentProcessId ();
#else
    int pid = (int) getpid ();
#endif

    //  The new interval is the current interval + random value.
    int this_interval = current_reconnect_ivl +
        ((pid * 13) % options.reconnect_ivl);

    //  Only change the current reconnect interval  if the maximum reconnect
    //  interval was set and if it's larger than the reconnect interval.
    if (options.reconnect_ivl_max > 0 && 
        options.reconnect_ivl_max > options.reconnect_ivl) {

        //  Calculate the next interval
        current_reconnect_ivl = current_reconnect_ivl * 2;
        if(current_reconnect_ivl >= options.reconnect_ivl_max) {
            current_reconnect_ivl = options.reconnect_ivl_max;
        }   
    }
    return this_interval;
}
