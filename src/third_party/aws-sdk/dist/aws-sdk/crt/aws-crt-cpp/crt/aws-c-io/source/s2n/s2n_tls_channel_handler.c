/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/tls_channel_handler.h>

#include <aws/common/clock.h>
#include <aws/common/encoding.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/task_scheduler.h>
#include <aws/common/thread.h>
#include <aws/io/channel.h>
#include <aws/io/event_loop.h>
#include <aws/io/file_utils.h>
#include <aws/io/logging.h>
#include <aws/io/private/event_loop_impl.h>
#include <aws/io/private/pki_utils.h>
#include <aws/io/private/tls_channel_handler_shared.h>
#include <aws/io/statistics.h>

#include <s2n.h>
#ifdef AWS_S2N_INSOURCE_PATH
#    include <api/unstable/cleanup.h>
#else
#    include <s2n/unstable/cleanup.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define EST_TLS_RECORD_OVERHEAD 53 /* 5 byte header + 32 + 16 bytes for padding */
#define KB_1 1024
#define MAX_RECORD_SIZE (KB_1 * 16)
#define EST_HANDSHAKE_SIZE (7 * KB_1)

static const char *s_default_ca_dir = NULL;
static const char *s_default_ca_file = NULL;

struct s2n_handler {
    struct aws_channel_handler handler;
    struct aws_tls_channel_handler_shared shared_state;
    struct s2n_connection *connection;
    struct s2n_ctx *s2n_ctx;
    struct aws_channel_slot *slot;
    struct aws_linked_list input_queue;
    struct aws_byte_buf protocol;
    struct aws_byte_buf server_name;
    aws_channel_on_message_write_completed_fn *latest_message_on_completion;
    struct aws_channel_task sequential_tasks;
    void *latest_message_completion_user_data;
    aws_tls_on_negotiation_result_fn *on_negotiation_result;
    aws_tls_on_data_read_fn *on_data_read;
    aws_tls_on_error_fn *on_error;
    void *user_data;
    bool advertise_alpn_message;
    enum {
        NEGOTIATION_ONGOING,
        NEGOTIATION_FAILED,
        NEGOTIATION_SUCCEEDED,
    } state;
    struct aws_channel_task read_task;
    bool read_task_pending;
    enum aws_tls_handler_read_state read_state;
    int shutdown_error_code;
    struct aws_channel_task delayed_shutdown_task;
};

struct s2n_ctx {
    struct aws_tls_ctx ctx;
    struct s2n_config *s2n_config;

    /* Only used in special circumstances (ex: have cert but no key, because key is in PKCS#11) */
    struct s2n_cert_chain_and_key *custom_cert_chain_and_key;

    /**
     * Custom key operations to perform when a private key operation is required in the TLS handshake.
     * Only will be used if non-NULL, otherwise this is ignored and the standard private key operations
     * are performed instead.
     * NOTE: PKCS11 also is done via this custom_key_handler.
     *
     * See aws_custom_key_op_handler in tls_channel_handler.h for more details.
     */
    struct aws_custom_key_op_handler *custom_key_handler;
};

struct aws_tls_key_operation {
    struct aws_allocator *alloc;
    struct s2n_async_pkey_op *s2n_op;
    struct s2n_handler *s2n_handler;
    enum aws_tls_key_operation_type operation_type;
    enum aws_tls_signature_algorithm signature_algorithm;
    enum aws_tls_hash_algorithm digest_algorithm;
    struct aws_byte_buf input_data;
    struct aws_channel_task completion_task;
    int completion_error_code;

    struct aws_atomic_var complete_count;
};

AWS_STATIC_STRING_FROM_LITERAL(s_debian_path, "/etc/ssl/certs");
AWS_STATIC_STRING_FROM_LITERAL(s_rhel_path, "/etc/pki/tls/certs");
AWS_STATIC_STRING_FROM_LITERAL(s_android_path, "/system/etc/security/cacerts");
AWS_STATIC_STRING_FROM_LITERAL(s_free_bsd_path, "/usr/local/share/certs");
AWS_STATIC_STRING_FROM_LITERAL(s_net_bsd_path, "/etc/openssl/certs");

AWS_IO_API const char *aws_determine_default_pki_dir(void) {
    /* debian variants; OpenBSD (although the directory doesn't exist by default) */
    if (aws_path_exists(s_debian_path)) {
        return aws_string_c_str(s_debian_path);
    }

    /* RHEL variants */
    if (aws_path_exists(s_rhel_path)) {
        return aws_string_c_str(s_rhel_path);
    }

    /* android */
    if (aws_path_exists(s_android_path)) {
        return aws_string_c_str(s_android_path);
    }

    /* FreeBSD */
    if (aws_path_exists(s_free_bsd_path)) {
        return aws_string_c_str(s_free_bsd_path);
    }

    /* NetBSD */
    if (aws_path_exists(s_net_bsd_path)) {
        return aws_string_c_str(s_net_bsd_path);
    }

    return NULL;
}

AWS_STATIC_STRING_FROM_LITERAL(s_debian_ca_file_path, "/etc/ssl/certs/ca-certificates.crt");
AWS_STATIC_STRING_FROM_LITERAL(s_old_rhel_ca_file_path, "/etc/pki/tls/certs/ca-bundle.crt");
AWS_STATIC_STRING_FROM_LITERAL(s_open_suse_ca_file_path, "/etc/ssl/ca-bundle.pem");
AWS_STATIC_STRING_FROM_LITERAL(s_open_elec_ca_file_path, "/etc/pki/tls/cacert.pem");
AWS_STATIC_STRING_FROM_LITERAL(s_modern_rhel_ca_file_path, "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem");
AWS_STATIC_STRING_FROM_LITERAL(s_openbsd_ca_file_path, "/etc/ssl/cert.pem");

AWS_IO_API const char *aws_determine_default_pki_ca_file(void) {
    /* debian variants */
    if (aws_path_exists(s_debian_ca_file_path)) {
        return aws_string_c_str(s_debian_ca_file_path);
    }

    /* Old RHEL variants */
    if (aws_path_exists(s_old_rhel_ca_file_path)) {
        return aws_string_c_str(s_old_rhel_ca_file_path);
    }

    /* Open SUSE */
    if (aws_path_exists(s_open_suse_ca_file_path)) {
        return aws_string_c_str(s_open_suse_ca_file_path);
    }

    /* Open ELEC */
    if (aws_path_exists(s_open_elec_ca_file_path)) {
        return aws_string_c_str(s_open_elec_ca_file_path);
    }

    /* Modern RHEL variants */
    if (aws_path_exists(s_modern_rhel_ca_file_path)) {
        return aws_string_c_str(s_modern_rhel_ca_file_path);
    }

    /* OpenBSD */
    if (aws_path_exists(s_openbsd_ca_file_path)) {
        return aws_string_c_str(s_openbsd_ca_file_path);
    }

    return NULL;
}

static struct aws_allocator *s_library_allocator = NULL;

static int s_s2n_mem_init(void) {
    return S2N_SUCCESS;
}

static int s_s2n_mem_cleanup(void) {
    return S2N_SUCCESS;
}

static int s_s2n_mem_malloc(void **ptr, uint32_t requested, uint32_t *allocated) {
    *ptr = aws_mem_acquire(s_library_allocator, requested);
    *allocated = requested;

    return S2N_SUCCESS;
}

static int s_s2n_mem_free(void *ptr, uint32_t size) {
    (void)size;
    aws_mem_release(s_library_allocator, ptr);
    return S2N_SUCCESS;
}

/* If s2n is already initialized, then we don't call s2n_init() or s2n_cleanup() ourselves */
static bool s_s2n_initialized_externally = false;

void aws_tls_init_static_state(struct aws_allocator *alloc) {
    AWS_FATAL_ASSERT(alloc);
    AWS_LOGF_INFO(AWS_LS_IO_TLS, "static: Initializing TLS using s2n.");

    /* Disable atexit behavior, so that s2n_cleanup() fully cleans things up.
     *
     * By default, s2n uses an ataexit handler and doesn't fully clean up until the program exits.
     * This can cause a crash if s2n is compiled into a shared library and
     * that library is unloaded before the appexit handler runs. */
    if (s2n_disable_atexit() != S2N_SUCCESS) {
        /* If this call fails, then s2n is already initialized
         * https://github.com/aws/s2n-tls/blob/2ad65c11a96368591fe809cd27fd1e390b2c8ce3/api/s2n.h#L211-L212 */
        AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "static: s2n is already initialized");
        s_s2n_initialized_externally = true;
    } else {
        s_s2n_initialized_externally = false;
    }

    if (!s_s2n_initialized_externally) {
        s_library_allocator = alloc;
        if (S2N_SUCCESS != s2n_mem_set_callbacks(s_s2n_mem_init, s_s2n_mem_cleanup, s_s2n_mem_malloc, s_s2n_mem_free)) {
            fprintf(stderr, "s2n_mem_set_callbacks() failed: %d (%s)\n", s2n_errno, s2n_strerror(s2n_errno, "EN"));
            AWS_FATAL_ASSERT(0 && "s2n_mem_set_callbacks() failed");
        }

        if (s2n_init() != S2N_SUCCESS) {
            fprintf(stderr, "s2n_init() failed: %d (%s)\n", s2n_errno, s2n_strerror(s2n_errno, "EN"));
            AWS_FATAL_ASSERT(0 && "s2n_init() failed");
        }
    }

    s_default_ca_dir = aws_determine_default_pki_dir();
    s_default_ca_file = aws_determine_default_pki_ca_file();
    if (s_default_ca_dir || s_default_ca_file) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_TLS,
            "ctx: Based on OS, we detected the default PKI path as %s, and ca file as %s",
            s_default_ca_dir,
            s_default_ca_file);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_IO_TLS,
            "Default TLS trust store not found on this system."
            " TLS connections will fail unless trusted CA certificates are installed,"
            " or \"override default trust store\" is used while creating the TLS context.");
    }
}

void aws_tls_clean_up_static_state(void) {
    /* only clean up s2n if we were the ones that initialized it */
    if (!s_s2n_initialized_externally) {
        s2n_cleanup_final();
    }
}

bool aws_tls_is_alpn_available(void) {
    return true;
}

bool aws_tls_is_cipher_pref_supported(enum aws_tls_cipher_pref cipher_pref) {
    switch (cipher_pref) {
        case AWS_IO_TLS_CIPHER_PREF_SYSTEM_DEFAULT:
            return true;
            /* PQ Crypto no-ops on android for now */
#ifndef ANDROID
        case AWS_IO_TLS_CIPHER_PREF_PQ_TLSv1_0_2021_05:
            return true;
        case AWS_IO_TLS_CIPHER_PREF_PQ_TLSV1_2_2024_10:
            return true;
#endif

        default:
            return false;
    }
}

static int s_generic_read(struct s2n_handler *handler, struct aws_byte_buf *buf) {

    size_t written = 0;

    while (!aws_linked_list_empty(&handler->input_queue) && written < buf->len) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&handler->input_queue);
        struct aws_io_message *message = AWS_CONTAINER_OF(node, struct aws_io_message, queueing_handle);

        size_t remaining_message_len = message->message_data.len - message->copy_mark;
        size_t remaining_buf_len = buf->len - written;

        size_t to_write = remaining_message_len < remaining_buf_len ? remaining_message_len : remaining_buf_len;

        struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&message->message_data);
        aws_byte_cursor_advance(&message_cursor, message->copy_mark);
        aws_byte_cursor_read(&message_cursor, buf->buffer + written, to_write);

        written += to_write;

        message->copy_mark += to_write;

        if (message->copy_mark == message->message_data.len) {
            aws_mem_release(message->allocator, message);
        } else {
            aws_linked_list_push_front(&handler->input_queue, &message->queueing_handle);
        }
    }

    if (written) {
        return (int)written;
    }

    errno = EAGAIN;
    return -1;
}

static int s_s2n_handler_recv(void *io_context, uint8_t *buf, uint32_t len) {
    struct s2n_handler *handler = (struct s2n_handler *)io_context;

    struct aws_byte_buf read_buffer = aws_byte_buf_from_array(buf, len);
    return s_generic_read(handler, &read_buffer);
}

static int s_generic_send(struct s2n_handler *handler, struct aws_byte_buf *buf) {

    struct aws_byte_cursor buffer_cursor = aws_byte_cursor_from_buf(buf);

    size_t processed = 0;
    while (processed < buf->len) {
        const size_t overhead = aws_channel_slot_upstream_message_overhead(handler->slot);
        const size_t message_size_hint = (buf->len - processed) + overhead;
        struct aws_io_message *message = aws_channel_acquire_message_from_pool(
            handler->slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, message_size_hint);

        if (message->message_data.capacity <= overhead) {
            aws_mem_release(message->allocator, message);
            errno = ENOMEM;
            return -1;
        }

        const size_t available_msg_write_capacity = message->message_data.capacity - overhead;
        const size_t to_write =
            available_msg_write_capacity >= buffer_cursor.len ? buffer_cursor.len : available_msg_write_capacity;

        struct aws_byte_cursor chunk = aws_byte_cursor_advance(&buffer_cursor, to_write);
        if (aws_byte_buf_append(&message->message_data, &chunk)) {
            aws_mem_release(message->allocator, message);
            return -1;
        }
        processed += message->message_data.len;

        if (processed == buf->len) {
            message->on_completion = handler->latest_message_on_completion;
            message->user_data = handler->latest_message_completion_user_data;
            handler->latest_message_on_completion = NULL;
            handler->latest_message_completion_user_data = NULL;
        }

        if (aws_channel_slot_send_message(handler->slot, message, AWS_CHANNEL_DIR_WRITE)) {
            aws_mem_release(message->allocator, message);
            errno = EPIPE;
            return -1;
        }
    }

    if (processed) {
        return (int)processed;
    }

    errno = EAGAIN;
    return -1;
}

static int s_s2n_handler_send(void *io_context, const uint8_t *buf, uint32_t len) {
    struct s2n_handler *handler = (struct s2n_handler *)io_context;
    struct aws_byte_buf send_buf = aws_byte_buf_from_array(buf, len);

    return s_generic_send(handler, &send_buf);
}

static void s_s2n_handler_destroy(struct aws_channel_handler *handler) {
    if (handler) {
        struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
        aws_tls_channel_handler_shared_clean_up(&s2n_handler->shared_state);
        if (s2n_handler->connection) {
            s2n_connection_free(s2n_handler->connection);
        }
        if (s2n_handler->s2n_ctx) {
            aws_tls_ctx_release(&s2n_handler->s2n_ctx->ctx);
        }
        aws_mem_release(handler->alloc, (void *)s2n_handler);
    }
}

static void s_on_negotiation_result(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int error_code,
    void *user_data) {

    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    aws_on_tls_negotiation_completed(&s2n_handler->shared_state, error_code);

    if (s2n_handler->on_negotiation_result) {
        s2n_handler->on_negotiation_result(handler, slot, error_code, user_data);
    }
}

static int s_drive_negotiation(struct aws_channel_handler *handler) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    AWS_ASSERT(s2n_handler->state == NEGOTIATION_ONGOING);

    aws_on_drive_tls_negotiation(&s2n_handler->shared_state);

    s2n_blocked_status blocked = S2N_NOT_BLOCKED;
    do {
        int negotiation_code = s2n_negotiate(s2n_handler->connection, &blocked);

        int s2n_error = s2n_errno;
        if (negotiation_code == S2N_ERR_T_OK) {
            s2n_handler->state = NEGOTIATION_SUCCEEDED;

            const char *protocol = s2n_get_application_protocol(s2n_handler->connection);
            if (protocol) {
                AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "id=%p: Alpn protocol negotiated as %s", (void *)handler, protocol);
                s2n_handler->protocol = aws_byte_buf_from_c_str(protocol);
            }

            const char *server_name = s2n_get_server_name(s2n_handler->connection);

            if (server_name) {
                AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "id=%p: Remote server name is %s", (void *)handler, server_name);
                s2n_handler->server_name = aws_byte_buf_from_c_str(server_name);
            }

            if (s2n_handler->slot->adj_right && s2n_handler->advertise_alpn_message && protocol) {
                struct aws_io_message *message = aws_channel_acquire_message_from_pool(
                    s2n_handler->slot->channel,
                    AWS_IO_MESSAGE_APPLICATION_DATA,
                    sizeof(struct aws_tls_negotiated_protocol_message));
                message->message_tag = AWS_TLS_NEGOTIATED_PROTOCOL_MESSAGE;
                struct aws_tls_negotiated_protocol_message *protocol_message =
                    (struct aws_tls_negotiated_protocol_message *)message->message_data.buffer;

                protocol_message->protocol = s2n_handler->protocol;
                message->message_data.len = sizeof(struct aws_tls_negotiated_protocol_message);
                if (aws_channel_slot_send_message(s2n_handler->slot, message, AWS_CHANNEL_DIR_READ)) {
                    aws_mem_release(message->allocator, message);
                    aws_channel_shutdown(s2n_handler->slot->channel, aws_last_error());
                    return AWS_OP_SUCCESS;
                }
            }

            s_on_negotiation_result(handler, s2n_handler->slot, AWS_OP_SUCCESS, s2n_handler->user_data);

            break;
        }
        if (s2n_error_get_type(s2n_error) != S2N_ERR_T_BLOCKED) {
            AWS_LOGF_WARN(
                AWS_LS_IO_TLS,
                "id=%p: negotiation failed with error %s (%s)",
                (void *)handler,
                s2n_strerror(s2n_error, "EN"),
                s2n_strerror_debug(s2n_error, "EN"));

            if (s2n_error_get_type(s2n_error) == S2N_ERR_T_ALERT) {
                AWS_LOGF_DEBUG(
                    AWS_LS_IO_TLS,
                    "id=%p: Alert code %d",
                    (void *)handler,
                    s2n_connection_get_alert(s2n_handler->connection));
            }

            const char *err_str = s2n_strerror_debug(s2n_error, NULL);
            (void)err_str;
            s2n_handler->state = NEGOTIATION_FAILED;

            aws_raise_error(AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE);

            s_on_negotiation_result(
                handler, s2n_handler->slot, AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE, s2n_handler->user_data);

            return AWS_OP_ERR;
        }
    } while (blocked == S2N_NOT_BLOCKED);

    return AWS_OP_SUCCESS;
}

static void s_negotiation_task(struct aws_channel_task *task, void *arg, aws_task_status status) {
    task->task_fn = NULL;
    task->arg = NULL;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_channel_handler *handler = arg;
        struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
        if (s2n_handler->state == NEGOTIATION_ONGOING) {
            s_drive_negotiation(handler);
        }
    }
}

int aws_tls_client_handler_start_negotiation(struct aws_channel_handler *handler) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    AWS_LOGF_TRACE(AWS_LS_IO_TLS, "id=%p: Kicking off TLS negotiation.", (void *)handler);
    if (aws_channel_thread_is_callers_thread(s2n_handler->slot->channel)) {
        if (s2n_handler->state == NEGOTIATION_ONGOING) {
            s_drive_negotiation(handler);
        }
        return AWS_OP_SUCCESS;
    }

    aws_channel_task_init(
        &s2n_handler->sequential_tasks, s_negotiation_task, handler, "s2n_channel_handler_negotiation");
    aws_channel_schedule_task_now(s2n_handler->slot->channel, &s2n_handler->sequential_tasks);

    return AWS_OP_SUCCESS;
}

static int s_s2n_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    struct s2n_handler *s2n_handler = handler->impl;

    if (s2n_handler->read_state == AWS_TLS_HANDLER_READ_SHUT_DOWN_COMPLETE) {
        if (message) {
            aws_mem_release(message->allocator, message);
        }
        return AWS_OP_SUCCESS;
    }

    if (AWS_UNLIKELY(s2n_handler->state == NEGOTIATION_FAILED)) {
        return aws_raise_error(AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE);
    }

    if (message) {
        aws_linked_list_push_back(&s2n_handler->input_queue, &message->queueing_handle);

        if (s2n_handler->state == NEGOTIATION_ONGOING) {
            size_t message_len = message->message_data.len;
            if (s_drive_negotiation(handler) == AWS_OP_SUCCESS) {
                aws_channel_slot_increment_read_window(slot, message_len);
            } else {
                aws_channel_shutdown(s2n_handler->slot->channel, AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE);
            }
            return AWS_OP_SUCCESS;
        }
    }

    s2n_blocked_status blocked = S2N_NOT_BLOCKED;
    size_t downstream_window = SIZE_MAX;
    if (slot->adj_right) {
        downstream_window = aws_channel_slot_downstream_read_window(slot);
    }
    int shutdown_error_code = 0;

    size_t processed = 0;
    AWS_LOGF_TRACE(
        AWS_LS_IO_TLS, "id=%p: Downstream window %llu", (void *)handler, (unsigned long long)downstream_window);

    while (processed < downstream_window) {

        struct aws_io_message *outgoing_read_message = aws_channel_acquire_message_from_pool(
            slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, downstream_window - processed);

        ssize_t read = s2n_recv(
            s2n_handler->connection,
            outgoing_read_message->message_data.buffer,
            outgoing_read_message->message_data.capacity,
            &blocked);

        AWS_LOGF_TRACE(AWS_LS_IO_TLS, "id=%p: Bytes read %lld", (void *)handler, (long long)read);

        /* weird race where we received an alert from the peer, but s2n doesn't tell us about it.....
         * if this happens, it's a graceful shutdown, so kick it off here.
         *
         * In other words, s2n, upon graceful shutdown, follows the unix EOF idiom. So just shutdown with
         * SUCCESS.
         */
        if (read == 0) {
            AWS_LOGF_DEBUG(
                AWS_LS_IO_TLS,
                "id=%p: Alert code %d",
                (void *)handler,
                s2n_connection_get_alert(s2n_handler->connection));
            aws_mem_release(outgoing_read_message->allocator, outgoing_read_message);
            goto shutdown_channel;
        }

        if (read < 0) {
            aws_mem_release(outgoing_read_message->allocator, outgoing_read_message);

            /* the socket blocked so exit from the loop */
            if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
                if (s2n_handler->read_state == AWS_TLS_HANDLER_READ_SHUTTING_DOWN) {
                    /* Propagate the shutdown as we blocked now. */
                    goto shutdown_channel;
                }
                break;
            }

            /* the socket returned a fatal error so shut down */
            AWS_LOGF_ERROR(
                AWS_LS_IO_TLS,
                "id=%p: S2N failed to read with error: %s (%s)",
                (void *)handler,
                s2n_strerror(s2n_errno, "EN"),
                s2n_strerror_debug(s2n_errno, "EN"));
            shutdown_error_code = AWS_IO_TLS_ERROR_READ_FAILURE;
            goto shutdown_channel;
        };

        /* if read > 0 */
        processed += read;
        outgoing_read_message->message_data.len = (size_t)read;

        if (s2n_handler->on_data_read) {
            s2n_handler->on_data_read(handler, slot, &outgoing_read_message->message_data, s2n_handler->user_data);
        }

        if (slot->adj_right) {
            aws_channel_slot_send_message(slot, outgoing_read_message, AWS_CHANNEL_DIR_READ);
        } else {
            aws_mem_release(outgoing_read_message->allocator, outgoing_read_message);
        }
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_TLS,
        "id=%p: Remaining window for this event-loop tick: %llu",
        (void *)handler,
        (unsigned long long)downstream_window - processed);

    return AWS_OP_SUCCESS;

shutdown_channel:
    if (s2n_handler->read_state == AWS_TLS_HANDLER_READ_SHUTTING_DOWN) {
        if (s2n_handler->shutdown_error_code != 0) {
            /* Propagate the original error code if it is set. */
            shutdown_error_code = s2n_handler->shutdown_error_code;
        }
        s2n_handler->read_state = AWS_TLS_HANDLER_READ_SHUT_DOWN_COMPLETE;
        aws_channel_slot_on_handler_shutdown_complete(slot, AWS_CHANNEL_DIR_READ, shutdown_error_code, false);
    } else {
        /* Starts the shutdown process */
        aws_channel_shutdown(slot->channel, shutdown_error_code);
    }
    return AWS_OP_SUCCESS;
}

static int s_s2n_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)slot;
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    if (AWS_UNLIKELY(s2n_handler->state != NEGOTIATION_SUCCEEDED)) {
        return aws_raise_error(AWS_IO_TLS_ERROR_NOT_NEGOTIATED);
    }

    s2n_handler->latest_message_on_completion = message->on_completion;
    s2n_handler->latest_message_completion_user_data = message->user_data;

    s2n_blocked_status blocked;
    ssize_t write_code =
        s2n_send(s2n_handler->connection, message->message_data.buffer, (ssize_t)message->message_data.len, &blocked);

    AWS_LOGF_TRACE(AWS_LS_IO_TLS, "id=%p: Bytes written: %llu", (void *)handler, (unsigned long long)write_code);

    ssize_t message_len = (ssize_t)message->message_data.len;

    if (write_code < message_len) {
        return aws_raise_error(AWS_IO_TLS_ERROR_WRITE_FAILURE);
    }

    aws_mem_release(message->allocator, message);

    return AWS_OP_SUCCESS;
}

static void s_delayed_shutdown_task_fn(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    (void)channel_task;

    struct aws_channel_handler *handler = arg;
    struct s2n_handler *s2n_handler = handler->impl;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "id=%p: Delayed shut down in write direction", (void *)handler);
        s2n_blocked_status blocked;
        /* make a best effort, but the channel is going away after this run, so.... you only get one shot anyways */
        s2n_shutdown(s2n_handler->connection, &blocked);
    }
    aws_channel_slot_on_handler_shutdown_complete(
        s2n_handler->slot, AWS_CHANNEL_DIR_WRITE, s2n_handler->shutdown_error_code, false);
}

static enum aws_tls_signature_algorithm s_s2n_to_aws_signature_algorithm(s2n_tls_signature_algorithm s2n_alg) {
    switch (s2n_alg) {
        case S2N_TLS_SIGNATURE_RSA:
            return AWS_TLS_SIGNATURE_RSA;
        case S2N_TLS_SIGNATURE_ECDSA:
            return AWS_TLS_SIGNATURE_ECDSA;
        default:
            return AWS_TLS_SIGNATURE_UNKNOWN;
    }
}

static enum aws_tls_hash_algorithm s_s2n_to_aws_hash_algorithm(s2n_tls_hash_algorithm s2n_alg) {
    switch (s2n_alg) {
        case (S2N_TLS_HASH_SHA1):
            return AWS_TLS_HASH_SHA1;
        case (S2N_TLS_HASH_SHA224):
            return AWS_TLS_HASH_SHA224;
        case (S2N_TLS_HASH_SHA256):
            return AWS_TLS_HASH_SHA256;
        case (S2N_TLS_HASH_SHA384):
            return AWS_TLS_HASH_SHA384;
        case (S2N_TLS_HASH_SHA512):
            return AWS_TLS_HASH_SHA512;
        default:
            return AWS_TLS_HASH_UNKNOWN;
    }
}

static void s_tls_key_operation_destroy(struct aws_tls_key_operation *operation) {
    if (operation->s2n_op) {
        s2n_async_pkey_op_free(operation->s2n_op);
    }
    if (operation->s2n_handler) {
        aws_channel_release_hold(operation->s2n_handler->slot->channel);
    }
    aws_byte_buf_clean_up(&operation->input_data);
    aws_mem_release(operation->alloc, operation);
}

/* This task finishes a private key operation on the event-loop thread.
 * If the operation was successful, TLS negotiation is resumed.
 * If the operation failed, the channel is shut down */
static void s_tls_key_operation_completion_task(
    struct aws_channel_task *channel_task,
    void *arg,
    enum aws_task_status status) {

    (void)channel_task;
    struct aws_tls_key_operation *operation = arg;
    struct s2n_handler *s2n_handler = operation->s2n_handler;
    struct aws_channel_handler *handler = &s2n_handler->handler;

    /* if things started failing since this task was scheduled, just clean up and bail out */
    if (status != AWS_TASK_STATUS_RUN_READY || s2n_handler->state != NEGOTIATION_ONGOING) {
        goto clean_up;
    }

    if (operation->completion_error_code == 0) {
        if (s2n_async_pkey_op_apply(operation->s2n_op, s2n_handler->connection)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed applying s2n async pkey op", (void *)handler);
            operation->completion_error_code = AWS_ERROR_INVALID_STATE;
        }
    }

    if (operation->completion_error_code == 0) {
        s_drive_negotiation(handler);
    } else {
        aws_channel_shutdown(s2n_handler->slot->channel, operation->completion_error_code);
    }

clean_up:
    s_tls_key_operation_destroy(operation);
}

/* Common implementation for aws_tls_key_operation_complete() and aws_tls_key_operation_complete_with_error()
 * This is called exactly once. Schedules a task to actually finish things up on the event-loop thread. */
static void s_tls_key_operation_complete_common(
    struct aws_tls_key_operation *operation,
    int error_code,
    const struct aws_byte_cursor *output) {

    AWS_ASSERT((error_code != 0) ^ (output != NULL)); /* error_code XOR output must be set */

    /* Ensure this can only be called once and exactly once. */
    size_t complete_count = aws_atomic_fetch_add(&operation->complete_count, 1);
    AWS_FATAL_ASSERT(complete_count == 0 && "TLS key operation marked complete multiple times");

    struct s2n_handler *s2n_handler = operation->s2n_handler;
    struct aws_channel_handler *handler = &s2n_handler->handler;

    if (output != NULL) {
        /* Immediately pass output through to s2n_op. */
        if (s2n_async_pkey_op_set_output(operation->s2n_op, output->ptr, output->len)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed setting output on s2n async pkey op", (void *)handler);
            error_code = AWS_ERROR_INVALID_STATE;
            goto done;
        }
    }

done:
    operation->completion_error_code = error_code;

    /* Schedule a task to finish the operation.
     * We schedule a task because the user might
     * have completed the operation asynchronously,
     * but we need to be on the event-loop thread to
     * resume TLS negotiation. */
    aws_channel_task_init(
        &operation->completion_task,
        s_tls_key_operation_completion_task,
        operation,
        "tls_key_operation_completion_task");
    aws_channel_schedule_task_now(s2n_handler->slot->channel, &operation->completion_task);
}

void aws_tls_key_operation_complete(struct aws_tls_key_operation *operation, struct aws_byte_cursor output) {
    if (operation == NULL) {
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "Operation complete: operation is null and therefore cannot be set to complete!");
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_TLS,
        "id=%p: TLS key operation complete with %zu bytes of output data",
        (void *)operation->s2n_handler,
        output.len);
    s_tls_key_operation_complete_common(operation, 0, &output);
}

void aws_tls_key_operation_complete_with_error(struct aws_tls_key_operation *operation, int error_code) {
    if (operation == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_TLS, "Operation complete with error: operation is null and therefore cannot be set to complete!");
        return;
    }

    if (error_code == 0) {
        error_code = AWS_ERROR_UNKNOWN;
        AWS_LOGF_ERROR(
            AWS_LS_IO_TLS,
            "id=%p: TLS key operation completed with error, but no error-code set. Using %s",
            (void *)operation->s2n_handler,
            aws_error_name(error_code));
    }

    AWS_LOGF_ERROR(
        AWS_LS_IO_TLS,
        "id=%p: TLS key operation complete with error %s",
        (void *)operation->s2n_handler,
        aws_error_name(error_code));

    s_tls_key_operation_complete_common(operation, error_code, NULL);
}

static struct aws_tls_key_operation *s_tls_key_operation_new(
    struct aws_channel_handler *handler,
    struct s2n_async_pkey_op *s2n_op) {

    struct s2n_handler *s2n_handler = handler->impl;

    struct aws_tls_key_operation *operation = aws_mem_calloc(handler->alloc, 1, sizeof(struct aws_tls_key_operation));
    operation->alloc = handler->alloc;

    /* Copy input data */
    uint32_t input_size = 0;
    if (s2n_async_pkey_op_get_input_size(s2n_op, &input_size)) {
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed querying s2n async pkey op size", (void *)handler);
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    aws_byte_buf_init(&operation->input_data, operation->alloc, input_size); /* cannot fail */
    if (s2n_async_pkey_op_get_input(s2n_op, operation->input_data.buffer, input_size)) {
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed querying s2n async pkey input", (void *)handler);
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }
    operation->input_data.len = input_size;

    /* Get operation type */
    s2n_async_pkey_op_type s2n_op_type = 0;
    if (s2n_async_pkey_op_get_op_type(s2n_op, &s2n_op_type)) {
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed querying s2n async pkey op type", (void *)handler);
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    if (s2n_op_type == S2N_ASYNC_SIGN) {
        operation->operation_type = AWS_TLS_KEY_OPERATION_SIGN;

        /* Gather additional information if this is a SIGN operation */
        s2n_tls_signature_algorithm s2n_sign_alg = 0;
        if (s2n_connection_get_selected_client_cert_signature_algorithm(s2n_handler->connection, &s2n_sign_alg)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed getting s2n client cert signature algorithm", (void *)handler);
            aws_raise_error(AWS_ERROR_INVALID_STATE);
            goto error;
        }

        operation->signature_algorithm = s_s2n_to_aws_signature_algorithm(s2n_sign_alg);
        if (operation->signature_algorithm == AWS_TLS_SIGNATURE_UNKNOWN) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_TLS,
                "id=%p: Cannot sign with s2n_tls_signature_algorithm=%d. Algorithm currently unsupported",
                (void *)handler,
                s2n_sign_alg);
            aws_raise_error(AWS_IO_TLS_SIGNATURE_ALGORITHM_UNSUPPORTED);
            goto error;
        }

        s2n_tls_hash_algorithm s2n_digest_alg = 0;
        if (s2n_connection_get_selected_client_cert_digest_algorithm(s2n_handler->connection, &s2n_digest_alg)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Failed getting s2n client cert digest algorithm", (void *)handler);
            aws_raise_error(AWS_ERROR_INVALID_STATE);
            goto error;
        }

        operation->digest_algorithm = s_s2n_to_aws_hash_algorithm(s2n_digest_alg);
        if (operation->digest_algorithm == AWS_TLS_HASH_UNKNOWN) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_TLS,
                "id=%p: Cannot sign digest created with s2n_tls_hash_algorithm=%d. Algorithm currently unsupported",
                (void *)handler,
                s2n_digest_alg);
            aws_raise_error(AWS_IO_TLS_DIGEST_ALGORITHM_UNSUPPORTED);
            goto error;
        }

    } else if (s2n_op_type == S2N_ASYNC_DECRYPT) {
        operation->operation_type = AWS_TLS_KEY_OPERATION_DECRYPT;

    } else {
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "id=%p: Unknown s2n async pkey op type:%d", (void *)handler, (int)s2n_op_type);
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    /* Keep channel alive until operation completes */
    operation->s2n_handler = s2n_handler;
    aws_channel_acquire_hold(s2n_handler->slot->channel);

    /* Set this to zero so we can track how many times complete has been called */
    aws_atomic_init_int(&operation->complete_count, 0);

    /* Set this last. We don't want to take ownership of s2n_op until we know setup was 100% successful */
    operation->s2n_op = s2n_op;

    return operation;
error:
    s_tls_key_operation_destroy(operation);
    return NULL;
}

struct aws_byte_cursor aws_tls_key_operation_get_input(const struct aws_tls_key_operation *operation) {
    return aws_byte_cursor_from_buf(&operation->input_data);
}

enum aws_tls_key_operation_type aws_tls_key_operation_get_type(const struct aws_tls_key_operation *operation) {
    return operation->operation_type;
}

enum aws_tls_signature_algorithm aws_tls_key_operation_get_signature_algorithm(
    const struct aws_tls_key_operation *operation) {
    return operation->signature_algorithm;
}

enum aws_tls_hash_algorithm aws_tls_key_operation_get_digest_algorithm(const struct aws_tls_key_operation *operation) {
    return operation->digest_algorithm;
}

static int s_s2n_async_pkey_callback(struct s2n_connection *conn, struct s2n_async_pkey_op *s2n_op) {
    struct s2n_handler *s2n_handler = s2n_connection_get_ctx(conn);
    struct aws_channel_handler *handler = &s2n_handler->handler;

    AWS_ASSERT(conn == s2n_handler->connection);
    (void)conn;

    AWS_LOGF_TRACE(AWS_LS_IO_TLS, "id=%p: s2n async pkey callback received", (void *)handler);

    /* Create the AWS wrapper around s2n_async_pkey_op */
    struct aws_tls_key_operation *operation = s_tls_key_operation_new(handler, s2n_op);
    if (operation == NULL) {
        s2n_async_pkey_op_free(s2n_op);
        return S2N_FAILURE;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_TLS,
        "id=%p: Begin TLS key operation. type=%s input_data.len=%zu signature=%s digest=%s",
        (void *)operation,
        aws_tls_key_operation_type_str(operation->operation_type),
        operation->input_data.len,
        aws_tls_signature_algorithm_str(operation->signature_algorithm),
        aws_tls_hash_algorithm_str(operation->digest_algorithm));

    aws_custom_key_op_handler_perform_operation(s2n_handler->s2n_ctx->custom_key_handler, operation);

    return S2N_SUCCESS;
}

static int s_s2n_do_delayed_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int error_code) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    s2n_handler->shutdown_error_code = error_code;

    uint64_t shutdown_delay = s2n_connection_get_delay(s2n_handler->connection);
    uint64_t now = 0;

    if (aws_channel_current_clock_time(slot->channel, &now)) {
        return AWS_OP_ERR;
    }

    uint64_t shutdown_time = aws_add_u64_saturating(shutdown_delay, now);
    aws_channel_schedule_task_future(slot->channel, &s2n_handler->delayed_shutdown_task, shutdown_time);

    return AWS_OP_SUCCESS;
}

static void s_run_read(struct aws_channel_task *task, void *arg, aws_task_status status) {
    task->task_fn = NULL;
    task->arg = NULL;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_channel_handler *handler = (struct aws_channel_handler *)arg;
        struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
        s2n_handler->read_task_pending = false;
        s_s2n_handler_process_read_message(handler, s2n_handler->slot, NULL);
    }
}

static void s_initialize_read_delay_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int error_code) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
    /**
     * In case of if we have any queued data in the handler after negotiation and we start to shutdown,
     * make sure we pass those data down the pipeline before we complete the shutdown.
     */
    AWS_LOGF_DEBUG(
        AWS_LS_IO_TLS,
        "id=%p: TLS handler still have pending data to be delivered during shutdown. Wait until downstream "
        "reads the data.",
        (void *)handler);
    if (aws_channel_slot_downstream_read_window(slot) == 0) {
        AWS_LOGF_WARN(
            AWS_LS_IO_TLS,
            "id=%p: TLS shutdown delayed. Pending data cannot be processed until the flow-control window opens. "
            " Your application may hang if the read window never opens",
            (void *)handler);
    }
    s2n_handler->read_state = AWS_TLS_HANDLER_READ_SHUTTING_DOWN;
    s2n_handler->shutdown_error_code = error_code;
    if (!s2n_handler->read_task_pending) {
        /* Kick off read, in case data arrives with TLS negotiation. Shutdown starts right after negotiation.
         * Nothing will kick off read in that case. */
        s2n_handler->read_task_pending = true;
        aws_channel_task_init(
            &s2n_handler->read_task, s_run_read, handler, "s2n_channel_handler_read_on_delay_shutdown");
        aws_channel_schedule_task_now(slot->channel, &s2n_handler->read_task);
    }
}

static int s_s2n_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool abort_immediately) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;

    if (dir == AWS_CHANNEL_DIR_READ) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_TLS, "id=%p: Shutting down read direction with error code %d", (void *)handler, error_code);

        /* If negotiation hasn't succeeded yet, it's certainly not going to succeed now */
        if (s2n_handler->state == NEGOTIATION_ONGOING) {
            s2n_handler->state = NEGOTIATION_FAILED;
        }

        if (!abort_immediately && s2n_handler->state == NEGOTIATION_SUCCEEDED &&
            !aws_linked_list_empty(&s2n_handler->input_queue) && slot->adj_right) {
            s_initialize_read_delay_shutdown(handler, slot, error_code);
            return AWS_OP_SUCCESS;
        }
        s2n_handler->read_state = AWS_TLS_HANDLER_READ_SHUT_DOWN_COMPLETE;
    } else {
        /* Shutdown in write direction */
        if (!abort_immediately && error_code != AWS_IO_SOCKET_CLOSED) {
            AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "id=%p: Scheduling delayed write direction shutdown", (void *)handler);
            if (s_s2n_do_delayed_shutdown(handler, slot, error_code) == AWS_OP_SUCCESS) {
                return AWS_OP_SUCCESS;
            }
        }
    }
    while (!aws_linked_list_empty(&s2n_handler->input_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&s2n_handler->input_queue);
        struct aws_io_message *message = AWS_CONTAINER_OF(node, struct aws_io_message, queueing_handle);
        aws_mem_release(message->allocator, message);
    }

    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, abort_immediately);
}

static int s_s2n_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {
    (void)size;
    struct s2n_handler *s2n_handler = handler->impl;
    if (s2n_handler->read_state == AWS_TLS_HANDLER_READ_SHUT_DOWN_COMPLETE) {
        return AWS_OP_SUCCESS;
    }

    size_t downstream_size = aws_channel_slot_downstream_read_window(slot);
    size_t current_window_size = slot->window_size;

    AWS_LOGF_TRACE(
        AWS_LS_IO_TLS, "id=%p: Increment read window message received %llu", (void *)handler, (unsigned long long)size);

    size_t likely_records_count = (size_t)ceil((double)(downstream_size) / (double)(MAX_RECORD_SIZE));
    size_t offset_size = aws_mul_size_saturating(likely_records_count, EST_TLS_RECORD_OVERHEAD);
    size_t total_desired_size = aws_add_size_saturating(offset_size, downstream_size);

    if (total_desired_size > current_window_size) {
        size_t window_update_size = total_desired_size - current_window_size;
        AWS_LOGF_TRACE(
            AWS_LS_IO_TLS,
            "id=%p: Propagating read window increment of size %llu",
            (void *)handler,
            (unsigned long long)window_update_size);
        aws_channel_slot_increment_read_window(slot, window_update_size);
    }

    if (s2n_handler->state == NEGOTIATION_SUCCEEDED && !s2n_handler->read_task_pending) {
        /* TLS requires full records before it can decrypt anything. As a result we need to check everything we've
         * buffered instead of just waiting on a read from the socket, or we'll hit a deadlock.
         *
         * We have messages in a queue and they need to be run after the socket has popped (even if it didn't have
         * data to read). Alternatively, s2n reads entire records at a time, so we'll need to grab whatever we can
         * and we have no idea what's going on inside there. So we need to attempt another read.*/
        s2n_handler->read_task_pending = true;
        aws_channel_task_init(
            &s2n_handler->read_task, s_run_read, handler, "s2n_channel_handler_read_on_window_increment");
        aws_channel_schedule_task_now(slot->channel, &s2n_handler->read_task);
    }

    return AWS_OP_SUCCESS;
}

static size_t s_s2n_handler_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return EST_TLS_RECORD_OVERHEAD;
}

static size_t s_s2n_handler_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;

    return EST_HANDSHAKE_SIZE;
}

static void s_s2n_handler_reset_statistics(struct aws_channel_handler *handler) {
    struct s2n_handler *s2n_handler = handler->impl;

    aws_crt_statistics_tls_reset(&s2n_handler->shared_state.stats);
}

static void s_s2n_handler_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats) {
    struct s2n_handler *s2n_handler = handler->impl;

    void *stats_base = &s2n_handler->shared_state.stats;
    aws_array_list_push_back(stats, &stats_base);
}

struct aws_byte_buf aws_tls_handler_protocol(struct aws_channel_handler *handler) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
    return s2n_handler->protocol;
}

struct aws_byte_buf aws_tls_handler_server_name(struct aws_channel_handler *handler) {
    struct s2n_handler *s2n_handler = (struct s2n_handler *)handler->impl;
    return s2n_handler->server_name;
}

static struct aws_channel_handler_vtable s_handler_vtable = {
    .destroy = s_s2n_handler_destroy,
    .process_read_message = s_s2n_handler_process_read_message,
    .process_write_message = s_s2n_handler_process_write_message,
    .shutdown = s_s2n_handler_shutdown,
    .increment_read_window = s_s2n_handler_increment_read_window,
    .initial_window_size = s_s2n_handler_initial_window_size,
    .message_overhead = s_s2n_handler_message_overhead,
    .reset_statistics = s_s2n_handler_reset_statistics,
    .gather_statistics = s_s2n_handler_gather_statistics,
};

static int s_parse_protocol_preferences(
    struct aws_string *alpn_list_str,
    const char protocol_output[4][128],
    size_t *protocol_count) {
    size_t max_count = *protocol_count;
    *protocol_count = 0;

    struct aws_byte_cursor alpn_list_buffer[4];
    AWS_ZERO_ARRAY(alpn_list_buffer);
    struct aws_array_list alpn_list;
    struct aws_byte_cursor user_alpn_str = aws_byte_cursor_from_string(alpn_list_str);

    aws_array_list_init_static(&alpn_list, alpn_list_buffer, 4, sizeof(struct aws_byte_cursor));

    if (aws_byte_cursor_split_on_char(&user_alpn_str, ';', &alpn_list)) {
        aws_raise_error(AWS_IO_TLS_CTX_ERROR);
        return AWS_OP_ERR;
    }

    size_t protocols_list_len = aws_array_list_length(&alpn_list);
    if (protocols_list_len < 1) {
        aws_raise_error(AWS_IO_TLS_CTX_ERROR);
        return AWS_OP_ERR;
    }

    for (size_t i = 0; i < protocols_list_len && i < max_count; ++i) {
        struct aws_byte_cursor cursor;
        AWS_ZERO_STRUCT(cursor);
        if (aws_array_list_get_at(&alpn_list, (void *)&cursor, (size_t)i)) {
            aws_raise_error(AWS_IO_TLS_CTX_ERROR);
            return AWS_OP_ERR;
        }
        AWS_FATAL_ASSERT(cursor.ptr && cursor.len > 0);
        memcpy((void *)protocol_output[i], cursor.ptr, cursor.len);
        *protocol_count += 1;
    }

    return AWS_OP_SUCCESS;
}

static size_t s_tl_cleanup_key = 0; /* Address of variable serves as key in hash table */

/*
 * This local object is added to the table of every event loop that has a (s2n) tls connection
 * added to it at some point in time
 */
static struct aws_event_loop_local_object s_tl_cleanup_object = {
    .key = &s_tl_cleanup_key,
    .object = NULL,
    .on_object_removed = NULL,
};

static void s_aws_cleanup_s2n_thread_local_state(void *user_data) {
    (void)user_data;

    s2n_cleanup_thread();
}

/* s2n allocates thread-local data structures. We need to clean these up when the event loop's thread exits. */
static int s_s2n_tls_channel_handler_schedule_thread_local_cleanup(struct aws_channel_slot *slot) {
    struct aws_channel *channel = slot->channel;

    struct aws_event_loop_local_object existing_marker;
    AWS_ZERO_STRUCT(existing_marker);

    /*
     * Check whether another s2n_tls_channel_handler has already scheduled the cleanup task.
     */
    if (aws_channel_fetch_local_object(channel, &s_tl_cleanup_key, &existing_marker)) {
        /* Doesn't exist in event loop table: add it and add the at-exit cleanup callback */
        if (aws_channel_put_local_object(channel, &s_tl_cleanup_key, &s_tl_cleanup_object)) {
            return AWS_OP_ERR;
        }

        aws_thread_current_at_exit(s_aws_cleanup_s2n_thread_local_state, NULL);
    }

    return AWS_OP_SUCCESS;
}

static struct aws_channel_handler *s_new_tls_handler(
    struct aws_allocator *allocator,
    struct aws_tls_connection_options *options,
    struct aws_channel_slot *slot,
    s2n_mode mode) {

    AWS_ASSERT(options->ctx);
    struct s2n_handler *s2n_handler = aws_mem_calloc(allocator, 1, sizeof(struct s2n_handler));
    s2n_handler->handler.impl = s2n_handler;
    s2n_handler->handler.alloc = allocator;
    s2n_handler->handler.vtable = &s_handler_vtable;
    s2n_handler->handler.slot = slot;

    aws_tls_ctx_acquire(options->ctx);
    s2n_handler->s2n_ctx = options->ctx->impl;

    s2n_handler->connection = s2n_connection_new(mode);

    if (!s2n_handler->connection) {
        goto cleanup_conn;
    }

    aws_tls_channel_handler_shared_init(&s2n_handler->shared_state, &s2n_handler->handler, options);

    s2n_handler->user_data = options->user_data;
    s2n_handler->on_data_read = options->on_data_read;
    s2n_handler->on_error = options->on_error;
    s2n_handler->on_negotiation_result = options->on_negotiation_result;
    s2n_handler->advertise_alpn_message = options->advertise_alpn_message;

    s2n_handler->latest_message_completion_user_data = NULL;
    s2n_handler->latest_message_on_completion = NULL;
    s2n_handler->slot = slot;
    aws_linked_list_init(&s2n_handler->input_queue);

    s2n_handler->protocol = aws_byte_buf_from_array(NULL, 0);

    if (options->server_name) {

        if (s2n_set_server_name(s2n_handler->connection, aws_string_c_str(options->server_name))) {
            aws_raise_error(AWS_IO_TLS_CTX_ERROR);
            goto cleanup_conn;
        }
    }

    s2n_handler->state = NEGOTIATION_ONGOING;

    s2n_connection_set_recv_cb(s2n_handler->connection, s_s2n_handler_recv);
    s2n_connection_set_recv_ctx(s2n_handler->connection, s2n_handler);
    s2n_connection_set_send_cb(s2n_handler->connection, s_s2n_handler_send);
    s2n_connection_set_send_ctx(s2n_handler->connection, s2n_handler);
    s2n_connection_set_ctx(s2n_handler->connection, s2n_handler);
    s2n_connection_set_blinding(s2n_handler->connection, S2N_SELF_SERVICE_BLINDING);

    if (options->alpn_list) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_TLS,
            "id=%p: Setting ALPN list %s",
            (void *)&s2n_handler->handler,
            aws_string_c_str(options->alpn_list));

        const char protocols_cpy[4][128];
        AWS_ZERO_ARRAY(protocols_cpy);
        size_t protocols_size = 4;
        if (s_parse_protocol_preferences(options->alpn_list, protocols_cpy, &protocols_size)) {
            aws_raise_error(AWS_IO_TLS_CTX_ERROR);
            goto cleanup_conn;
        }

        const char *protocols[4];
        AWS_ZERO_ARRAY(protocols);
        for (size_t i = 0; i < protocols_size; ++i) {
            protocols[i] = protocols_cpy[i];
        }

        if (s2n_connection_set_protocol_preferences(
                s2n_handler->connection, (const char *const *)protocols, (int)protocols_size)) {
            aws_raise_error(AWS_IO_TLS_CTX_ERROR);
            goto cleanup_conn;
        }
    }

    if (s2n_connection_set_config(s2n_handler->connection, s2n_handler->s2n_ctx->s2n_config)) {
        AWS_LOGF_WARN(
            AWS_LS_IO_TLS,
            "id=%p: configuration error %s (%s)",
            (void *)&s2n_handler->handler,
            s2n_strerror(s2n_errno, "EN"),
            s2n_strerror_debug(s2n_errno, "EN"));
        aws_raise_error(AWS_IO_TLS_CTX_ERROR);
        goto cleanup_conn;
    }

    aws_channel_task_init(
        &s2n_handler->delayed_shutdown_task, s_delayed_shutdown_task_fn, &s2n_handler->handler, "s2n_delayed_shutdown");

    if (s_s2n_tls_channel_handler_schedule_thread_local_cleanup(slot)) {
        goto cleanup_conn;
    }

    return &s2n_handler->handler;

cleanup_conn:
    s_s2n_handler_destroy(&s2n_handler->handler);

    return NULL;
}

struct aws_channel_handler *aws_tls_client_handler_new(
    struct aws_allocator *allocator,
    struct aws_tls_connection_options *options,
    struct aws_channel_slot *slot) {

    return s_new_tls_handler(allocator, options, slot, S2N_CLIENT);
}

struct aws_channel_handler *aws_tls_server_handler_new(
    struct aws_allocator *allocator,
    struct aws_tls_connection_options *options,
    struct aws_channel_slot *slot) {

    return s_new_tls_handler(allocator, options, slot, S2N_SERVER);
}

static void s_s2n_ctx_destroy(struct s2n_ctx *s2n_ctx) {
    if (s2n_ctx != NULL) {
        if (s2n_ctx->s2n_config) {
            s2n_config_free(s2n_ctx->s2n_config);
        }
        if (s2n_ctx->custom_cert_chain_and_key) {
            s2n_cert_chain_and_key_free(s2n_ctx->custom_cert_chain_and_key);
        }
        s2n_ctx->custom_key_handler = aws_custom_key_op_handler_release(s2n_ctx->custom_key_handler);

        aws_mem_release(s2n_ctx->ctx.alloc, s2n_ctx);
    }
}

static int s2n_wall_clock_time_nanoseconds(void *context, uint64_t *time_in_ns) {
    (void)context;
    if (aws_sys_clock_get_ticks(time_in_ns)) {
        *time_in_ns = 0;
        return -1;
    }

    return 0;
}

static int s2n_monotonic_clock_time_nanoseconds(void *context, uint64_t *time_in_ns) {
    (void)context;
    if (aws_high_res_clock_get_ticks(time_in_ns)) {
        *time_in_ns = 0;
        return -1;
    }

    return 0;
}

static void s_log_and_raise_s2n_errno(const char *msg) {
    AWS_LOGF_ERROR(
        AWS_LS_IO_TLS, "%s: %s (%s)", msg, s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"));
    aws_raise_error(AWS_IO_TLS_CTX_ERROR);
}

static struct aws_tls_ctx *s_tls_ctx_new(
    struct aws_allocator *alloc,
    const struct aws_tls_ctx_options *options,
    s2n_mode mode) {
    struct s2n_ctx *s2n_ctx = aws_mem_calloc(alloc, 1, sizeof(struct s2n_ctx));

    if (!s2n_ctx) {
        return NULL;
    }

    if (!aws_tls_is_cipher_pref_supported(options->cipher_pref)) {
        aws_raise_error(AWS_IO_TLS_CIPHER_PREF_UNSUPPORTED);
        AWS_LOGF_ERROR(AWS_LS_IO_TLS, "static: TLS Cipher Preference is not supported: %d.", options->cipher_pref);
        return NULL;
    }

    s2n_ctx->ctx.alloc = alloc;
    s2n_ctx->ctx.impl = s2n_ctx;
    aws_ref_count_init(&s2n_ctx->ctx.ref_count, s2n_ctx, (aws_simple_completion_callback *)s_s2n_ctx_destroy);

    s2n_ctx->s2n_config = s2n_config_new();
    if (!s2n_ctx->s2n_config) {
        s_log_and_raise_s2n_errno("ctx: creation failed");
        goto cleanup_s2n_config;
    }

    int set_clock_result = s2n_config_set_wall_clock(s2n_ctx->s2n_config, s2n_wall_clock_time_nanoseconds, NULL);
    if (set_clock_result != S2N_ERR_T_OK) {
        s_log_and_raise_s2n_errno("ctx: failed to set wall clock");
        goto cleanup_s2n_config;
    }

    set_clock_result = s2n_config_set_monotonic_clock(s2n_ctx->s2n_config, s2n_monotonic_clock_time_nanoseconds, NULL);
    if (set_clock_result != S2N_ERR_T_OK) {
        s_log_and_raise_s2n_errno("ctx: failed to set monotonic clock");
        goto cleanup_s2n_config;
    }

    const char *security_policy = NULL;
    if (options->custom_key_op_handler != NULL) {
        /* When custom_key_op_handler is set, don't use security policy that allow TLS 1.3.
         * This hack is necessary until our PKCS#11 custom_key_op_handler supports RSA PSS */
        switch (options->minimum_tls_version) {
            case AWS_IO_SSLv3:
                security_policy = "CloudFront-SSL-v-3";
                break;
            case AWS_IO_TLSv1:
                security_policy = "CloudFront-TLS-1-0-2014";
                break;
            case AWS_IO_TLSv1_1:
                security_policy = "ELBSecurityPolicy-TLS-1-1-2017-01";
                break;
            case AWS_IO_TLSv1_2:
                security_policy = "ELBSecurityPolicy-TLS-1-2-Ext-2018-06";
                break;
            case AWS_IO_TLSv1_3:
                AWS_LOGF_ERROR(AWS_LS_IO_TLS, "TLS 1.3 with PKCS#11 is not supported yet.");
                aws_raise_error(AWS_IO_TLS_VERSION_UNSUPPORTED);
                goto cleanup_s2n_config;
            case AWS_IO_TLS_VER_SYS_DEFAULTS:
            default:
                security_policy = "ELBSecurityPolicy-TLS-1-1-2017-01";
        }
    } else {
        /* No custom_key_op_handler is set, use normal security policies */
        switch (options->minimum_tls_version) {
            case AWS_IO_SSLv3:
                security_policy = "AWS-CRT-SDK-SSLv3.0-2023";
                break;
            case AWS_IO_TLSv1:
                security_policy = "AWS-CRT-SDK-TLSv1.0-2023";
                break;
            case AWS_IO_TLSv1_1:
                security_policy = "AWS-CRT-SDK-TLSv1.1-2023";
                break;
            case AWS_IO_TLSv1_2:
                security_policy = "AWS-CRT-SDK-TLSv1.2-2023";
                break;
            case AWS_IO_TLSv1_3:
                security_policy = "AWS-CRT-SDK-TLSv1.3-2023";
                break;
            case AWS_IO_TLS_VER_SYS_DEFAULTS:
            default:
                security_policy = "AWS-CRT-SDK-TLSv1.0-2023";
        }
    }

    switch (options->cipher_pref) {
        case AWS_IO_TLS_CIPHER_PREF_SYSTEM_DEFAULT:
            /* No-Op, if the user configured a minimum_tls_version then a version-specific Cipher Preference was set
             */
            break;
        case AWS_IO_TLS_CIPHER_PREF_PQ_TLSv1_0_2021_05:
            security_policy = "PQ-TLS-1-0-2021-05-26";
            break;
        case AWS_IO_TLS_CIPHER_PREF_PQ_TLSV1_2_2024_10:
            security_policy = "AWS-CRT-SDK-TLSv1.2-2023-PQ";
            break;
        default:
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "Unrecognized TLS Cipher Preference: %d", options->cipher_pref);
            aws_raise_error(AWS_IO_TLS_CIPHER_PREF_UNSUPPORTED);
            goto cleanup_s2n_config;
    }

    AWS_ASSERT(security_policy != NULL);
    if (s2n_config_set_cipher_preferences(s2n_ctx->s2n_config, security_policy)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_TLS,
            "ctx: Failed setting security policy '%s' (newer S2N required?): %s (%s)",
            security_policy,
            s2n_strerror(s2n_errno, "EN"),
            s2n_strerror_debug(s2n_errno, "EN"));
        aws_raise_error(AWS_IO_TLS_CTX_ERROR);
        goto cleanup_s2n_config;
    }

    if (aws_tls_options_buf_is_set(&options->certificate) && aws_tls_options_buf_is_set(&options->private_key)) {
        AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "ctx: Certificate and key have been set, setting them up now.");

        if (!aws_text_is_utf8(options->certificate.buffer, options->certificate.len)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "static: failed to import certificate, must be ASCII/UTF-8 encoded");
            aws_raise_error(AWS_IO_FILE_VALIDATION_FAILURE);
            goto cleanup_s2n_config;
        }

        if (!aws_text_is_utf8(options->private_key.buffer, options->private_key.len)) {
            AWS_LOGF_ERROR(AWS_LS_IO_TLS, "static: failed to import private key, must be ASCII/UTF-8 encoded");
            aws_raise_error(AWS_IO_FILE_VALIDATION_FAILURE);
            goto cleanup_s2n_config;
        }

        /* Ensure that what we pass to s2n is zero-terminated */
        struct aws_string *certificate_string = aws_string_new_from_buf(alloc, &options->certificate);
        struct aws_string *private_key_string = aws_string_new_from_buf(alloc, &options->private_key);

        int err_code = s2n_config_add_cert_chain_and_key(
            s2n_ctx->s2n_config, (const char *)certificate_string->bytes, (const char *)private_key_string->bytes);

        aws_string_destroy(certificate_string);
        aws_string_destroy_secure(private_key_string);

        if (mode == S2N_CLIENT) {
            s2n_config_set_client_auth_type(s2n_ctx->s2n_config, S2N_CERT_AUTH_REQUIRED);
        }

        if (err_code != S2N_ERR_T_OK) {
            s_log_and_raise_s2n_errno("ctx: Failed to add certificate and private key");
            goto cleanup_s2n_config;
        }
    } else if (options->custom_key_op_handler != NULL) {

        s2n_ctx->custom_key_handler = aws_custom_key_op_handler_acquire(options->custom_key_op_handler);

        /* set callback so that we can do custom private key operations */
        if (s2n_config_set_async_pkey_callback(s2n_ctx->s2n_config, s_s2n_async_pkey_callback)) {
            s_log_and_raise_s2n_errno("ctx: failed to set private key callback");
            goto cleanup_s2n_config;
        }

        /* set certificate.
         * we need to create a custom s2n_cert_chain_and_key that knows the cert but not the key */
        s2n_ctx->custom_cert_chain_and_key = s2n_cert_chain_and_key_new();
        if (!s2n_ctx->custom_cert_chain_and_key) {
            s_log_and_raise_s2n_errno("ctx: creation failed");
            goto cleanup_s2n_config;
        }

        if (s2n_cert_chain_and_key_load_public_pem_bytes(
                s2n_ctx->custom_cert_chain_and_key, options->certificate.buffer, options->certificate.len)) {
            s_log_and_raise_s2n_errno("ctx: failed to load certificate");
            goto cleanup_s2n_config;
        }

        if (s2n_config_add_cert_chain_and_key_to_store(s2n_ctx->s2n_config, s2n_ctx->custom_cert_chain_and_key)) {
            s_log_and_raise_s2n_errno("ctx: failed to add certificate to store");
            goto cleanup_s2n_config;
        }

        if (mode == S2N_CLIENT) {
            s2n_config_set_client_auth_type(s2n_ctx->s2n_config, S2N_CERT_AUTH_REQUIRED);
        }
    }

    if (options->verify_peer) {
        if (s2n_config_set_check_stapled_ocsp_response(s2n_ctx->s2n_config, 1) == S2N_SUCCESS) {
            if (s2n_config_set_status_request_type(s2n_ctx->s2n_config, S2N_STATUS_REQUEST_OCSP) != S2N_SUCCESS) {
                s_log_and_raise_s2n_errno("ctx: ocsp status request cannot be set");
                goto cleanup_s2n_config;
            }
        } else {
            if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_USAGE) {
                AWS_LOGF_INFO(AWS_LS_IO_TLS, "ctx: cannot enable ocsp stapling: %s", s2n_strerror(s2n_errno, "EN"));
            } else {
                s_log_and_raise_s2n_errno("ctx: cannot enable ocsp stapling");
                goto cleanup_s2n_config;
            }
        }

        if (options->ca_path || aws_tls_options_buf_is_set(&options->ca_file)) {
            /* The user called an override_default_trust_store() function.
             * Begin by wiping anything that s2n loaded by default */
            if (s2n_config_wipe_trust_store(s2n_ctx->s2n_config)) {
                s_log_and_raise_s2n_errno("ctx: failed to wipe default trust store");
                goto cleanup_s2n_config;
            }

            if (options->ca_path) {
                if (s2n_config_set_verification_ca_location(
                        s2n_ctx->s2n_config, NULL, aws_string_c_str(options->ca_path))) {
                    s_log_and_raise_s2n_errno("ctx: configuration error");
                    AWS_LOGF_ERROR(AWS_LS_IO_TLS, "Failed to set ca_path %s\n", aws_string_c_str(options->ca_path));
                    goto cleanup_s2n_config;
                }
            }

            if (aws_tls_options_buf_is_set(&options->ca_file)) {
                /* Ensure that what we pass to s2n is zero-terminated */
                struct aws_string *ca_file_string = aws_string_new_from_buf(alloc, &options->ca_file);
                int set_ca_result =
                    s2n_config_add_pem_to_trust_store(s2n_ctx->s2n_config, (const char *)ca_file_string->bytes);
                aws_string_destroy(ca_file_string);

                if (set_ca_result) {
                    s_log_and_raise_s2n_errno("ctx: configuration error");
                    AWS_LOGF_ERROR(AWS_LS_IO_TLS, "Failed to set ca_file %s\n", (const char *)options->ca_file.buffer);
                    goto cleanup_s2n_config;
                }
            }
        } else if (s_default_ca_file || s_default_ca_dir) {
            /* User wants to use the system's default trust store.
             *
             * Note that s2n's trust store always starts with libcrypto's default locations.
             * These paths are configured when libcrypto is built (--openssldir),
             * but might not be right for the current machine (e.g. if libcrypto
             * is statically linked into an application that is distributed
             * to multiple flavors of Linux). Therefore, load the locations that
             * were found at library startup. */
            if (s2n_config_set_verification_ca_location(s2n_ctx->s2n_config, s_default_ca_file, s_default_ca_dir)) {
                s_log_and_raise_s2n_errno("ctx: configuration error");
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS, "Failed to set ca_path: %s and ca_file %s\n", s_default_ca_dir, s_default_ca_file);
                goto cleanup_s2n_config;
            }
        } else {
            /* Cannot find system's trust store */
            aws_raise_error(AWS_IO_TLS_ERROR_DEFAULT_TRUST_STORE_NOT_FOUND);
            AWS_LOGF_ERROR(
                AWS_LS_IO_TLS,
                "Default TLS trust store not found on this system."
                " Install CA certificates, or \"override default trust store\".");
            goto cleanup_s2n_config;
        }

        if (mode == S2N_SERVER && s2n_config_set_client_auth_type(s2n_ctx->s2n_config, S2N_CERT_AUTH_REQUIRED)) {
            s_log_and_raise_s2n_errno("ctx: failed to set client auth type");
            goto cleanup_s2n_config;
        }
    } else if (mode != S2N_SERVER) {
        AWS_LOGF_WARN(
            AWS_LS_IO_TLS,
            "ctx: X.509 validation has been disabled. "
            "If this is not running in a test environment, this is likely a security vulnerability.");
        if (s2n_config_disable_x509_verification(s2n_ctx->s2n_config)) {
            s_log_and_raise_s2n_errno("ctx: failed to disable x509 verification");
            goto cleanup_s2n_config;
        }
    }

    if (options->alpn_list) {
        AWS_LOGF_DEBUG(AWS_LS_IO_TLS, "ctx: Setting ALPN list %s", aws_string_c_str(options->alpn_list));
        const char protocols_cpy[4][128];
        AWS_ZERO_ARRAY(protocols_cpy);
        size_t protocols_size = 4;
        if (s_parse_protocol_preferences(options->alpn_list, protocols_cpy, &protocols_size)) {
            s_log_and_raise_s2n_errno("ctx: Failed to parse ALPN list");
            goto cleanup_s2n_config;
        }

        const char *protocols[4];
        AWS_ZERO_ARRAY(protocols);
        for (size_t i = 0; i < protocols_size; ++i) {
            protocols[i] = protocols_cpy[i];
        }

        if (s2n_config_set_protocol_preferences(s2n_ctx->s2n_config, protocols, (int)protocols_size)) {
            s_log_and_raise_s2n_errno("ctx: Failed to set protocol preferences");
            goto cleanup_s2n_config;
        }
    }

    if (options->max_fragment_size == 512) {
        s2n_config_send_max_fragment_length(s2n_ctx->s2n_config, S2N_TLS_MAX_FRAG_LEN_512);
    } else if (options->max_fragment_size == 1024) {
        s2n_config_send_max_fragment_length(s2n_ctx->s2n_config, S2N_TLS_MAX_FRAG_LEN_1024);
    } else if (options->max_fragment_size == 2048) {
        s2n_config_send_max_fragment_length(s2n_ctx->s2n_config, S2N_TLS_MAX_FRAG_LEN_2048);
    } else if (options->max_fragment_size == 4096) {
        s2n_config_send_max_fragment_length(s2n_ctx->s2n_config, S2N_TLS_MAX_FRAG_LEN_4096);
    }

    return &s2n_ctx->ctx;

cleanup_s2n_config:
    s_s2n_ctx_destroy(s2n_ctx);

    return NULL;
}

struct aws_tls_ctx *aws_tls_server_ctx_new(struct aws_allocator *alloc, const struct aws_tls_ctx_options *options) {
    aws_io_fatal_assert_library_initialized();
    return s_tls_ctx_new(alloc, options, S2N_SERVER);
}

struct aws_tls_ctx *aws_tls_client_ctx_new(struct aws_allocator *alloc, const struct aws_tls_ctx_options *options) {
    aws_io_fatal_assert_library_initialized();
    return s_tls_ctx_new(alloc, options, S2N_CLIENT);
}
