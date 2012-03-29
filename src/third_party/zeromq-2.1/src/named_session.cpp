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

#include "named_session.hpp"
#include "socket_base.hpp"

zmq::named_session_t::named_session_t (class io_thread_t *io_thread_,
      socket_base_t *socket_, const options_t &options_,
      const blob_t &name_) :
    session_t (io_thread_, socket_, options_),
    name (name_)
{
    //  Make double sure that the session has valid name.
    zmq_assert (!name.empty ());
    zmq_assert (name [0] != 0);

    if (!socket_->register_session (name, this)) {

        //  TODO: There's already a session with the specified
        //  identity. We should log the error and drop the
        //  session.
        zmq_assert (false);
    }
}

zmq::named_session_t::~named_session_t ()
{
    //  Unregister the session from the global list of named sessions.
    if (!name.empty ())
        unregister_session (name);
}

void zmq::named_session_t::attached (const blob_t &peer_identity_)
{
    if (!name.empty ()) {

        //  If both IDs are temporary, no checking is needed.
        //  TODO: Old ID should be reused in this case...
        if (name.empty () || name [0] != 0 ||
            peer_identity_.empty () || peer_identity_ [0] != 0) {

            //  If we already know the peer name do nothing, just check whether
            //  it haven't changed.
            zmq_assert (name == peer_identity_);
        }
    }
    else if (!peer_identity_.empty ()) {

        //  Store the peer identity.
        name = peer_identity_;

        //  Register the session using the peer name.
        if (!register_session (name, this)) {

            //  TODO: There's already a session with the specified
            //  identity. We should presumably syslog it and drop the
            //  session.
            zmq_assert (false);
        }
    }
}

void zmq::named_session_t::detached ()
{
    //  Do nothing. Named sessions are never destroyed because of disconnection,
    //  neither they have to actively reconnect.
}

