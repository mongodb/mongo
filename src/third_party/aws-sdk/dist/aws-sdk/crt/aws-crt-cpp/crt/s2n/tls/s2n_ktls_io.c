/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#if defined(__FreeBSD__) || defined(__APPLE__)
    /* https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/sys_socket.h.html
     * The POSIX standard does not define the CMSG_LEN and CMSG_SPACE macros. FreeBSD
     * and APPLE check and disable these macros if the _POSIX_C_SOURCE flag is set.
     *
     * Since s2n-tls already unsets the _POSIX_C_SOURCE in other files and is not
     * POSIX compliant, we continue the pattern here.
     */
    #undef _POSIX_C_SOURCE
#endif
#include <sys/socket.h>

#ifdef S2N_LINUX_SENDFILE
    #include <sys/sendfile.h>
#endif

#include "crypto/s2n_sequence.h"
#include "error/s2n_errno.h"
#include "tls/s2n_ktls.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_io.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_socket.h"

/* record_type is of type uint8_t */
#define S2N_KTLS_RECORD_TYPE_SIZE    (sizeof(uint8_t))
#define S2N_KTLS_CONTROL_BUFFER_SIZE (CMSG_SPACE(S2N_KTLS_RECORD_TYPE_SIZE))

#define S2N_MAX_STACK_IOVECS     16
#define S2N_MAX_STACK_IOVECS_MEM (S2N_MAX_STACK_IOVECS * sizeof(struct iovec))

/* Used to override sendmsg and recvmsg for testing. */
static ssize_t s2n_ktls_default_sendmsg(void *io_context, const struct msghdr *msg);
static ssize_t s2n_ktls_default_recvmsg(void *io_context, struct msghdr *msg);
s2n_ktls_sendmsg_fn s2n_sendmsg_fn = s2n_ktls_default_sendmsg;
s2n_ktls_recvmsg_fn s2n_recvmsg_fn = s2n_ktls_default_recvmsg;

S2N_RESULT s2n_ktls_set_sendmsg_cb(struct s2n_connection *conn, s2n_ktls_sendmsg_fn send_cb,
        void *send_ctx)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(send_ctx);
    RESULT_ENSURE(s2n_in_test(), S2N_ERR_NOT_IN_TEST);
    conn->send_io_context = send_ctx;
    s2n_sendmsg_fn = send_cb;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ktls_set_recvmsg_cb(struct s2n_connection *conn, s2n_ktls_recvmsg_fn recv_cb,
        void *recv_ctx)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(recv_ctx);
    RESULT_ENSURE(s2n_in_test(), S2N_ERR_NOT_IN_TEST);
    conn->recv_io_context = recv_ctx;
    s2n_recvmsg_fn = recv_cb;
    return S2N_RESULT_OK;
}

static ssize_t s2n_ktls_default_recvmsg(void *io_context, struct msghdr *msg)
{
    POSIX_ENSURE_REF(io_context);
    POSIX_ENSURE_REF(msg);

    const struct s2n_socket_read_io_context *peer_socket_ctx = io_context;
    POSIX_ENSURE_REF(peer_socket_ctx);
    int fd = peer_socket_ctx->fd;

    return recvmsg(fd, msg, 0);
}

static ssize_t s2n_ktls_default_sendmsg(void *io_context, const struct msghdr *msg)
{
    POSIX_ENSURE_REF(io_context);
    POSIX_ENSURE_REF(msg);

    const struct s2n_socket_write_io_context *peer_socket_ctx = io_context;
    POSIX_ENSURE_REF(peer_socket_ctx);
    int fd = peer_socket_ctx->fd;

    return sendmsg(fd, msg, 0);
}

S2N_RESULT s2n_ktls_set_control_data(struct msghdr *msg, char *buf, size_t buf_size,
        int cmsg_type, uint8_t record_type)
{
    RESULT_ENSURE_REF(msg);
    RESULT_ENSURE_REF(buf);

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * To create ancillary data, first initialize the msg_controllen
     * member of the msghdr with the length of the control message
     * buffer.
     */
    msg->msg_control = buf;
    msg->msg_controllen = buf_size;

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * Use CMSG_FIRSTHDR() on the msghdr to get the first
     * control message and CMSG_NXTHDR() to get all subsequent ones.
     */
    struct cmsghdr *hdr = CMSG_FIRSTHDR(msg);
    RESULT_ENSURE_REF(hdr);

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * In each control message, initialize cmsg_len (with CMSG_LEN()), the
     * other cmsghdr header fields, and the data portion using
     * CMSG_DATA().
     */
    hdr->cmsg_len = CMSG_LEN(S2N_KTLS_RECORD_TYPE_SIZE);
    hdr->cmsg_level = S2N_SOL_TLS;
    hdr->cmsg_type = cmsg_type;
    *CMSG_DATA(hdr) = record_type;

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * Finally, the msg_controllen field of the msghdr
     * should be set to the sum of the CMSG_SPACE() of the length of all
     * control messages in the buffer
     */
    RESULT_ENSURE_GTE(msg->msg_controllen, CMSG_SPACE(S2N_KTLS_RECORD_TYPE_SIZE));
    msg->msg_controllen = CMSG_SPACE(S2N_KTLS_RECORD_TYPE_SIZE);

    return S2N_RESULT_OK;
}

/* Expect to receive a single cmsghdr containing the TLS record_type.
 *
 * s2n-tls allocates enough space to receive a single cmsghdr. Since this is
 * used to get the record_type when receiving over kTLS (enabled via
 * `s2n_connection_ktls_enable_recv`), the application should not configure
 * the socket to receive additional control messages. In the event s2n-tls
 * can not retrieve the record_type, it is safer to drop the record.
 */
S2N_RESULT s2n_ktls_get_control_data(struct msghdr *msg, int cmsg_type, uint8_t *record_type)
{
    RESULT_ENSURE_REF(msg);
    RESULT_ENSURE_REF(record_type);

    /* https://man7.org/linux/man-pages/man3/recvmsg.3p.html
     * MSG_CTRUNC  Control data was truncated.
     */
    if (msg->msg_flags & MSG_CTRUNC) {
        RESULT_BAIL(S2N_ERR_KTLS_BAD_CMSG);
    }

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * To create ancillary data, first initialize the msg_controllen
     * member of the msghdr with the length of the control message
     * buffer.
     */
    RESULT_ENSURE(msg->msg_control, S2N_ERR_SAFETY);
    RESULT_ENSURE(msg->msg_controllen >= CMSG_SPACE(S2N_KTLS_RECORD_TYPE_SIZE), S2N_ERR_SAFETY);

    /* https://man7.org/linux/man-pages/man3/cmsg.3.html
     * Use CMSG_FIRSTHDR() on the msghdr to get the first
     * control message and CMSG_NXTHDR() to get all subsequent ones.
     */
    struct cmsghdr *hdr = CMSG_FIRSTHDR(msg);
    RESULT_ENSURE(hdr, S2N_ERR_KTLS_BAD_CMSG);

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * In each control message, initialize cmsg_len (with CMSG_LEN()), the
     * other cmsghdr header fields, and the data portion using
     * CMSG_DATA().
     */
    RESULT_ENSURE(hdr->cmsg_level == S2N_SOL_TLS, S2N_ERR_KTLS_BAD_CMSG);
    RESULT_ENSURE(hdr->cmsg_type == cmsg_type, S2N_ERR_KTLS_BAD_CMSG);
    RESULT_ENSURE(hdr->cmsg_len == CMSG_LEN(S2N_KTLS_RECORD_TYPE_SIZE), S2N_ERR_KTLS_BAD_CMSG);
    *record_type = *CMSG_DATA(hdr);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ktls_sendmsg(void *io_context, uint8_t record_type, const struct iovec *msg_iov,
        size_t msg_iovlen, s2n_blocked_status *blocked, size_t *bytes_written)
{
    RESULT_ENSURE_REF(bytes_written);
    RESULT_ENSURE_REF(blocked);
    RESULT_ENSURE(msg_iov != NULL || msg_iovlen == 0, S2N_ERR_NULL);

    *blocked = S2N_BLOCKED_ON_WRITE;
    *bytes_written = 0;

    struct msghdr msg = {
        /* msghdr requires a non-const iovec. This is safe because s2n-tls does
         * not modify msg_iov after this point.
         */
        .msg_iov = (struct iovec *) (uintptr_t) msg_iov,
        .msg_iovlen = msg_iovlen,
    };

    char control_data[S2N_KTLS_CONTROL_BUFFER_SIZE] = { 0 };
    RESULT_GUARD(s2n_ktls_set_control_data(&msg, control_data, sizeof(control_data),
            S2N_TLS_SET_RECORD_TYPE, record_type));

    ssize_t result = 0;
    S2N_IO_RETRY_EINTR(result, s2n_sendmsg_fn(io_context, &msg));
    RESULT_GUARD(s2n_io_check_write_result(result));

    *blocked = S2N_NOT_BLOCKED;
    *bytes_written = result;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ktls_recvmsg(void *io_context, uint8_t *record_type, uint8_t *buf,
        size_t buf_len, s2n_blocked_status *blocked, size_t *bytes_read)
{
    RESULT_ENSURE_REF(record_type);
    RESULT_ENSURE_REF(bytes_read);
    RESULT_ENSURE_REF(blocked);
    RESULT_ENSURE_REF(buf);
    /* Ensure that buf_len is > 0 since trying to receive 0 bytes does not
     * make sense and a return value of `0` from recvmsg is treated as EOF.
     */
    RESULT_ENSURE_GT(buf_len, 0);

    *blocked = S2N_BLOCKED_ON_READ;
    *record_type = 0;
    *bytes_read = 0;
    struct iovec msg_iov = {
        .iov_base = buf,
        .iov_len = buf_len
    };
    struct msghdr msg = {
        .msg_iov = &msg_iov,
        .msg_iovlen = 1,
    };

    /*
     * https://man7.org/linux/man-pages/man3/cmsg.3.html
     * To create ancillary data, first initialize the msg_controllen
     * member of the msghdr with the length of the control message
     * buffer.
     */
    char control_data[S2N_KTLS_CONTROL_BUFFER_SIZE] = { 0 };
    msg.msg_controllen = sizeof(control_data);
    msg.msg_control = control_data;

    ssize_t result = 0;
    S2N_IO_RETRY_EINTR(result, s2n_recvmsg_fn(io_context, &msg));
    RESULT_GUARD(s2n_io_check_read_result(result));

    RESULT_GUARD(s2n_ktls_get_control_data(&msg, S2N_TLS_GET_RECORD_TYPE, record_type));

    *blocked = S2N_NOT_BLOCKED;
    *bytes_read = result;
    return S2N_RESULT_OK;
}

/* The RFC defines the encryption limits in terms of "full-size records" sent.
 * We can estimate the number of "full-sized records" sent by assuming that
 * all records are full-sized.
 */
static S2N_RESULT s2n_ktls_estimate_records(size_t bytes, uint64_t *estimate)
{
    RESULT_ENSURE_REF(estimate);
    uint64_t records = bytes / S2N_TLS_MAXIMUM_FRAGMENT_LENGTH;
    if (bytes % S2N_TLS_MAXIMUM_FRAGMENT_LENGTH) {
        records++;
    }
    *estimate = records;
    return S2N_RESULT_OK;
}

/* ktls does not currently support updating keys, so we should kill the connection
 * when the key encryption limit is reached. We could get the current record
 * sequence number from the kernel with getsockopt, but that requires a surprisingly
 * expensive syscall.
 *
 * Instead, we track the estimated sequence number and enforce the limit based
 * on that estimate.
 */
static S2N_RESULT s2n_ktls_check_estimated_record_limit(
        struct s2n_connection *conn, size_t bytes_requested)
{
    RESULT_ENSURE_REF(conn);
    if (conn->actual_protocol_version < S2N_TLS13) {
        return S2N_RESULT_OK;
    }

    uint64_t new_records_sent = 0;
    RESULT_GUARD(s2n_ktls_estimate_records(bytes_requested, &new_records_sent));

    uint64_t old_records_sent = 0;
    struct s2n_blob seq_num = { 0 };
    RESULT_GUARD(s2n_connection_get_sequence_number(conn, conn->mode, &seq_num));
    RESULT_GUARD_POSIX(s2n_sequence_number_to_uint64(&seq_num, &old_records_sent));

    RESULT_ENSURE(S2N_ADD_IS_OVERFLOW_SAFE(old_records_sent, new_records_sent, UINT64_MAX),
            S2N_ERR_KTLS_KEY_LIMIT);
    uint64_t total_records_sent = old_records_sent + new_records_sent;

    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);
    RESULT_ENSURE_REF(conn->secure->cipher_suite->record_alg);
    uint64_t encryption_limit = conn->secure->cipher_suite->record_alg->encryption_limit;
    RESULT_ENSURE(total_records_sent <= encryption_limit, S2N_ERR_KTLS_KEY_LIMIT);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_ktls_set_estimated_sequence_number(
        struct s2n_connection *conn, size_t bytes_written)
{
    RESULT_ENSURE_REF(conn);
    if (conn->actual_protocol_version < S2N_TLS13) {
        return S2N_RESULT_OK;
    }

    uint64_t new_records_sent = 0;
    RESULT_GUARD(s2n_ktls_estimate_records(bytes_written, &new_records_sent));

    struct s2n_blob seq_num = { 0 };
    RESULT_GUARD(s2n_connection_get_sequence_number(conn, conn->mode, &seq_num));

    for (size_t i = 0; i < new_records_sent; i++) {
        RESULT_GUARD_POSIX(s2n_increment_sequence_number(&seq_num));
    }
    return S2N_RESULT_OK;
}

/* The iovec array `bufs` is constant and owned by the application.
 *
 * However, we need to apply the given offset to `bufs`. That may involve
 * updating the iov_base and iov_len of entries in `bufs` to reflect the bytes
 * already sent. Because `bufs` is constant, we need to instead copy `bufs` and
 * modify the copy.
 *
 * Since one of the primary benefits of kTLS is that we avoid buffering application
 * data and can pass application data as-is to the kernel, we try to limit the
 * situations where we need to copy `bufs` and use stack memory where possible.
 *
 * Note: We are copying an array of iovecs here, NOT the scattered application
 * data the iovecs reference. On Linux, the maximum data copied would be
 * 1024 (IOV_MAX on Linux) * 16 (sizeof(struct iovec)) = ~16KB.
 *
 * To avoid any copies when using a large number of iovecs, applications should
 * call s2n_sendv instead of s2n_sendv_with_offset.
 */
static S2N_RESULT s2n_ktls_update_bufs_with_offset(const struct iovec **bufs, size_t *count,
        size_t offs, struct s2n_blob *mem)
{
    RESULT_ENSURE_REF(bufs);
    RESULT_ENSURE_REF(count);
    RESULT_ENSURE(*bufs != NULL || *count == 0, S2N_ERR_NULL);
    RESULT_ENSURE_REF(mem);

    size_t skipped = 0;
    while (offs > 0) {
        /* If we need to skip more iovecs than actually exist,
         * then the offset is too large and therefore invalid.
         */
        RESULT_ENSURE(skipped < *count, S2N_ERR_INVALID_ARGUMENT);

        size_t iov_len = (*bufs)[skipped].iov_len;

        /* This is the last iovec affected by the offset. */
        if (offs < iov_len) {
            break;
        }

        offs -= iov_len;
        skipped++;
    }

    *count = (*count) - skipped;
    if (*count == 0) {
        return S2N_RESULT_OK;
    }

    *bufs = &(*bufs)[skipped];
    if (offs == 0) {
        return S2N_RESULT_OK;
    }

    size_t size = (*count) * (sizeof(struct iovec));
    /* If possible, use the existing stack memory in `mem` for the copy.
     * Otherwise, we need to allocate sufficient new heap memory. */
    if (size > mem->size) {
        RESULT_GUARD_POSIX(s2n_alloc(mem, size));
    }

    struct iovec *new_bufs = (struct iovec *) (void *) mem->data;
    RESULT_CHECKED_MEMCPY(new_bufs, *bufs, size);
    new_bufs[0].iov_base = (uint8_t *) new_bufs[0].iov_base + offs;
    new_bufs[0].iov_len = new_bufs[0].iov_len - offs;
    *bufs = new_bufs;

    return S2N_RESULT_OK;
}

ssize_t s2n_ktls_sendv_with_offset(struct s2n_connection *conn, const struct iovec *bufs,
        ssize_t count_in, ssize_t offs_in, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(count_in >= 0, S2N_ERR_INVALID_ARGUMENT);
    size_t count = count_in;
    POSIX_ENSURE(offs_in >= 0, S2N_ERR_INVALID_ARGUMENT);
    size_t offs = offs_in;

    ssize_t total_bytes = 0;
    POSIX_GUARD_RESULT(s2n_sendv_with_offset_total_size(bufs, count_in, offs_in, &total_bytes));
    POSIX_GUARD_RESULT(s2n_ktls_check_estimated_record_limit(conn, total_bytes));

    /* The order of new_bufs and new_bufs_mem matters. See https://github.com/aws/s2n-tls/issues/4354 */
    uint8_t new_bufs_mem[S2N_MAX_STACK_IOVECS_MEM] = { 0 };
    DEFER_CLEANUP(struct s2n_blob new_bufs = { 0 }, s2n_free_or_wipe);
    POSIX_GUARD(s2n_blob_init(&new_bufs, new_bufs_mem, sizeof(new_bufs_mem)));
    if (offs > 0) {
        POSIX_GUARD_RESULT(s2n_ktls_update_bufs_with_offset(&bufs, &count, offs, &new_bufs));
    }

    size_t bytes_written = 0;
    POSIX_GUARD_RESULT(s2n_ktls_sendmsg(conn->send_io_context, TLS_APPLICATION_DATA,
            bufs, count, blocked, &bytes_written));

    POSIX_GUARD_RESULT(s2n_ktls_set_estimated_sequence_number(conn, bytes_written));
    return bytes_written;
}

int s2n_ktls_send_cb(void *io_context, const uint8_t *buf, uint32_t len)
{
    POSIX_ENSURE_REF(io_context);
    POSIX_ENSURE_REF(buf);

    /* For now, all control records are assumed to be alerts.
     * We can set the record_type on the io_context in the future.
     */
    const uint8_t record_type = TLS_ALERT;

    const struct iovec iov = {
        .iov_base = (void *) (uintptr_t) buf,
        .iov_len = len,
    };
    s2n_blocked_status blocked = S2N_NOT_BLOCKED;
    size_t bytes_written = 0;

    POSIX_GUARD_RESULT(s2n_ktls_sendmsg(io_context, record_type, &iov, 1,
            &blocked, &bytes_written));

    POSIX_ENSURE_LTE(bytes_written, len);
    return bytes_written;
}

int s2n_ktls_record_writev(struct s2n_connection *conn, uint8_t content_type,
        const struct iovec *in, int in_count, size_t offs, size_t to_write)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(in_count > 0, S2N_ERR_INVALID_ARGUMENT);
    size_t count = in_count;
    POSIX_ENSURE_REF(in);

    /* Currently, ktls only supports sending alerts.
     * To also support handshake messages, we would need a way to track record_type.
     * We could add a field to the send io context.
     */
    POSIX_ENSURE(content_type == TLS_ALERT, S2N_ERR_UNIMPLEMENTED);

    /* When stuffers automatically resize, they allocate a potentially large
     * chunk of memory to avoid repeated resizes.
     * Since ktls only uses conn->out for control messages (alerts and eventually
     * handshake messages), we expect infrequent small writes with conn->out
     * freed in between. Since we're therefore more concerned with the size of
     * the allocation than the frequency, use a more accurate size for each write.
     */
    POSIX_GUARD(s2n_stuffer_resize_if_empty(&conn->out, to_write));

    POSIX_GUARD(s2n_stuffer_writev_bytes(&conn->out, in, count, offs, to_write));
    return to_write;
}

int s2n_sendfile(struct s2n_connection *conn, int in_fd, off_t offset, size_t count,
        size_t *bytes_written, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(blocked);
    *blocked = S2N_BLOCKED_ON_WRITE;
    POSIX_ENSURE_REF(bytes_written);
    *bytes_written = 0;
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(conn->ktls_send_enabled, S2N_ERR_KTLS_UNSUPPORTED_CONN);
    POSIX_GUARD_RESULT(s2n_ktls_check_estimated_record_limit(conn, count));

    int out_fd = 0;
    POSIX_GUARD_RESULT(s2n_ktls_get_file_descriptor(conn, S2N_KTLS_MODE_SEND, &out_fd));

#ifdef S2N_LINUX_SENDFILE
    /* https://man7.org/linux/man-pages/man2/sendfile.2.html */
    ssize_t result = 0;
    S2N_IO_RETRY_EINTR(result, sendfile(out_fd, in_fd, &offset, count));
    POSIX_GUARD_RESULT(s2n_io_check_write_result(result));
    *bytes_written = result;
#else
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
#endif

    POSIX_GUARD_RESULT(s2n_ktls_set_estimated_sequence_number(conn, *bytes_written));
    *blocked = S2N_NOT_BLOCKED;
    return S2N_SUCCESS;
}

int s2n_ktls_read_full_record(struct s2n_connection *conn, uint8_t *record_type)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(record_type);

    /* If any unread data remains in conn->in, it must be application data that
     * couldn't be returned due to the size of the application's provided buffer.
     */
    if (s2n_stuffer_data_available(&conn->in)) {
        *record_type = TLS_APPLICATION_DATA;
        return S2N_SUCCESS;
    }

    POSIX_GUARD(s2n_stuffer_resize_if_empty(&conn->buffer_in, S2N_DEFAULT_FRAGMENT_LENGTH));

    struct s2n_stuffer record_stuffer = conn->buffer_in;
    size_t len = s2n_stuffer_space_remaining(&record_stuffer);
    uint8_t *buf = s2n_stuffer_raw_write(&record_stuffer, len);
    POSIX_ENSURE_REF(buf);

    s2n_blocked_status blocked = S2N_NOT_BLOCKED;
    size_t bytes_read = 0;

    /* Since recvmsg is responsible for decrypting the record in ktls,
     * we apply blinding to the recvmsg call.
     */
    s2n_result result = s2n_ktls_recvmsg(conn->recv_io_context, record_type,
            buf, len, &blocked, &bytes_read);
    WITH_ERROR_BLINDING(conn, POSIX_GUARD_RESULT(result));

    POSIX_GUARD(s2n_stuffer_skip_write(&conn->buffer_in, bytes_read));

    /* We don't care about returning a full fragment because we don't need to decrypt.
     * kTLS handled decryption already.
     * So we can always set conn->in equal to the full buffer_in.
     */
    POSIX_GUARD_RESULT(s2n_recv_in_init(conn, bytes_read, bytes_read));
    return S2N_SUCCESS;
}
