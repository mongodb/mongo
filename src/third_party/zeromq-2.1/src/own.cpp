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

#include "own.hpp"
#include "err.hpp"
#include "io_thread.hpp"

zmq::own_t::own_t (class ctx_t *parent_, uint32_t tid_) :
    object_t (parent_, tid_),
    terminating (false),
    sent_seqnum (0),
    processed_seqnum (0),
    owner (NULL),
    term_acks (0)
{
}

zmq::own_t::own_t (io_thread_t *io_thread_, const options_t &options_) :
    object_t (io_thread_),
    options (options_),
    terminating (false),
    sent_seqnum (0),
    processed_seqnum (0),
    owner (NULL),
    term_acks (0)
{
}

zmq::own_t::~own_t ()
{
}

void zmq::own_t::set_owner (own_t *owner_)
{
    zmq_assert (!owner);
    owner = owner_;
}

void zmq::own_t::inc_seqnum ()
{
    //  This function may be called from a different thread!
    sent_seqnum.add (1);
}

void zmq::own_t::process_seqnum ()
{
    //  Catch up with counter of processed commands.
    processed_seqnum++;

    //  We may have catched up and still have pending terms acks.
    check_term_acks ();
}

void zmq::own_t::launch_child (own_t *object_)
{
    //  Specify the owner of the object.
    object_->set_owner (this);

    //  Plug the object into the I/O thread.
    send_plug (object_);

    //  Take ownership of the object.
    send_own (this, object_);
}

void zmq::own_t::launch_sibling (own_t *object_)
{
    //  At this point it is important that object is plugged in before its
    //  owner has a chance to terminate it. Thus, 'plug' command is sent before
    //  the 'own' command. Given that the mailbox preserves ordering of
    //  commands, 'term' command from the owner cannot make it to the object
    //  before the already written 'plug' command.

    //  Specify the owner of the object.
    object_->set_owner (owner);

    //  Plug the object into its I/O thread.
    send_plug (object_);

    //  Make parent own the object.
    send_own (owner, object_);
}

void zmq::own_t::process_term_req (own_t *object_)
{
    //  When shutting down we can ignore termination requests from owned
    //  objects. The termination request was already sent to the object.
    if (terminating)
        return;

    //  If I/O object is well and alive let's ask it to terminate.
    owned_t::iterator it = std::find (owned.begin (), owned.end (), object_);

    //  If not found, we assume that termination request was already sent to
    //  the object so we can safely ignore the request.
    if (it == owned.end ())
        return;

    owned.erase (it);
    register_term_acks (1);

    //  Note that this object is the root of the (partial shutdown) thus, its
    //  value of linger is used, rather than the value stored by the children.
    send_term (object_, options.linger);
}

void zmq::own_t::process_own (own_t *object_)
{
    //  If the object is already being shut down, new owned objects are
    //  immediately asked to terminate. Note that linger is set to zero.
    if (terminating) {
        register_term_acks (1);
        send_term (object_, 0);
        return;
    }

    //  Store the reference to the owned object.
    owned.insert (object_);
}

void zmq::own_t::terminate ()
{
    //  If termination is already underway, there's no point
    //  in starting it anew.
    if (terminating)
        return;

    //  As for the root of the ownership tree, there's noone to terminate it,
    //  so it has to terminate itself.
    if (!owner) {
        process_term (options.linger);
        return;
    }

    //  If I am an owned object, I'll ask my owner to terminate me.
    send_term_req (owner, this);
}

void zmq::own_t::process_term (int linger_)
{
    //  Double termination should never happen.
    zmq_assert (!terminating);

    //  Send termination request to all owned objects.
    for (owned_t::iterator it = owned.begin (); it != owned.end (); ++it)
        send_term (*it, linger_);
    register_term_acks (owned.size ());
    owned.clear ();

    //  Start termination process and check whether by chance we cannot
    //  terminate immediately.
    terminating = true;
    check_term_acks ();
}

void zmq::own_t::register_term_acks (int count_)
{
    term_acks += count_;
}

void zmq::own_t::unregister_term_ack ()
{
    zmq_assert (term_acks > 0);
    term_acks--;

    //  This may be a last ack we are waiting for before termination...
    check_term_acks (); 
}

void zmq::own_t::process_term_ack ()
{
    unregister_term_ack ();
}

void zmq::own_t::check_term_acks ()
{
    if (terminating && processed_seqnum == sent_seqnum.get () &&
          term_acks == 0) {

        //  Sanity check. There should be no active children at this point.
        zmq_assert (owned.empty ());

        //  The root object has nobody to confirm the termination to.
        //  Other nodes will confirm the termination to the owner.
        if (owner)
            send_term_ack (owner);

        //  Deallocate the resources.
        process_destroy ();
    }
}

void zmq::own_t::process_destroy ()
{
    delete this;
}

