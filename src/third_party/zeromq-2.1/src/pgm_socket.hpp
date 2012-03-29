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

#ifndef __PGM_SOCKET_HPP_INCLUDED__
#define __PGM_SOCKET_HPP_INCLUDED__

#include "platform.hpp"

#if defined ZMQ_HAVE_OPENPGM

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#endif

#define __PGM_WININT_H__
#include <pgm/pgm.h>

#ifdef ZMQ_HAVE_OSX
#include <pgm/in.h>
#endif

#include "fd.hpp"
#include "options.hpp"

namespace zmq
{
    //  Encapsulates PGM socket.
    class pgm_socket_t
    {

    public:

        //  If receiver_ is true PGM transport is not generating SPM packets.
        pgm_socket_t (bool receiver_, const options_t &options_);

        //  Closes the transport.
        ~pgm_socket_t ();

        //  Initialize PGM network structures (GSI, GSRs).
        int init (bool udp_encapsulation_, const char *network_);
        
        //   Get receiver fds and store them into user allocated memory.
        void get_receiver_fds (fd_t *receive_fd_, fd_t *waiting_pipe_fd_);

        //   Get sender and receiver fds and store it to user allocated 
        //   memory. Receive fd is used to process NAKs from peers.
        void get_sender_fds (fd_t *send_fd_, fd_t *receive_fd_,
            fd_t *rdata_notify_fd_, fd_t *pending_notify_fd_);

        //  Send data as one APDU, transmit window owned memory.
        size_t send (unsigned char *data_, size_t data_len_);

        //  Returns max tsdu size without fragmentation.
        size_t get_max_tsdu_size ();

        //  Receive data from pgm socket.
        ssize_t receive (void **data_, const pgm_tsi_t **tsi_);

        long get_rx_timeout ();
        long get_tx_timeout ();

        //  POLLIN on sender side should mean NAK or SPMR receiving. 
        //  process_upstream function is used to handle such a situation.
        void process_upstream ();

    private:

        //  Compute size of the buffer based on rate and recovery interval.
        int compute_sqns (int tpdu_);
    
        //  OpenPGM transport.
        pgm_sock_t* sock;

        int last_rx_status, last_tx_status;

        //  Associated socket options.
        options_t options;
       
        //  true when pgm_socket should create receiving side.
        bool receiver;

        //  Array of pgm_msgv_t structures to store received data
        //  from the socket (pgm_transport_recvmsgv).
        pgm_msgv_t *pgm_msgv;

        //  Size of pgm_msgv array.
        size_t pgm_msgv_len;

        // How many bytes were read from pgm socket.
        size_t nbytes_rec;

        //  How many bytes were processed from last pgm socket read.
        size_t nbytes_processed;
        
        //  How many messages from pgm_msgv were already sent up.
        size_t pgm_msgv_processed;
    };
}
#endif

#endif

