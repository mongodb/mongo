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

#include <stdlib.h>

#include "command.hpp"

void zmq::deallocate_command (command_t *cmd_)
{
    switch (cmd_->type) {
    case command_t::attach:
        if (cmd_->args.attach.peer_identity)
            free (cmd_->args.attach.peer_identity);
        break;
    case command_t::bind:
        if (cmd_->args.bind.peer_identity)
            free (cmd_->args.bind.peer_identity);
        break;
    default:
        /*  noop  */;
    }
}
