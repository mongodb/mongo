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

#include "mailbox.hpp"
#include "platform.hpp"
#include "err.hpp"

zmq::mailbox_t::mailbox_t ()
{
    //  Get the pipe into passive state. That way, if the users starts by
    //  polling on the associated file descriptor it will get woken up when
    //  new command is posted.
    bool ok = cpipe.read (NULL);
    zmq_assert (!ok);
    active = false;
}

zmq::mailbox_t::~mailbox_t ()
{
    //  TODO: Retrieve and deallocate commands inside the cpipe.

    // Work around problem that other threads might still be in our
    // send() method, by waiting on the mutex before disappearing.
    sync.lock ();
    sync.unlock ();
}

zmq::fd_t zmq::mailbox_t::get_fd ()
{
    return signaler.get_fd ();
}

void zmq::mailbox_t::send (const command_t &cmd_)
{
    sync.lock ();
    cpipe.write (cmd_, false);
    bool ok = cpipe.flush ();
    sync.unlock ();
    if (!ok)
        signaler.send ();
}

int zmq::mailbox_t::recv (command_t *cmd_, int timeout_)
{
    //  Try to get the command straight away.
    if (active) {
        bool ok = cpipe.read (cmd_);
        if (ok)
            return 0;

        //  If there are no more commands available, switch into passive state.
        active = false;
        signaler.recv ();
    }

    //  Wait for signal from the command sender.
    int rc = signaler.wait (timeout_);
    if (rc != 0 && (errno == EAGAIN || errno == EINTR))
        return -1;

    //  We've got the signal. Now we can switch into active state.
    active = true;

    //  Get a command.
    errno_assert (rc == 0);
    bool ok = cpipe.read (cmd_);
    zmq_assert (ok);
    return 0;
}

