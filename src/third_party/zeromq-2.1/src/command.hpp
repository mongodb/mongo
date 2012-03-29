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

#ifndef __ZMQ_COMMAND_HPP_INCLUDED__
#define __ZMQ_COMMAND_HPP_INCLUDED__

#include "stdint.hpp"

namespace zmq
{

    //  This structure defines the commands that can be sent between threads.

    struct command_t
    {
        //  Object to process the command.
        class object_t *destination;

        enum type_t
        {
            stop,
            plug,
            own,
            attach,
            bind,
            activate_reader,
            activate_writer,
            pipe_term,
            pipe_term_ack,
            term_req,
            term,
            term_ack,
            reap,
            reaped,
            done
        } type;

        union {

            //  Sent to I/O thread to let it know that it should
            //  terminate itself.
            struct {
            } stop;

            //  Sent to I/O object to make it register with its I/O thread.
            struct {
            } plug;

            //  Sent to socket to let it know about the newly created object.
            struct {
                class own_t *object;
            } own;

            //  Attach the engine to the session. If engine is NULL, it informs
            //  session that the connection have failed.
            struct {
                struct i_engine *engine;
                size_t peer_identity_size;
                unsigned char *peer_identity;
            } attach;

            //  Sent from session to socket to establish pipe(s) between them.
            //  Caller have used inc_seqnum beforehand sending the command.
            struct {
                class reader_t *in_pipe;
                class writer_t *out_pipe;
                size_t peer_identity_size;
                unsigned char *peer_identity;
            } bind;

            //  Sent by pipe writer to inform dormant pipe reader that there
            //  are messages in the pipe.
            struct {
            } activate_reader;

            //  Sent by pipe reader to inform pipe writer about how many
            //  messages it has read so far.
            struct {
                uint64_t msgs_read;
            } activate_writer;

            //  Sent by pipe reader to pipe writer to ask it to terminate
            //  its end of the pipe.
            struct {
            } pipe_term;

            //  Pipe writer acknowledges pipe_term command.
            struct {
            } pipe_term_ack;

            //  Sent by I/O object ot the socket to request the shutdown of
            //  the I/O object.
            struct {
                class own_t *object;
            } term_req;

            //  Sent by socket to I/O object to start its shutdown.
            struct {
                int linger;
            } term;

            //  Sent by I/O object to the socket to acknowledge it has
            //  shut down.
            struct {
            } term_ack;

            //  Transfers the ownership of the closed socket
            //  to the reaper thread.
            struct {
                class socket_base_t *socket;
            } reap;

            //  Closed socket notifies the reaper that it's already deallocated.
            struct {
            } reaped;

            //  Sent by reaper thread to the term thread when all the sockets
            //  are successfully deallocated.
            struct {
            } done;

        } args;
    };

    //  Function to deallocate dynamically allocated components of the command.
    void deallocate_command (command_t *cmd_);

}    

#endif
