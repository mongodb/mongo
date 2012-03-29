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

#ifndef __ZMQ_TEST_TESTUTIL_HPP_INCLUDED__
#define __ZMQ_TEST_TESTUTIL_HPP_INCLUDED__

#include <assert.h>
#include <iostream>
#include <string>
#include <utility>

#include "../include/zmq.hpp"

namespace zmqtestutil
{

    using namespace std ;

    typedef std::pair <zmq::socket_t*, zmq::socket_t*> socket_pair;

    //  Create a pair of sockets connected to each other.
    socket_pair create_bound_pair (zmq::context_t *context_,
        int t1_, int t2_, const char *transport_)
    {
        zmq::socket_t *s1 = new zmq::socket_t (*context_, t1_);
        zmq::socket_t *s2 = new zmq::socket_t (*context_, t2_);
        s1->bind (transport_);
        s2->connect (transport_);
        return socket_pair (s1, s2);
    }

    //  Send a message from one socket in the pair to the other and back.
    std::string ping_pong (const socket_pair &sp_, const std::string &orig_msg_)
    {
        zmq::socket_t &s1 = *sp_.first;
        zmq::socket_t &s2 = *sp_.second;

        //  Construct message to send.
        zmq::message_t ping (orig_msg_.size ());
        memcpy (ping.data (), orig_msg_.c_str (), orig_msg_.size ());

        //  Send ping out.
        s1.send (ping, 0);

        //  Get pong from connected socket.
        zmq::message_t pong;
        s2.recv (&pong, 0);

        //  Send message via s2, so state is clean in case of req/rep.
        std::string ret ((char*) pong.data(), pong.size ());
        s2.send (pong, 0);

        //  Return received data as std::string.
        return ret ;
    }

    /*  Run basic tests for the given transport.

        Basic tests are:
        * ping pong as defined above.
        * send receive where the receive is signalled by zmq::poll
    */
    void basic_tests (const char *transport_, int t1_, int t2_)
    {
        zmq::context_t context (1);

        zmq::pollitem_t items [2];
        socket_pair p = create_bound_pair (&context, t1_, t2_, transport_);

        //  First test simple ping pong.
        const string expect ("XXX");

        {
            const string returned = zmqtestutil::ping_pong (p, expect);
            assert (expect == returned);

            //  Adjust socket state so that poll shows only 1 pending message.
            zmq::message_t mx ;
            p.first->recv (&mx, 0);
        }

        {
            //  Now poll is used to singal that a message is ready to read.
            zmq::message_t m1 (expect.size ());
            memcpy (m1.data (), expect.c_str (), expect.size ());
            items [0].socket = *p.first;
            items [0].fd = 0;
            items [0].events = ZMQ_POLLIN;
            items [0].revents = 0;
            items [1].socket = *p.second;
            items [1].fd = 0;
            items [1].events = ZMQ_POLLIN;
            items [1].revents = 0;

            p.first->send (m1, 0);

            int rc = zmq::poll (&items [0], 2, -1);
            assert (rc == 1);
            assert ((items [1].revents & ZMQ_POLLIN) != 0);

            zmq::message_t m2;
            p.second->recv (&m2, 0);
            const string ret ((char*) m2.data (), m2.size ());
            assert (expect == ret);
        }

        //  Delete sockets.
        delete (p.first);
        delete (p.second);
    }
}

#endif
