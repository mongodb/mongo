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

#ifndef __ZMQ_HPP_INCLUDED__
#define __ZMQ_HPP_INCLUDED__

#include "zmq.h"

#include <cassert>
#include <cstring>
#include <exception>

// Detect whether the compiler supports C++11 rvalue references.
#if (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 2)) && defined(__GXX_EXPERIMENTAL_CXX0X__))
#   define ZMQ_HAS_RVALUE_REFS
#endif
#if (defined(__clang__))
#   if __has_feature(cxx_rvalue_references)
#       define ZMQ_HAS_RVALUE_REFS
#   endif
#endif
#if (defined(_MSC_VER) && (_MSC_VER >= 1600))
#   define ZMQ_HAS_RVALUE_REFS
#endif

// In order to prevent unused variable warnings when building in non-debug
// mode use this macro to make assertions.
#ifndef NDEBUG
#   define ZMQ_ASSERT(expression) assert(expression)
#else
#   define ZMQ_ASSERT(expression) (expression)
#endif

namespace zmq
{

    typedef zmq_free_fn free_fn;
    typedef zmq_pollitem_t pollitem_t;

    class error_t : public std::exception
    {
    public:

        error_t () : errnum (zmq_errno ()) {}

        virtual const char *what () const throw ()
        {
            return zmq_strerror (errnum);
        }

        int num () const
        {
            return errnum;
        }

    private:

        int errnum;
    };

    inline int poll (zmq_pollitem_t *items_, int nitems_, long timeout_ = -1)
    {
        int rc = zmq_poll (items_, nitems_, timeout_);
        if (rc < 0)
            throw error_t ();
        return rc;
    }

    inline void device (int device_, void * insocket_, void* outsocket_)
    {
        int rc = zmq_device (device_, insocket_, outsocket_);
        if (rc != 0)
            throw error_t ();
    }

    inline void version (int *major_, int *minor_, int *patch_)
    {
        zmq_version (major_, minor_, patch_);
    }

    class message_t : private zmq_msg_t
    {
        friend class socket_t;

    public:

        inline message_t ()
        {
            int rc = zmq_msg_init (this);
            if (rc != 0)
                throw error_t ();
        }

        inline message_t (size_t size_)
        {
            int rc = zmq_msg_init_size (this, size_);
            if (rc != 0)
                throw error_t ();
        }

        inline message_t (void *data_, size_t size_, free_fn *ffn_,
            void *hint_ = NULL)
        {
            int rc = zmq_msg_init_data (this, data_, size_, ffn_, hint_);
            if (rc != 0)
                throw error_t ();
        }

        inline ~message_t ()
        {
            int rc = zmq_msg_close (this);
            ZMQ_ASSERT (rc == 0);
        }

        inline void rebuild ()
        {
            int rc = zmq_msg_close (this);
            if (rc != 0)
                throw error_t ();
            rc = zmq_msg_init (this);
            if (rc != 0)
                throw error_t ();
        }

        inline void rebuild (size_t size_)
        {
            int rc = zmq_msg_close (this);
            if (rc != 0)
                throw error_t ();
            rc = zmq_msg_init_size (this, size_);
            if (rc != 0)
                throw error_t ();
        }

        inline void rebuild (void *data_, size_t size_, free_fn *ffn_,
            void *hint_ = NULL)
        {
            int rc = zmq_msg_close (this);
            if (rc != 0)
                throw error_t ();
            rc = zmq_msg_init_data (this, data_, size_, ffn_, hint_);
            if (rc != 0)
                throw error_t ();
        }

        inline void move (message_t *msg_)
        {
            int rc = zmq_msg_move (this, (zmq_msg_t*) msg_);
            if (rc != 0)
                throw error_t ();
        }

        inline void copy (message_t *msg_)
        {
            int rc = zmq_msg_copy (this, (zmq_msg_t*) msg_);
            if (rc != 0)
                throw error_t ();
        }

        inline void *data ()
        {
            return zmq_msg_data (this);
        }

        inline size_t size ()
        {
            return zmq_msg_size (this);
        }

    private:

        //  Disable implicit message copying, so that users won't use shared
        //  messages (less efficient) without being aware of the fact.
        message_t (const message_t&);
        void operator = (const message_t&);
    };

    class context_t
    {
        friend class socket_t;

    public:

        inline context_t (int io_threads_)
        {
            ptr = zmq_init (io_threads_);
            if (ptr == NULL)
                throw error_t ();
        }

#ifdef ZMQ_HAS_RVALUE_REFS
        inline context_t(context_t&& rhs) : ptr(rhs.ptr)
        {
            rhs.ptr = NULL;
        }
        inline context_t& operator=(context_t&& rhs)
        {
            std::swap(ptr, rhs.ptr);
            return *this;
        }
#endif

        inline ~context_t ()
        {
            if (ptr == NULL)
                return;
            int rc = zmq_term (ptr);
            ZMQ_ASSERT (rc == 0);
        }

        //  Be careful with this, it's probably only useful for
        //  using the C api together with an existing C++ api.
        //  Normally you should never need to use this.
        inline operator void* ()
        {
            return ptr;
        }

    private:

        void *ptr;

        context_t (const context_t&);
        void operator = (const context_t&);
    };

    class socket_t
    {
    public:

        inline socket_t (context_t &context_, int type_)
        {
            ptr = zmq_socket (context_.ptr, type_);
            if (ptr == NULL)
                throw error_t ();
        }

#ifdef ZMQ_HAS_RVALUE_REFS
        inline socket_t(socket_t&& rhs) : ptr(rhs.ptr)
        {
            rhs.ptr = NULL;
        }
        inline socket_t& operator=(socket_t&& rhs)
        {
            std::swap(ptr, rhs.ptr);
            return *this;
        }
#endif

        inline ~socket_t ()
        {
            close();
        }

        inline operator void* ()
        {
            return ptr;
        }

        inline void close()
        {
            if(ptr == NULL)
                // already closed
                return ;
            int rc = zmq_close (ptr);
            if (rc != 0)
                throw error_t ();
            ptr = 0 ;
        }

        inline void setsockopt (int option_, const void *optval_,
            size_t optvallen_)
        {
            int rc = zmq_setsockopt (ptr, option_, optval_, optvallen_);
            if (rc != 0)
                throw error_t ();
        }

        inline void getsockopt (int option_, void *optval_,
            size_t *optvallen_)
        {
            int rc = zmq_getsockopt (ptr, option_, optval_, optvallen_);
            if (rc != 0)
                throw error_t ();
        }

        inline void bind (const char *addr_)
        {
            int rc = zmq_bind (ptr, addr_);
            if (rc != 0)
                throw error_t ();
        }

        inline void connect (const char *addr_)
        {
            int rc = zmq_connect (ptr, addr_);
            if (rc != 0)
                throw error_t ();
        }

        inline bool send (message_t &msg_, int flags_ = 0)
        {
            int rc = zmq_send (ptr, &msg_, flags_);
            if (rc == 0)
                return true;
            if (rc == -1 && zmq_errno () == EAGAIN)
                return false;
            throw error_t ();
        }

        inline bool recv (message_t *msg_, int flags_ = 0)
        {
            int rc = zmq_recv (ptr, msg_, flags_);
            if (rc == 0)
                return true;
            if (rc == -1 && zmq_errno () == EAGAIN)
                return false;
            throw error_t ();
        }

    private:

        void *ptr;

        socket_t (const socket_t&);
        void operator = (const socket_t&);
    };

}

#endif
