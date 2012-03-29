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

#ifndef __ZMQ_TRANSIENT_SESSION_HPP_INCLUDED__
#define __ZMQ_TRANSIENT_SESSION_HPP_INCLUDED__

#include "session.hpp"

namespace zmq
{

    //  Transient session is created by the listener when the connected peer
    //  stays anonymous. Transient session is destroyed on disconnect.

    class transient_session_t : public session_t
    {
    public:

        transient_session_t (class io_thread_t *io_thread_,
            class socket_base_t *socket_, const options_t &options_);
        ~transient_session_t ();

    private:

        //  Handlers for events from session base class.
        void attached (const blob_t &peer_identity_);
        void detached ();

        transient_session_t (const transient_session_t&);
        const transient_session_t &operator = (const transient_session_t&);
    };

}

#endif
