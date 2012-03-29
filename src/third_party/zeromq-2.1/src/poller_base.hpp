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

#ifndef __ZMQ_POLLER_BASE_HPP_INCLUDED__
#define __ZMQ_POLLER_BASE_HPP_INCLUDED__

#include <map>

#include "clock.hpp"
#include "atomic_counter.hpp"

namespace zmq
{

    class poller_base_t
    {
    public:

        poller_base_t ();
        virtual ~poller_base_t ();

        //  Returns load of the poller. Note that this function can be
        //  invoked from a different thread!
        int get_load ();

        //  Add a timeout to expire in timeout_ milliseconds. After the
        //  expiration timer_event on sink_ object will be called with
        //  argument set to id_.
        void add_timer (int timeout_, struct i_poll_events *sink_, int id_);

        //  Cancel the timer created by sink_ object with ID equal to id_.
        void cancel_timer (struct i_poll_events *sink_, int id_);

    protected:

        //  Called by individual poller implementations to manage the load.
        void adjust_load (int amount_);

        //  Executes any timers that are due. Returns number of milliseconds
        //  to wait to match the next timer or 0 meaning "no timers".
        uint64_t execute_timers ();

    private:

        //  Clock instance private to this I/O thread.
        clock_t clock;

        //  List of active timers.
        struct timer_info_t
        {
            struct i_poll_events *sink;
            int id;
        };
        typedef std::multimap <uint64_t, timer_info_t> timers_t;
        timers_t timers;

        //  Load of the poller. Currently the number of file descriptors
        //  registered.
        atomic_counter_t load;

        poller_base_t (const poller_base_t&);
        const poller_base_t &operator = (const poller_base_t&);
    };

}

#endif
