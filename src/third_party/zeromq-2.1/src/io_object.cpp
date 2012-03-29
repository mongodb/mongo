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

#include "io_object.hpp"
#include "io_thread.hpp"
#include "err.hpp"

zmq::io_object_t::io_object_t (io_thread_t *io_thread_) :
    poller (NULL)
{
    if (io_thread_)
        plug (io_thread_);
}

zmq::io_object_t::~io_object_t ()
{
}

void zmq::io_object_t::plug (io_thread_t *io_thread_)
{
    zmq_assert (io_thread_);
    zmq_assert (!poller);

    //  Retrieve the poller from the thread we are running in.
    poller = io_thread_->get_poller ();
}

void zmq::io_object_t::unplug ()
{
    zmq_assert (poller);

    //  Forget about old poller in preparation to be migrated
    //  to a different I/O thread.
    poller = NULL;
}

zmq::io_object_t::handle_t zmq::io_object_t::add_fd (fd_t fd_)
{
    return poller->add_fd (fd_, this);
}

void zmq::io_object_t::rm_fd (handle_t handle_)
{
    poller->rm_fd (handle_);
}

void zmq::io_object_t::set_pollin (handle_t handle_)
{
    poller->set_pollin (handle_);
}

void zmq::io_object_t::reset_pollin (handle_t handle_)
{
    poller->reset_pollin (handle_);
}

void zmq::io_object_t::set_pollout (handle_t handle_)
{
    poller->set_pollout (handle_);
}

void zmq::io_object_t::reset_pollout (handle_t handle_)
{
    poller->reset_pollout (handle_);
}

void zmq::io_object_t::add_timer (int timeout_, int id_)
{
    poller->add_timer (timeout_, this, id_);
}

void zmq::io_object_t::cancel_timer (int id_)
{
    poller->cancel_timer (this, id_);
}

void zmq::io_object_t::in_event ()
{
    zmq_assert (false);
}

void zmq::io_object_t::out_event ()
{
    zmq_assert (false);
}

void zmq::io_object_t::timer_event (int id_)
{
    zmq_assert (false);
}
