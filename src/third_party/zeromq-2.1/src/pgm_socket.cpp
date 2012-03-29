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

#ifdef ZMQ_HAVE_OPENPGM

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#endif

#ifdef ZMQ_HAVE_LINUX
#include <poll.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <string>

#include "options.hpp"
#include "pgm_socket.hpp"
#include "config.hpp"
#include "err.hpp"
#include "uuid.hpp"
#include "stdint.hpp"

#ifndef MSG_ERRQUEUE
#define MSG_ERRQUEUE 0x2000
#endif

zmq::pgm_socket_t::pgm_socket_t (bool receiver_, const options_t &options_) :
    sock (NULL),
    options (options_),
    receiver (receiver_),
    pgm_msgv (NULL),
    pgm_msgv_len (0),
    nbytes_rec (0),
    nbytes_processed (0),
    pgm_msgv_processed (0)
{
}

//  Create, bind and connect PGM socket.
//  network_ of the form <interface & multicast group decls>:<IP port>
//  e.g. eth0;239.192.0.1:7500
//       link-local;224.250.0.1,224.250.0.2;224.250.0.3:8000
//       ;[fe80::1%en0]:7500
int zmq::pgm_socket_t::init (bool udp_encapsulation_, const char *network_)
{
    //  Can not open transport before destroying old one. 
    zmq_assert (sock == NULL);
 
    //  Parse port number, start from end for IPv6
    const char *port_delim = strrchr (network_, ':');
    if (!port_delim) {
        errno = EINVAL;
        return -1;
    }

    uint16_t port_number = atoi (port_delim + 1);
  
    char network [256];
    if (port_delim - network_ >= (int) sizeof (network) - 1) {
        errno = EINVAL;
        return -1;
    }
    memset (network, '\0', sizeof (network));
    memcpy (network, network_, port_delim - network_);
   
    //  Validate socket options 
    //  Data rate is in [B/s]. options.rate is in [kb/s].
    if (options.rate <= 0) {
        errno = EINVAL;
        return -1;
    }
    //  Recovery interval [s] or [ms] - based on the user's call 
    if ((options.recovery_ivl <= 0) && (options.recovery_ivl_msec <= 0)) {
        errno = EINVAL;
        return -1;
    }

    //  Zero counter used in msgrecv.
    nbytes_rec = 0;
    nbytes_processed = 0;
    pgm_msgv_processed = 0;

    pgm_error_t *pgm_error = NULL;
    struct pgm_addrinfo_t hints, *res = NULL;
    sa_family_t sa_family;

    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_UNSPEC;
    if (!pgm_getaddrinfo (network, NULL, &res, &pgm_error)) {

        //  Invalid parameters don't set pgm_error_t.
        zmq_assert (pgm_error != NULL);
        if (pgm_error->domain == PGM_ERROR_DOMAIN_IF && (

              //  NB: cannot catch EAI_BADFLAGS.
              pgm_error->code != PGM_ERROR_SERVICE &&
              pgm_error->code != PGM_ERROR_SOCKTNOSUPPORT))

            //  User, host, or network configuration or transient error.
            goto err_abort;

        //  Fatal OpenPGM internal error.
        zmq_assert (false);
    }

    zmq_assert (res != NULL);

    //  Pick up detected IP family.
    sa_family = res->ai_send_addrs[0].gsr_group.ss_family;

    //  Create IP/PGM or UDP/PGM socket.
    if (udp_encapsulation_) {
        if (!pgm_socket (&sock, sa_family, SOCK_SEQPACKET, IPPROTO_UDP,
              &pgm_error)) {

            //  Invalid parameters don't set pgm_error_t.
            zmq_assert (pgm_error != NULL);
            if (pgm_error->domain == PGM_ERROR_DOMAIN_SOCKET && (
                  pgm_error->code != PGM_ERROR_BADF &&
                  pgm_error->code != PGM_ERROR_FAULT &&
                  pgm_error->code != PGM_ERROR_NOPROTOOPT &&
                  pgm_error->code != PGM_ERROR_FAILED))

                //  User, host, or network configuration or transient error.
                goto err_abort;

            //  Fatal OpenPGM internal error.
            zmq_assert (false);
        }

        //  All options are of data type int
        const int encapsulation_port = port_number;
        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_UDP_ENCAP_UCAST_PORT,
                &encapsulation_port, sizeof (encapsulation_port)))
            goto err_abort;
        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_UDP_ENCAP_MCAST_PORT,
                &encapsulation_port, sizeof (encapsulation_port)))
            goto err_abort;
    }
    else {
        if (!pgm_socket (&sock, sa_family, SOCK_SEQPACKET, IPPROTO_PGM,
              &pgm_error)) {

            //  Invalid parameters don't set pgm_error_t.
            zmq_assert (pgm_error != NULL);
            if (pgm_error->domain == PGM_ERROR_DOMAIN_SOCKET && (
                  pgm_error->code != PGM_ERROR_BADF &&
                  pgm_error->code != PGM_ERROR_FAULT &&
                  pgm_error->code != PGM_ERROR_NOPROTOOPT &&
                  pgm_error->code != PGM_ERROR_FAILED))

                //  User, host, or network configuration or transient error.
                goto err_abort;

            //  Fatal OpenPGM internal error.
            zmq_assert (false);
        }
    }

    {
        const int rcvbuf = (int) options.rcvbuf,
                  sndbuf = (int) options.sndbuf,
                  max_tpdu = (int) pgm_max_tpdu;
        if (rcvbuf) {
            if (!pgm_setsockopt (sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                  sizeof (rcvbuf)))
                goto err_abort;
        }
        if (sndbuf) {
            if (!pgm_setsockopt (sock, SOL_SOCKET, SO_SNDBUF, &sndbuf,
                  sizeof (sndbuf)))
                goto err_abort;
        }

        //  Set maximum transport protocol data unit size (TPDU).
        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_MTU, &max_tpdu,
              sizeof (max_tpdu)))
            goto err_abort;
    }

    if (receiver) {
        const int recv_only        = 1,
                  rxw_max_tpdu     = (int) pgm_max_tpdu,
                  rxw_sqns         = compute_sqns (rxw_max_tpdu),
                  peer_expiry      = pgm_secs (300),
                  spmr_expiry      = pgm_msecs (25),
                  nak_bo_ivl       = pgm_msecs (50),
                  nak_rpt_ivl      = pgm_msecs (200),
                  nak_rdata_ivl    = pgm_msecs (200),
                  nak_data_retries = 50,
                  nak_ncf_retries  = 50;

        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_RECV_ONLY, &recv_only,
                sizeof (recv_only)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_RXW_SQNS, &rxw_sqns,
                sizeof (rxw_sqns)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_PEER_EXPIRY, &peer_expiry,
                sizeof (peer_expiry)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_SPMR_EXPIRY, &spmr_expiry,
                sizeof (spmr_expiry)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_NAK_BO_IVL, &nak_bo_ivl,
                sizeof (nak_bo_ivl)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_NAK_RPT_IVL, &nak_rpt_ivl,
                sizeof (nak_rpt_ivl)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_NAK_RDATA_IVL,
                &nak_rdata_ivl, sizeof (nak_rdata_ivl)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_NAK_DATA_RETRIES,
                &nak_data_retries, sizeof (nak_data_retries)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_NAK_NCF_RETRIES,
                &nak_ncf_retries, sizeof (nak_ncf_retries)))
            goto err_abort;
    } else {
        const int send_only        = 1,
                  max_rte          = (int) ((options.rate * 1000) / 8),
                  txw_max_tpdu     = (int) pgm_max_tpdu,
                  txw_sqns         = compute_sqns (txw_max_tpdu),
                  ambient_spm      = pgm_secs (30),
                  heartbeat_spm[]  = { pgm_msecs (100),
                                       pgm_msecs (100),
                                       pgm_msecs (100),
                                       pgm_msecs (100),
                                       pgm_msecs (1300),
                                       pgm_secs  (7),
                                       pgm_secs  (16),
                                       pgm_secs  (25),
                                       pgm_secs  (30) };

        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_SEND_ONLY,
                &send_only, sizeof (send_only)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_ODATA_MAX_RTE,
                &max_rte, sizeof (max_rte)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_TXW_SQNS,
                &txw_sqns, sizeof (txw_sqns)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_AMBIENT_SPM,
                &ambient_spm, sizeof (ambient_spm)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_HEARTBEAT_SPM,
                &heartbeat_spm, sizeof (heartbeat_spm)))
            goto err_abort;
    }

    //  PGM transport GSI.
    struct pgm_sockaddr_t addr;

    memset (&addr, 0, sizeof(addr));
    addr.sa_port = port_number;
    addr.sa_addr.sport = DEFAULT_DATA_SOURCE_PORT;

    if (options.identity.size () > 0) {

        //  Create gsi from identity.
        if (!pgm_gsi_create_from_data (&addr.sa_addr.gsi,
              options.identity.data (), options.identity.size ()))
            goto err_abort;
    } else {

        //  Generate random gsi.
        std::string gsi_base = uuid_t ().to_string ();
        if (!pgm_gsi_create_from_string (&addr.sa_addr.gsi,
              gsi_base.c_str (), -1))
            goto err_abort;
    }

    //  Bind a transport to the specified network devices.
    struct pgm_interface_req_t if_req;
    memset (&if_req, 0, sizeof(if_req));
    if_req.ir_interface = res->ai_recv_addrs[0].gsr_interface;
    if_req.ir_scope_id  = 0;
    if (AF_INET6 == sa_family) {
        struct sockaddr_in6 sa6;
        memcpy (&sa6, &res->ai_recv_addrs[0].gsr_group, sizeof (sa6));
        if_req.ir_scope_id = sa6.sin6_scope_id;
    }
    if (!pgm_bind3 (sock, &addr, sizeof (addr), &if_req, sizeof (if_req),
          &if_req, sizeof (if_req), &pgm_error)) {

        //  Invalid parameters don't set pgm_error_t.
        zmq_assert (pgm_error != NULL);
        if ((pgm_error->domain == PGM_ERROR_DOMAIN_SOCKET ||
             pgm_error->domain == PGM_ERROR_DOMAIN_IF) && (
             pgm_error->code != PGM_ERROR_INVAL &&
             pgm_error->code != PGM_ERROR_BADF &&
             pgm_error->code != PGM_ERROR_FAULT))

            //  User, host, or network configuration or transient error.
            goto err_abort;

        //  Fatal OpenPGM internal error.
        zmq_assert (false);
    }

    //  Join IP multicast groups.
    for (unsigned i = 0; i < res->ai_recv_addrs_len; i++) {
        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_JOIN_GROUP,
              &res->ai_recv_addrs [i], sizeof (struct group_req)))
            goto err_abort;
    }
    if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_SEND_GROUP,
          &res->ai_send_addrs [0], sizeof (struct group_req)))
        goto err_abort;

    pgm_freeaddrinfo (res);
    res = NULL;

    //  Set IP level parameters.
    {
        const int nonblocking      = 1,
                  multicast_loop   = options.use_multicast_loop ? 1 : 0,
                  multicast_hops   = 16,

                  //  Expedited Forwarding PHB for network elements, no ECN.
                  dscp             = 0x2e << 2; 

        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_MULTICAST_LOOP,
                &multicast_loop, sizeof (multicast_loop)) ||
            !pgm_setsockopt (sock, IPPROTO_PGM, PGM_MULTICAST_HOPS,
                &multicast_hops, sizeof (multicast_hops)))
            goto err_abort;
        if (AF_INET6 != sa_family && !pgm_setsockopt (sock,
              IPPROTO_PGM, PGM_TOS, &dscp, sizeof (dscp)))
            goto err_abort;
        if (!pgm_setsockopt (sock, IPPROTO_PGM, PGM_NOBLOCK,
              &nonblocking, sizeof (nonblocking)))
            goto err_abort;
    }

    //  Connect PGM transport to start state machine.
    if (!pgm_connect (sock, &pgm_error)) {

        //  Invalid parameters don't set pgm_error_t.
        zmq_assert (pgm_error != NULL);
        goto err_abort;
    }

    //  For receiver transport preallocate pgm_msgv array.
    if (receiver) {
        zmq_assert (in_batch_size > 0);
        size_t max_tsdu_size = get_max_tsdu_size ();
        pgm_msgv_len = (int) in_batch_size / max_tsdu_size;
        if ((int) in_batch_size % max_tsdu_size)
            pgm_msgv_len++;
        zmq_assert (pgm_msgv_len);

        pgm_msgv = (pgm_msgv_t*) malloc (sizeof (pgm_msgv_t) * pgm_msgv_len);
        alloc_assert (pgm_msgv);
    }

    return 0;

err_abort:
    if (sock != NULL) {
        pgm_close (sock, FALSE);
        sock = NULL;
    }
    if (res != NULL) {
        pgm_freeaddrinfo (res);
        res = NULL;
    }
    if (pgm_error != NULL) {
        pgm_error_free (pgm_error);
        pgm_error = NULL;
    }
    errno = EINVAL;
    return -1;
}

zmq::pgm_socket_t::~pgm_socket_t ()
{
    if (pgm_msgv)
        free (pgm_msgv);
    if (sock) 
        pgm_close (sock, TRUE);
}

//  Get receiver fds. receive_fd_ is signaled for incoming packets,
//  waiting_pipe_fd_ is signaled for state driven events and data.
void zmq::pgm_socket_t::get_receiver_fds (fd_t *receive_fd_, 
    fd_t *waiting_pipe_fd_)
{
    socklen_t socklen;
    bool rc;

    zmq_assert (receive_fd_);
    zmq_assert (waiting_pipe_fd_);

    socklen = sizeof (*receive_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_RECV_SOCK, receive_fd_,
        &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*receive_fd_));

    socklen = sizeof (*waiting_pipe_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_PENDING_SOCK, waiting_pipe_fd_,
        &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*waiting_pipe_fd_));
}

//  Get fds and store them into user allocated memory. 
//  send_fd is for non-blocking send wire notifications.
//  receive_fd_ is for incoming back-channel protocol packets.
//  rdata_notify_fd_ is raised for waiting repair transmissions.
//  pending_notify_fd_ is for state driven events.
void zmq::pgm_socket_t::get_sender_fds (fd_t *send_fd_, fd_t *receive_fd_, 
    fd_t *rdata_notify_fd_, fd_t *pending_notify_fd_)
{
    socklen_t socklen;
    bool rc;

    zmq_assert (send_fd_);
    zmq_assert (receive_fd_);
    zmq_assert (rdata_notify_fd_);
    zmq_assert (pending_notify_fd_);

    socklen = sizeof (*send_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_SEND_SOCK, send_fd_, &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*receive_fd_));

    socklen = sizeof (*receive_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_RECV_SOCK, receive_fd_,
        &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*receive_fd_));

    socklen = sizeof (*rdata_notify_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_REPAIR_SOCK, rdata_notify_fd_,
        &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*rdata_notify_fd_));

    socklen = sizeof (*pending_notify_fd_);
    rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_PENDING_SOCK,
        pending_notify_fd_, &socklen);
    zmq_assert (rc);
    zmq_assert (socklen == sizeof (*pending_notify_fd_));
}

//  Send one APDU, transmit window owned memory.
//  data_len_ must be less than one TPDU.
size_t zmq::pgm_socket_t::send (unsigned char *data_, size_t data_len_)
{
    size_t nbytes = 0;
   
    const int status = pgm_send (sock, data_, data_len_, &nbytes);

    //  We have to write all data as one packet.
    if (nbytes > 0) {
        zmq_assert (status == PGM_IO_STATUS_NORMAL);
        zmq_assert ((ssize_t) nbytes == (ssize_t) data_len_);
    } else {
        zmq_assert (status == PGM_IO_STATUS_RATE_LIMITED ||
            status == PGM_IO_STATUS_WOULD_BLOCK);

        if (status == PGM_IO_STATUS_RATE_LIMITED)
            errno = ENOMEM;
        else
            errno = EBUSY;
    }

    //  Save return value.
    last_tx_status = status;

    return nbytes;
}

long zmq::pgm_socket_t::get_rx_timeout ()
{
    if (last_rx_status != PGM_IO_STATUS_RATE_LIMITED &&
          last_rx_status != PGM_IO_STATUS_TIMER_PENDING)
        return -1;

    struct timeval tv;
    socklen_t optlen = sizeof (tv);
    const bool rc = pgm_getsockopt (sock, IPPROTO_PGM,
        last_rx_status == PGM_IO_STATUS_RATE_LIMITED ? PGM_RATE_REMAIN :
        PGM_TIME_REMAIN, &tv, &optlen);
    zmq_assert (rc);

    const long timeout = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);

    return timeout;
}

long zmq::pgm_socket_t::get_tx_timeout ()
{
    if (last_tx_status != PGM_IO_STATUS_RATE_LIMITED)
        return -1;

    struct timeval tv;
    socklen_t optlen = sizeof (tv);
    const bool rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_RATE_REMAIN, &tv,
        &optlen);
    zmq_assert (rc);

    const long timeout = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);

    return timeout;
}

//  Return max TSDU size without fragmentation from current PGM transport.
size_t zmq::pgm_socket_t::get_max_tsdu_size ()
{
    int max_tsdu = 0;
    socklen_t optlen = sizeof (max_tsdu);

    bool rc = pgm_getsockopt (sock, IPPROTO_PGM, PGM_MSS, &max_tsdu, &optlen);
    zmq_assert (rc);
    zmq_assert (optlen == sizeof (max_tsdu));
    return (size_t) max_tsdu;
}

//  pgm_recvmsgv is called to fill the pgm_msgv array up to  pgm_msgv_len.
//  In subsequent calls data from pgm_msgv structure are returned.
ssize_t zmq::pgm_socket_t::receive (void **raw_data_, const pgm_tsi_t **tsi_)
{
    size_t raw_data_len = 0;

    //  We just sent all data from pgm_transport_recvmsgv up 
    //  and have to return 0 that another engine in this thread is scheduled.
    if (nbytes_rec == nbytes_processed && nbytes_rec > 0) {

        //  Reset all the counters.
        nbytes_rec = 0;
        nbytes_processed = 0;
        pgm_msgv_processed = 0;
        errno = EAGAIN;
        return 0;
    }

    //  If we have are going first time or if we have processed all pgm_msgv_t
    //  structure previously read from the pgm socket.
    if (nbytes_rec == nbytes_processed) {

        //  Check program flow.
        zmq_assert (pgm_msgv_processed == 0);
        zmq_assert (nbytes_processed == 0);
        zmq_assert (nbytes_rec == 0);

        //  Receive a vector of Application Protocol Domain Unit's (APDUs) 
        //  from the transport.
        pgm_error_t *pgm_error = NULL;

        const int status = pgm_recvmsgv (sock, pgm_msgv,
            pgm_msgv_len, MSG_ERRQUEUE, &nbytes_rec, &pgm_error);

        //  Invalid parameters.
        zmq_assert (status != PGM_IO_STATUS_ERROR);

        last_rx_status = status;

        //  In a case when no ODATA/RDATA fired POLLIN event (SPM...)
        //  pgm_recvmsg returns PGM_IO_STATUS_TIMER_PENDING.
        if (status == PGM_IO_STATUS_TIMER_PENDING) {

            zmq_assert (nbytes_rec == 0);

            //  In case if no RDATA/ODATA caused POLLIN 0 is 
            //  returned.
            nbytes_rec = 0;
            errno = EBUSY;
            return 0;
        }

        //  Send SPMR, NAK, ACK is rate limited.
        if (status == PGM_IO_STATUS_RATE_LIMITED) {

            zmq_assert (nbytes_rec == 0);

            //  In case if no RDATA/ODATA caused POLLIN 0 is returned.
            nbytes_rec = 0;
            errno = ENOMEM;
            return 0;
        }

        //  No peers and hence no incoming packets.
        if (status == PGM_IO_STATUS_WOULD_BLOCK) {

            zmq_assert (nbytes_rec == 0);

            //  In case if no RDATA/ODATA caused POLLIN 0 is returned.
            nbytes_rec = 0;
            errno = EAGAIN;
            return 0;
        }

        //  Data loss.
        if (status == PGM_IO_STATUS_RESET) {

            struct pgm_sk_buff_t* skb = pgm_msgv [0].msgv_skb [0];

            //  Save lost data TSI.
            *tsi_ = &skb->tsi;
            nbytes_rec = 0;

            //  In case of dala loss -1 is returned.
            errno = EINVAL;
            pgm_free_skb (skb);
            return -1;
        }

        zmq_assert (status == PGM_IO_STATUS_NORMAL);
    }
    else
    {
        zmq_assert (pgm_msgv_processed <= pgm_msgv_len);
    }

    // Zero byte payloads are valid in PGM, but not 0MQ protocol.
    zmq_assert (nbytes_rec > 0);

    // Only one APDU per pgm_msgv_t structure is allowed.
    zmq_assert (pgm_msgv [pgm_msgv_processed].msgv_len == 1);
 
    struct pgm_sk_buff_t* skb = 
        pgm_msgv [pgm_msgv_processed].msgv_skb [0];

    //  Take pointers from pgm_msgv_t structure.
    *raw_data_ = skb->data;
    raw_data_len = skb->len;

    //  Save current TSI.
    *tsi_ = &skb->tsi;

    //  Move the the next pgm_msgv_t structure.
    pgm_msgv_processed++;
    zmq_assert (pgm_msgv_processed <= pgm_msgv_len);
    nbytes_processed +=raw_data_len;

    return raw_data_len;
}

void zmq::pgm_socket_t::process_upstream ()
{
    pgm_msgv_t dummy_msg;

    size_t dummy_bytes = 0;
    pgm_error_t *pgm_error = NULL;

    const int status = pgm_recvmsgv (sock, &dummy_msg,
        1, MSG_ERRQUEUE, &dummy_bytes, &pgm_error);

    //  Invalid parameters.
    zmq_assert (status != PGM_IO_STATUS_ERROR);

    //  No data should be returned.
    zmq_assert (dummy_bytes == 0 && (status == PGM_IO_STATUS_TIMER_PENDING || 
        status == PGM_IO_STATUS_RATE_LIMITED ||
        status == PGM_IO_STATUS_WOULD_BLOCK));

    last_rx_status = status;

    if (status == PGM_IO_STATUS_TIMER_PENDING)
        errno = EBUSY;
    else if (status == PGM_IO_STATUS_RATE_LIMITED)
        errno = ENOMEM;
    else
        errno = EAGAIN;
}

int zmq::pgm_socket_t::compute_sqns (int tpdu_)
{
    //  Convert rate into B/ms.
    uint64_t rate = ((uint64_t) options.rate) / 8;

    //  Get recovery interval in milliseconds.
    uint64_t interval = options.recovery_ivl_msec >= 0 ?
        options.recovery_ivl_msec :
        options.recovery_ivl * 1000;
        
    //  Compute the size of the buffer in bytes.
    uint64_t size = interval * rate;

    //  Translate the size into number of packets.
    uint64_t sqns = size / tpdu_;

    //  Buffer should be able to contain at least one packet.
    if (sqns == 0)
        sqns = 1;

    return (int) sqns;
}

#endif

