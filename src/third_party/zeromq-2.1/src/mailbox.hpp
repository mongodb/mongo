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

#ifndef __ZMQ_MAILBOX_HPP_INCLUDED__
#define __ZMQ_MAILBOX_HPP_INCLUDED__

#include <stddef.h>

#include "platform.hpp"
#include "signaler.hpp"
#include "fd.hpp"
#include "config.hpp"
#include "command.hpp"
#include "ypipe.hpp"
#include "mutex.hpp"

namespace zmq
{

    class mailbox_t
    {
    public:

        mailbox_t ();
        ~mailbox_t ();

        fd_t get_fd ();
        void send (const command_t &cmd_);
        int recv (command_t *cmd_, int timeout_);
        
    private:

        //  The pipe to store actual commands.
        typedef ypipe_t <command_t, command_pipe_granularity> cpipe_t;
        cpipe_t cpipe;

        //  Signaler to pass signals from writer thread to reader thread.
        signaler_t signaler;

        //  There's only one thread receiving from the mailbox, but there
        //  is arbitrary number of threads sending. Given that ypipe requires
        //  synchronised access on both of its endpoints, we have to synchronise
        //  the sending side.
        mutex_t sync;

        //  True if the underlying pipe is active, ie. when we are allowed to
        //  read commands from it.
        bool active;

        //  Disable copying of mailbox_t object.
        mailbox_t (const mailbox_t&);
        const mailbox_t &operator = (const mailbox_t&);
    };

}

#endif
