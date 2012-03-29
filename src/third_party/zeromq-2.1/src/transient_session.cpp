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

#include "transient_session.hpp"

zmq::transient_session_t::transient_session_t (class io_thread_t *io_thread_,
      class socket_base_t *socket_, const options_t &options_) :
    session_t (io_thread_, socket_, options_)
{
}

zmq::transient_session_t::~transient_session_t ()
{
}

void zmq::transient_session_t::attached (const blob_t &peer_identity_)
{
}

void zmq::transient_session_t::detached ()
{
    //  There's no way to reestablish a transient session. Tear it down.
    terminate ();
}
