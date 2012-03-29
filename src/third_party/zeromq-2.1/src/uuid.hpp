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

#ifndef __ZMQ_UUID_HPP_INCLUDED__
#define __ZMQ_UUID_HPP_INCLUDED__

#include "platform.hpp"
#include "stdint.hpp"

#if defined ZMQ_HAVE_FREEBSD || defined ZMQ_HAVE_NETBSD
#include <uuid.h>
#elif defined ZMQ_HAVE_HPUX && defined HAVE_LIBDCEKT
#include <dce/uuid.h>
#elif defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_SOLARIS ||\
      defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_CYGWIN
#include <uuid/uuid.h>
#elif defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#include <rpc.h>
#elif defined ZMQ_HAVE_OPENVMS
typedef struct
{
    unsigned long data0;
    unsigned short data1;
    unsigned short data2;
    unsigned char data3 [8];
} uuid_t;
#endif

namespace zmq
{

    //  This class provides RFC 4122 (a Universally Unique IDentifier)
    //  implementation.

    class uuid_t
    {
    public:

        uuid_t ();
        ~uuid_t ();

        //  The length of textual representation of UUID.
        enum { uuid_string_len = 36 };

        //  Returns a pointer to buffer containing the textual
        //  representation of the UUID. The callee is reponsible to
        //  free the allocated memory.
        const char *to_string ();

        //  The length of binary representation of UUID.
        enum { uuid_blob_len = 16 };

        const unsigned char *to_blob ();

    private:

        //  Converts one byte from hexa representation to binary.
        unsigned char convert_byte (const char *hexa_);

        //  Converts string representation of UUID into standardised BLOB.
        //  The function is endianness agnostic.
        void create_blob ();

#if defined ZMQ_HAVE_WINDOWS
#ifdef ZMQ_HAVE_MINGW32
        typedef unsigned char* RPC_CSTR;
#endif
        ::UUID uuid;
        RPC_CSTR string_buf;
#elif defined ZMQ_HAVE_FREEBSD || defined ZMQ_HAVE_NETBSD
        ::uuid_t uuid;
        char *string_buf;
#elif defined ZMQ_HAVE_HPUX && defined HAVE_LIBDCEKT
        ::uuid_t uuid;
        unsigned_char_t *string_buf;
#elif defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_SOLARIS ||\
      defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_CYGWIN ||\
      defined ZMQ_HAVE_OPENVMS
        ::uuid_t uuid;
        char string_buf [uuid_string_len + 1];
#else
        //  RFC 4122 UUID's fields
        uint32_t time_low;
        uint16_t time_mid;
        uint16_t time_hi_and_version;
        uint8_t clock_seq_hi_and_reserved;
        uint8_t clock_seq_low;
        uint8_t node [6];

        char string_buf [uuid_string_len + 1];
#endif

        unsigned char blob_buf [uuid_blob_len];
    };

}

#endif
