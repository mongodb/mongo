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

#ifndef __ZMQ_CTX_HPP_INCLUDED__
#define __ZMQ_CTX_HPP_INCLUDED__

#include <map>
#include <vector>
#include <string>
#include <stdarg.h>

#include "../include/zmq.h"

#include "mailbox.hpp"
#include "semaphore.hpp"
#include "ypipe.hpp"
#include "array.hpp"
#include "config.hpp"
#include "mutex.hpp"
#include "stdint.hpp"
#include "thread.hpp"
#include "options.hpp"

namespace zmq
{
    //  Information associated with inproc endpoint. Note that endpoint options
    //  are registered as well so that the peer can access them without a need
    //  for synchronisation, handshaking or similar.
    struct endpoint_t
    {
        class socket_base_t *socket;
        options_t options;
    };

    //  Context object encapsulates all the global state associated with
    //  the library.
    
    class ctx_t
    {
    public:

        //  Create the context object. The argument specifies the size
        //  of I/O thread pool to create.
        ctx_t (uint32_t io_threads_);

        //  Returns false if object is not a context.
        bool check_tag ();

        //  This function is called when user invokes zmq_term. If there are
        //  no more sockets open it'll cause all the infrastructure to be shut
        //  down. If there are open sockets still, the deallocation happens
        //  after the last one is closed.
        int terminate ();

        //  Create and destroy a socket.
        class socket_base_t *create_socket (int type_);
        void destroy_socket (class socket_base_t *socket_);

        //  Send command to the destination thread.
        void send_command (uint32_t tid_, const command_t &command_);

        //  Returns the I/O thread that is the least busy at the moment.
        //  Affinity specifies which I/O threads are eligible (0 = all).
        //  Returns NULL is no I/O thread is available.
        class io_thread_t *choose_io_thread (uint64_t affinity_);

        //  Returns reaper thread object.
        class object_t *get_reaper ();

        //  Management of inproc endpoints.
        int register_endpoint (const char *addr_, endpoint_t &endpoint_);
        void unregister_endpoints (class socket_base_t *socket_);
        endpoint_t find_endpoint (const char *addr_);

        //  Logging.
        void log (const char *format_, va_list args_);

        enum {
            term_tid = 0,
            reaper_tid = 1
        };

        ~ctx_t ();
    private:


        //  Used to check whether the object is a context.
        uint32_t tag;

        //  Sockets belonging to this context. We need the list so that
        //  we can notify the sockets when zmq_term() is called. The sockets
        //  will return ETERM then.
        typedef array_t <socket_base_t> sockets_t;
        sockets_t sockets;

        //  List of unused thread slots.
        typedef std::vector <uint32_t> emtpy_slots_t;
        emtpy_slots_t empty_slots;

        //  If true, zmq_term was already called.
        bool terminating;

        //  Synchronisation of accesses to global slot-related data:
        //  sockets, empty_slots, terminating. It also synchronises
        //  access to zombie sockets as such (as oposed to slots) and provides
        //  a memory barrier to ensure that all CPU cores see the same data.
        mutex_t slot_sync;

        //  The reaper thread.
        class reaper_t *reaper;

        //  I/O threads.
        typedef std::vector <class io_thread_t*> io_threads_t;
        io_threads_t io_threads;

        //  Array of pointers to mailboxes for both application and I/O threads.
        uint32_t slot_count;
        mailbox_t **slots;

        //  Mailbox for zmq_term thread.
        mailbox_t term_mailbox;

        //  List of inproc endpoints within this context.
        typedef std::map <std::string, endpoint_t> endpoints_t;
        endpoints_t endpoints;

        //  Synchronisation of access to the list of inproc endpoints.
        mutex_t endpoints_sync;

        //  PUB socket for logging. The socket is shared among all the threads,
        //  thus it is synchronised by a mutex.
        class socket_base_t *log_socket;
        mutex_t log_sync;

        ctx_t (const ctx_t&);
        const ctx_t &operator = (const ctx_t&);
    };
    
}

#endif

