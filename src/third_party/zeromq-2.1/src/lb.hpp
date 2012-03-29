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

#ifndef __ZMQ_LB_HPP_INCLUDED__
#define __ZMQ_LB_HPP_INCLUDED__

#include "array.hpp"
#include "pipe.hpp"

namespace zmq
{

    //  Class manages a set of outbound pipes. On send it load balances
    //  messages fairly among the pipes.
    class lb_t : public i_writer_events
    {
    public:

        lb_t (class own_t *sink_);
        ~lb_t ();

        void attach (writer_t *pipe_);
        void terminate ();
        int send (zmq_msg_t *msg_, int flags_);
        bool has_out ();

        //  i_writer_events interface implementation.
        void activated (writer_t *pipe_);
        void terminated (writer_t *pipe_);

    private:

        //  List of outbound pipes.
        typedef array_t <class writer_t> pipes_t;
        pipes_t pipes;

        //  Number of active pipes. All the active pipes are located at the
        //  beginning of the pipes array.
        pipes_t::size_type active;

        //  Points to the last pipe that the most recent message was sent to.
        pipes_t::size_type current;

        //  True if last we are in the middle of a multipart message.
        bool more;

        //  True if we are dropping current message.
        bool dropping;

        //  Object to send events to.
        class own_t *sink;

        //  If true, termination process is already underway.
        bool terminating;

        lb_t (const lb_t&);
        const lb_t &operator = (const lb_t&);
    };

}

#endif
