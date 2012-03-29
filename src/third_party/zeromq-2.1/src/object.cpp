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

#include <string.h>
#include <stdarg.h>

#include "object.hpp"
#include "ctx.hpp"
#include "err.hpp"
#include "pipe.hpp"
#include "io_thread.hpp"
#include "session.hpp"
#include "socket_base.hpp"

zmq::object_t::object_t (ctx_t *ctx_, uint32_t tid_) :
    ctx (ctx_),
    tid (tid_)
{
}

zmq::object_t::object_t (object_t *parent_) :
    ctx (parent_->ctx),
    tid (parent_->tid)
{
}

zmq::object_t::~object_t ()
{
}

uint32_t zmq::object_t::get_tid ()
{
    return tid;
}

zmq::ctx_t *zmq::object_t::get_ctx ()
{
    return ctx;
}

void zmq::object_t::process_command (command_t &cmd_)
{
    switch (cmd_.type) {

    case command_t::activate_reader:
        process_activate_reader ();
        break;

    case command_t::activate_writer:
        process_activate_writer (cmd_.args.activate_writer.msgs_read);
        break;

    case command_t::stop:
        process_stop ();
        break;

    case command_t::plug:
        process_plug ();
        process_seqnum ();
        return;

    case command_t::own:
        process_own (cmd_.args.own.object);
        process_seqnum ();
        break;

    case command_t::attach:
        process_attach (cmd_.args.attach.engine,
            cmd_.args.attach.peer_identity ?
            blob_t (cmd_.args.attach.peer_identity,
            cmd_.args.attach.peer_identity_size) : blob_t ());
        process_seqnum ();
        break;

    case command_t::bind:
        process_bind (cmd_.args.bind.in_pipe, cmd_.args.bind.out_pipe,
            cmd_.args.bind.peer_identity ? blob_t (cmd_.args.bind.peer_identity,
            cmd_.args.bind.peer_identity_size) : blob_t ());
        process_seqnum ();
        break;

    case command_t::pipe_term:
        process_pipe_term ();
        return;

    case command_t::pipe_term_ack:
        process_pipe_term_ack ();
        break;

    case command_t::term_req:
        process_term_req (cmd_.args.term_req.object);
        break;
    
    case command_t::term:
        process_term (cmd_.args.term.linger);
        break;

    case command_t::term_ack:
        process_term_ack ();
        break;

    case command_t::reap:
        process_reap (cmd_.args.reap.socket);
        break;

    case command_t::reaped:
        process_reaped ();
        break;

    default:
        zmq_assert (false);
    }

    //  The assumption here is that each command is processed once only,
    //  so deallocating it after processing is all right.
    deallocate_command (&cmd_);
}

int zmq::object_t::register_endpoint (const char *addr_, endpoint_t &endpoint_)
{
    return ctx->register_endpoint (addr_, endpoint_);
}

void zmq::object_t::unregister_endpoints (socket_base_t *socket_)
{
    return ctx->unregister_endpoints (socket_);
}

zmq::endpoint_t zmq::object_t::find_endpoint (const char *addr_)
{
    return ctx->find_endpoint (addr_);
}

void zmq::object_t::destroy_socket (socket_base_t *socket_)
{
    ctx->destroy_socket (socket_);
}

void zmq::object_t::log (const char *format_, ...)
{
    va_list args;
    va_start (args, format_);
    ctx->log (format_, args);
    va_end (args);
}

zmq::io_thread_t *zmq::object_t::choose_io_thread (uint64_t affinity_)
{
    return ctx->choose_io_thread (affinity_);
}

void zmq::object_t::send_stop ()
{
    //  'stop' command goes always from administrative thread to
    //  the current object. 
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = this;
    cmd.type = command_t::stop;
    ctx->send_command (tid, cmd);
}

void zmq::object_t::send_plug (own_t *destination_, bool inc_seqnum_)
{
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::plug;
    send_command (cmd);
}

void zmq::object_t::send_own (own_t *destination_, own_t *object_)
{
    destination_->inc_seqnum ();
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::own;
    cmd.args.own.object = object_;
    send_command (cmd);
}

void zmq::object_t::send_attach (session_t *destination_, i_engine *engine_,
    const blob_t &peer_identity_, bool inc_seqnum_)
{
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::attach;
    cmd.args.attach.engine = engine_;
    if (peer_identity_.empty ()) {
        cmd.args.attach.peer_identity_size = 0;
        cmd.args.attach.peer_identity = NULL;
    }
    else {
        zmq_assert (peer_identity_.size () <= 0xff);
        cmd.args.attach.peer_identity_size =
            (unsigned char) peer_identity_.size ();
        cmd.args.attach.peer_identity =
            (unsigned char*) malloc (peer_identity_.size ());
        alloc_assert (cmd.args.attach.peer_identity_size);
        memcpy (cmd.args.attach.peer_identity, peer_identity_.data (),
            peer_identity_.size ());
    }
    send_command (cmd);
}

void zmq::object_t::send_bind (own_t *destination_, reader_t *in_pipe_,
    writer_t *out_pipe_, const blob_t &peer_identity_, bool inc_seqnum_)
{
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::bind;
    cmd.args.bind.in_pipe = in_pipe_;
    cmd.args.bind.out_pipe = out_pipe_;
    if (peer_identity_.empty ()) {
        cmd.args.bind.peer_identity_size = 0;
        cmd.args.bind.peer_identity = NULL;
    }
    else {
        zmq_assert (peer_identity_.size () <= 0xff);
        cmd.args.bind.peer_identity_size =
            (unsigned char) peer_identity_.size ();
        cmd.args.bind.peer_identity =
            (unsigned char*) malloc (peer_identity_.size ());
        alloc_assert (cmd.args.bind.peer_identity_size);
        memcpy (cmd.args.bind.peer_identity, peer_identity_.data (),
            peer_identity_.size ());
    }
    send_command (cmd);
}

void zmq::object_t::send_activate_reader (reader_t *destination_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::activate_reader;
    send_command (cmd);
}

void zmq::object_t::send_activate_writer (writer_t *destination_,
    uint64_t msgs_read_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::activate_writer;
    cmd.args.activate_writer.msgs_read = msgs_read_;
    send_command (cmd);
}

void zmq::object_t::send_pipe_term (writer_t *destination_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::pipe_term;
    send_command (cmd);
}

void zmq::object_t::send_pipe_term_ack (reader_t *destination_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::pipe_term_ack;
    send_command (cmd);
}

void zmq::object_t::send_term_req (own_t *destination_,
    own_t *object_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::term_req;
    cmd.args.term_req.object = object_;
    send_command (cmd);
}

void zmq::object_t::send_term (own_t *destination_, int linger_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::term;
    cmd.args.term.linger = linger_;
    send_command (cmd);
}

void zmq::object_t::send_term_ack (own_t *destination_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = destination_;
    cmd.type = command_t::term_ack;
    send_command (cmd);
}

void zmq::object_t::send_reap (class socket_base_t *socket_)
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = ctx->get_reaper ();
    cmd.type = command_t::reap;
    cmd.args.reap.socket = socket_;
    send_command (cmd);
}

void zmq::object_t::send_reaped ()
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = ctx->get_reaper ();
    cmd.type = command_t::reaped;
    send_command (cmd);
}

void zmq::object_t::send_done ()
{
    command_t cmd;
#if defined ZMQ_MAKE_VALGRIND_HAPPY
    memset (&cmd, 0, sizeof (cmd));
#endif
    cmd.destination = NULL;
    cmd.type = command_t::done;
    ctx->send_command (ctx_t::term_tid, cmd);
}

void zmq::object_t::process_stop ()
{
    zmq_assert (false);
}

void zmq::object_t::process_plug ()
{
    zmq_assert (false);
}

void zmq::object_t::process_own (own_t *object_)
{
    zmq_assert (false);
}

void zmq::object_t::process_attach (i_engine *engine_,
    const blob_t &peer_identity_)
{
    zmq_assert (false);
}

void zmq::object_t::process_bind (reader_t *in_pipe_, writer_t *out_pipe_,
    const blob_t &peer_identity_)
{
    zmq_assert (false);
}

void zmq::object_t::process_activate_reader ()
{
    zmq_assert (false);
}

void zmq::object_t::process_activate_writer (uint64_t msgs_read_)
{
    zmq_assert (false);
}

void zmq::object_t::process_pipe_term ()
{
    zmq_assert (false);
}

void zmq::object_t::process_pipe_term_ack ()
{
    zmq_assert (false);
}

void zmq::object_t::process_term_req (own_t *object_)
{
    zmq_assert (false);
}

void zmq::object_t::process_term (int linger_)
{
    zmq_assert (false);
}

void zmq::object_t::process_term_ack ()
{
    zmq_assert (false);
}

void zmq::object_t::process_reap (class socket_base_t *socket_)
{
    zmq_assert (false);
}

void zmq::object_t::process_reaped ()
{
    zmq_assert (false);
}

void zmq::object_t::process_seqnum ()
{
    zmq_assert (false);
}

void zmq::object_t::send_command (command_t &cmd_)
{
    ctx->send_command (cmd_.destination->get_tid (), cmd_);
}

