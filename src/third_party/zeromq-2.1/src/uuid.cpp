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

#include "uuid.hpp"
#include "err.hpp"

#if defined ZMQ_HAVE_WINDOWS

zmq::uuid_t::uuid_t ()
{
    RPC_STATUS ret = UuidCreate (&uuid);
    zmq_assert (ret == RPC_S_OK);
    ret = UuidToString (&uuid, &string_buf);
    zmq_assert (ret == RPC_S_OK);

    create_blob ();
}

zmq::uuid_t::~uuid_t ()
{
    if (string_buf)
        RpcStringFree (&string_buf);
}

const char *zmq::uuid_t::to_string ()
{
    return (char*) string_buf;
}

#elif defined ZMQ_HAVE_FREEBSD || defined ZMQ_HAVE_NETBSD || (defined ZMQ_HAVE_HPUX && defined HAVE_LIBDCEKT)

#include <stdlib.h>
#ifdef ZMQ_HAVE_HPUX
#  include <dce/uuid.h>
#else
#  include <uuid.h>
#endif

zmq::uuid_t::uuid_t ()
{
#ifdef ZMQ_HAVE_HPUX
    unsigned32 status;
#else
    uint32_t status;
#endif
    uuid_create (&uuid, &status);
    zmq_assert (status == uuid_s_ok);
    uuid_to_string (&uuid, &string_buf, &status);
    zmq_assert (status == uuid_s_ok);

    create_blob ();
}

zmq::uuid_t::~uuid_t ()
{
    free (string_buf);
}

const char *zmq::uuid_t::to_string ()
{
    return (char*) string_buf;
}

#elif defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_SOLARIS ||\
      defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_CYGWIN

#include <uuid/uuid.h>

zmq::uuid_t::uuid_t ()
{
    uuid_generate (uuid);
    uuid_unparse (uuid, string_buf);

    create_blob ();
}

zmq::uuid_t::~uuid_t ()
{
}

const char *zmq::uuid_t::to_string ()
{
    return string_buf;
}

#elif defined ZMQ_HAVE_OPENVMS

#include <starlet.h>

#define uuid_generate(x) sys$create_uid(&(x))

#define uuid_unparse(x, y) \
        sprintf (y, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", \
                        x.data0, x.data1, x.data2, \
                        x.data3 [0], x.data3 [1], \
                        x.data3 [2], x.data3 [3], \
                        x.data3 [4], x.data3 [5], \
                        x.data3 [6], x.data3 [7]);

zmq::uuid_t::uuid_t ()
{
    uuid_generate (uuid);
    uuid_unparse (uuid, string_buf);
}

zmq::uuid_t::~uuid_t ()
{
}

const char *zmq::uuid_t::to_string ()
{
    return string_buf;
}

#else

#include <stdio.h>
#include <string.h>
#include <openssl/rand.h>

zmq::uuid_t::uuid_t ()
{
    unsigned char rand_buf [16];
    int ret = RAND_bytes (rand_buf, sizeof rand_buf);
    zmq_assert (ret == 1);

    //  Read in UUID fields.
    memcpy (&time_low, rand_buf, sizeof time_low);
    memcpy (&time_mid, rand_buf + 4, sizeof time_mid);
    memcpy (&time_hi_and_version, rand_buf + 6, sizeof time_hi_and_version);
    memcpy (&clock_seq_hi_and_reserved, rand_buf + 8,
        sizeof clock_seq_hi_and_reserved);
    memcpy (&clock_seq_low, rand_buf + 9, sizeof clock_seq_low);
    memcpy (&node [0], rand_buf + 10, sizeof node);

    //  Store UUID version number.
    time_hi_and_version = (time_hi_and_version & 0x0fff) | 4 << 12;

    //  Store UUID type.
    clock_seq_hi_and_reserved = (clock_seq_hi_and_reserved & 0x3f) | 0x80;

    snprintf (string_buf, sizeof string_buf,
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        time_low,
        time_mid,
        time_hi_and_version,
        clock_seq_hi_and_reserved,
        clock_seq_low,
        node [0], node [1], node [2], node [3], node [4], node [5]);

    create_blob ();
}

zmq::uuid_t::~uuid_t ()
{
}

const char *zmq::uuid_t::to_string ()
{
    return string_buf;
}

#endif

const unsigned char *zmq::uuid_t::to_blob ()
{
    return blob_buf;
}

unsigned char zmq::uuid_t::convert_byte (const char *hexa_)
{
    unsigned char byte;

    if (*hexa_ >= '0' && *hexa_ <= '9')
        byte = *hexa_ - '0';
    else if (*hexa_ >= 'A' && *hexa_ <= 'F')
        byte = *hexa_ - 'A' + 10;
    else if (*hexa_ >= 'a' && *hexa_ <= 'f')
        byte = *hexa_ - 'a' + 10;
    else {
        zmq_assert (false);
        byte = 0;
    }

    byte *= 16;

    hexa_++;
    if (*hexa_ >= '0' && *hexa_ <= '9')
        byte += *hexa_ - '0';
    else if (*hexa_ >= 'A' && *hexa_ <= 'F')
        byte += *hexa_ - 'A' + 10;
    else if (*hexa_ >= 'a' && *hexa_ <= 'f')
        byte += *hexa_ - 'a' + 10;
    else
        zmq_assert (false);  

    return byte;
}

void zmq::uuid_t::create_blob ()
{
    const char *buf = (const char*) string_buf;

    blob_buf [0] = convert_byte (buf + 0);
    blob_buf [1] = convert_byte (buf + 2);
    blob_buf [2] = convert_byte (buf + 4);
    blob_buf [3] = convert_byte (buf + 6);

    blob_buf [4] = convert_byte (buf + 9);
    blob_buf [5] = convert_byte (buf + 11);

    blob_buf [6] = convert_byte (buf + 14);
    blob_buf [7] = convert_byte (buf + 16);

    blob_buf [8] = convert_byte (buf + 19);
    blob_buf [9] = convert_byte (buf + 21);

    blob_buf [10] = convert_byte (buf + 24);
    blob_buf [11] = convert_byte (buf + 26);
    blob_buf [12] = convert_byte (buf + 28);
    blob_buf [13] = convert_byte (buf + 30);
    blob_buf [14] = convert_byte (buf + 32);
    blob_buf [15] = convert_byte (buf + 34);
}
