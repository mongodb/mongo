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

#ifndef __ZMQ_NAMED_SESSION_HPP_INCLUDED__
#define __ZMQ_NAMED_SESSION_HPP_INCLUDED__

#include "session.hpp"
#include "blob.hpp"

namespace zmq
{

    //  Named session is created by listener object when the peer identifies
    //  itself by a strong name. Named session survives reconnections.

    class named_session_t : public session_t
    {
    public:

        named_session_t (class io_thread_t *io_thread_,
            class socket_base_t *socket_, const options_t &options_, 
            const blob_t &name_);
        ~named_session_t ();

        //  Handlers for events from session base class.
        void attached (const blob_t &peer_identity_);
        void detached ();

    private:

        //  Name of the session. Corresponds to the peer's strong identity.
        blob_t name;

        named_session_t (const named_session_t&);
        const named_session_t &operator = (const named_session_t&);
    };

}

#endif
