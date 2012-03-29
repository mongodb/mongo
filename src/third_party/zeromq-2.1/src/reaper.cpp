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

#include "reaper.hpp"
#include "socket_base.hpp"
#include "err.hpp"

zmq::reaper_t::reaper_t (class ctx_t *ctx_, uint32_t tid_) :
    object_t (ctx_, tid_),
    sockets (0),
    terminating (false)
{
    poller = new (std::nothrow) poller_t;
    alloc_assert (poller);

    mailbox_handle = poller->add_fd (mailbox.get_fd (), this);
    poller->set_pollin (mailbox_handle);
}

zmq::reaper_t::~reaper_t ()
{
    delete poller;
}

zmq::mailbox_t *zmq::reaper_t::get_mailbox ()
{
    return &mailbox;
}

void zmq::reaper_t::start ()
{
    //  Start the thread.
    poller->start ();
}

void zmq::reaper_t::stop ()
{
    send_stop ();
}

void zmq::reaper_t::in_event ()
{
    while (true) {

        //  Get the next command. If there is none, exit.
        command_t cmd;
        int rc = mailbox.recv (&cmd, 0);
        if (rc != 0 && errno == EINTR)
            continue;
        if (rc != 0 && errno == EAGAIN)
            break;
        errno_assert (rc == 0);

        //  Process the command.
        cmd.destination->process_command (cmd);
    }
}

void zmq::reaper_t::out_event ()
{
    zmq_assert (false);
}

void zmq::reaper_t::timer_event (int id_)
{
    zmq_assert (false);
}

void zmq::reaper_t::process_stop ()
{
    terminating = true;

    //  If there are no sockets beig reaped finish immediately.
    if (!sockets) {
        send_done ();
        poller->rm_fd (mailbox_handle);
        poller->stop ();
    }
}

void zmq::reaper_t::process_reap (socket_base_t *socket_)
{
    //  Add the socket to the poller.
    socket_->start_reaping (poller);

    //  Start termination of associated I/O object hierarchy.
    socket_->terminate ();
    socket_->check_destroy ();

    ++sockets;
}

void zmq::reaper_t::process_reaped ()
{
    --sockets;

    //  If reaped was already asked to terminate and there are no more sockets,
    //  finish immediately.
    if (!sockets && terminating) {
        send_done ();
        poller->rm_fd (mailbox_handle);
        poller->stop ();
    }
}
