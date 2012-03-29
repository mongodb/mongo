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

#include "platform.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#include <io.h>
#else
#include <unistd.h>
#endif

#include "../include/zmq.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sstream>
#include <algorithm>

#include "swap.hpp"
#include "config.hpp"
#include "atomic_counter.hpp"
#include "err.hpp"

zmq::swap_t::swap_t (int64_t filesize_) :
    fd (-1),
    filesize (filesize_),
    file_pos (0),
    write_pos (0),
    read_pos (0),
    block_size (swap_block_size),
    write_buf_start_addr (0)
{
    zmq_assert (filesize > 0);
    zmq_assert (block_size > 0);

    buf1 = new (std::nothrow) char [block_size];
    alloc_assert (buf1);

    buf2 = new (std::nothrow) char [block_size];
    alloc_assert (buf2);

    read_buf = write_buf = buf1;
}

zmq::swap_t::~swap_t ()
{
    delete [] buf1;
    delete [] buf2;

    if (fd == -1)
        return;

#ifdef ZMQ_HAVE_WINDOWS
    int rc = _close (fd);
#else
    int rc = close (fd);
#endif
    errno_assert (rc == 0);

#ifdef ZMQ_HAVE_WINDOWS
    rc = _unlink (filename.c_str ());
#else
    rc = unlink (filename.c_str ());
#endif
    errno_assert (rc == 0);
}

int zmq::swap_t::init ()
{
    static zmq::atomic_counter_t seqnum (0);

    //  Get process ID.
#ifdef ZMQ_HAVE_WINDOWS
    int pid = GetCurrentThreadId ();
#else
    pid_t pid = getpid ();
#endif

    std::ostringstream outs;
    outs << "zmq_" << pid << '_' << seqnum.get () << ".swap";
    filename = outs.str ();

    seqnum.add (1);

    //  Open the backing file.
#ifdef ZMQ_HAVE_WINDOWS
    fd = _open (filename.c_str (), _O_RDWR | _O_CREAT, 0600);
#else
    fd = open (filename.c_str (), O_RDWR | O_CREAT, 0600);
#endif
    if (fd == -1)
        return -1;

#if (defined (ZMQ_HAVE_LINUX) && !defined (ZMQ_HAVE_ANDROID))
    //  Enable more aggresive read-ahead optimization.
    posix_fadvise (fd, 0, filesize, POSIX_FADV_SEQUENTIAL);
#endif
    return 0;
}

bool zmq::swap_t::store (zmq_msg_t *msg_)
{
    size_t msg_size = zmq_msg_size (msg_);

    //  Check buffer space availability.
    //  NOTE: We always keep one byte open.
    if (buffer_space () <= (int64_t) (sizeof msg_size + 1 + msg_size))
        return false;

    //  Don't store the ZMQ_MSG_SHARED flag.
    uint8_t msg_flags = msg_->flags & ~ZMQ_MSG_SHARED;

    //  Write message length, flags, and message body.
    copy_to_file (&msg_size, sizeof msg_size);
    copy_to_file (&msg_flags, sizeof msg_flags);
    copy_to_file (zmq_msg_data (msg_), msg_size);

    return true;
}

void zmq::swap_t::fetch (zmq_msg_t *msg_)
{
    //  There must be at least one message available.
    zmq_assert (read_pos != write_pos);

    //  Retrieve the message size.
    size_t msg_size;
    copy_from_file (&msg_size, sizeof msg_size);

    //  Initialize the message.
    zmq_msg_init_size (msg_, msg_size);

    //  Retrieve the message flags.
    copy_from_file (&msg_->flags, sizeof msg_->flags);

    //  Retrieve the message payload.
    copy_from_file (zmq_msg_data (msg_), msg_size);
}

void zmq::swap_t::commit ()
{
    commit_pos = write_pos;
}

void zmq::swap_t::rollback ()
{
    if (commit_pos == write_pos || read_pos == write_pos)
        return;

    if (write_pos > read_pos)
        zmq_assert (read_pos <= commit_pos && commit_pos <= write_pos);
    else
        zmq_assert (read_pos <= commit_pos || commit_pos <= write_pos);

    if (commit_pos / block_size == read_pos / block_size) {
        write_buf_start_addr = commit_pos % block_size;
        write_buf = read_buf;
    }
    else if (commit_pos / block_size != write_pos / block_size) {
        write_buf_start_addr = commit_pos % block_size;
        fill_buf (write_buf, write_buf_start_addr);
    }
    write_pos = commit_pos;
}

bool zmq::swap_t::empty ()
{
    return read_pos == write_pos;
}

/*
bool zmq::swap_t::full ()
{
    //  Check that at least the message size can be written to the swap.
    return buffer_space () < (int64_t) (sizeof (size_t) + 1);
}
*/

bool zmq::swap_t::fits (zmq_msg_t *msg_)
{
    //  Check whether whole binary representation of the message
    //  fits into the swap.
    size_t msg_size = zmq_msg_size (msg_);
    if (buffer_space () <= (int64_t) (sizeof msg_size + 1 + msg_size))
        return false;
    return true;
 }

void zmq::swap_t::copy_from_file (void *buffer_, size_t count_)
{
    char *dest_ptr = (char *) buffer_;
    size_t chunk_size, remainder = count_;

    while (remainder > 0) {
        chunk_size = std::min (remainder,
            std::min ((size_t) (filesize - read_pos),
            (size_t) (block_size - read_pos % block_size)));

        memcpy (dest_ptr, &read_buf [read_pos % block_size], chunk_size);
        dest_ptr += chunk_size;

        read_pos = (read_pos + chunk_size) % filesize;
        if (read_pos % block_size == 0) {
            if (read_pos / block_size == write_pos / block_size)
                read_buf = write_buf;
            else
                fill_buf (read_buf, read_pos);
        }
        remainder -= chunk_size;
    }
}

void zmq::swap_t::copy_to_file (const void *buffer_, size_t count_)
{
    char *source_ptr = (char *) buffer_;
    size_t chunk_size, remainder = count_;

    while (remainder > 0) {
        chunk_size = std::min (remainder,
            std::min ((size_t) (filesize - write_pos),
            (size_t) (block_size - write_pos % block_size)));

        memcpy (&write_buf [write_pos % block_size], source_ptr, chunk_size);
        source_ptr += chunk_size;

        write_pos = (write_pos + chunk_size) % filesize;
        if (write_pos % block_size == 0) {
            save_write_buf ();
            write_buf_start_addr = write_pos;

            if (write_buf == read_buf) {
                if (read_buf == buf2)
                    write_buf = buf1;
                else
                    write_buf = buf2;
            }
        }
        remainder -= chunk_size;
    }
}

void zmq::swap_t::fill_buf (char *buf, int64_t pos)
{
    if (file_pos != pos) {
#ifdef ZMQ_HAVE_WINDOWS
        __int64 offset = _lseeki64 (fd, pos, SEEK_SET);
#else
        off_t offset = lseek (fd, (off_t) pos, SEEK_SET);
#endif
        errno_assert (offset == pos);
        file_pos = pos;
    }
    size_t octets_stored = 0;
    size_t octets_total = std::min (block_size, (size_t) (filesize - file_pos));

    while (octets_stored < octets_total) {
#ifdef ZMQ_HAVE_WINDOWS
        int rc = _read (fd, &buf [octets_stored], octets_total - octets_stored);
#else
        ssize_t rc = read (fd, &buf [octets_stored],
            octets_total - octets_stored);
#endif
        errno_assert (rc > 0);
        octets_stored += rc;
    }
    file_pos += octets_total;
}

void zmq::swap_t::save_write_buf ()
{
    if (file_pos != write_buf_start_addr) {
#ifdef ZMQ_HAVE_WINDOWS
        __int64 offset = _lseeki64 (fd, write_buf_start_addr, SEEK_SET);
#else
        off_t offset = lseek (fd, (off_t) write_buf_start_addr, SEEK_SET);
#endif
        errno_assert (offset == write_buf_start_addr);
        file_pos = write_buf_start_addr;
    }
    size_t octets_stored = 0;
    size_t octets_total = std::min (block_size, (size_t) (filesize - file_pos));

    while (octets_stored < octets_total) {
#ifdef ZMQ_HAVE_WINDOWS
        int rc = _write (fd, &write_buf [octets_stored],
            octets_total - octets_stored);
#else
        ssize_t rc = write (fd, &write_buf [octets_stored],
            octets_total - octets_stored);
#endif
        errno_assert (rc > 0);
        octets_stored += rc;
    }
    file_pos += octets_total;
}

int64_t zmq::swap_t::buffer_space ()
{
    if (write_pos < read_pos)
        return read_pos - write_pos;

    return filesize - (write_pos - read_pos);
}
