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

#include "testutil.hpp"
#include "../src/stdint.hpp"

using namespace std;
using namespace zmqtestutil;

int main (int argc, char *argv [])
{
    uint64_t hwm = 5;
    int linger = 0;

    zmq::context_t context (1);
    zmq::socket_t s1 (context, ZMQ_PULL);
    zmq::socket_t s2 (context, ZMQ_PUSH);

    s2.setsockopt (ZMQ_LINGER, &linger, sizeof (int));
    s2.setsockopt (ZMQ_HWM, &hwm, sizeof (uint64_t));

    s1.bind ("tcp://127.0.0.1:5858");
    s2.connect ("tcp://127.0.0.1:5858");

    for (int i = 0; i < 10; i++)
    {
        zmq::message_t msg (sizeof ("test") - 1);
        memcpy (msg.data (), "test", sizeof ("test") - 1);

        bool sent = s2.send (msg, ZMQ_NOBLOCK);

        // Anything below HWM should be sent
        if (i < 5) {
            assert (sent);
        } else {
            assert (!sent && errno == EAGAIN);
        }
    }

    // There should be now 5 messages pending, consume one
    zmq::message_t msg;

    bool received = s1.recv (&msg, 0);
    assert (received);

    // Now it should be possible to send one more
    bool sent = s2.send (msg, 0);
    assert (sent);

	return 0;
}
