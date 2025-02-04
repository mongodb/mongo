/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/channel_bootstrap.h>

#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/io/event_loop.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/socket_channel_handler.h>
#include <aws/io/tls_channel_handler.h>

#ifdef _MSC_VER
/* non-constant aggregate initializer */
#    pragma warning(disable : 4204)
/* allow automatic variable to escape scope
   (it's intentional and we make sure it doesn't actually return
    before the task is finished).*/
#    pragma warning(disable : 4221)
#endif

static void s_client_bootstrap_destroy_impl(struct aws_client_bootstrap *bootstrap) {
    AWS_ASSERT(bootstrap);
    AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: bootstrap destroying", (void *)bootstrap);
    aws_client_bootstrap_shutdown_complete_fn *on_shutdown_complete = bootstrap->on_shutdown_complete;
    void *user_data = bootstrap->user_data;

    aws_event_loop_group_release(bootstrap->event_loop_group);
    aws_host_resolver_release(bootstrap->host_resolver);

    aws_mem_release(bootstrap->allocator, bootstrap);

    if (on_shutdown_complete) {
        on_shutdown_complete(user_data);
    }
}

struct aws_client_bootstrap *aws_client_bootstrap_acquire(struct aws_client_bootstrap *bootstrap) {
    if (bootstrap != NULL) {
        AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: acquiring bootstrap reference", (void *)bootstrap);
        aws_ref_count_acquire(&bootstrap->ref_count);
    }

    return bootstrap;
}

void aws_client_bootstrap_release(struct aws_client_bootstrap *bootstrap) {
    if (bootstrap != NULL) {
        AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: releasing bootstrap reference", (void *)bootstrap);
        aws_ref_count_release(&bootstrap->ref_count);
    }
}

struct aws_client_bootstrap *aws_client_bootstrap_new(
    struct aws_allocator *allocator,
    const struct aws_client_bootstrap_options *options) {
    AWS_ASSERT(allocator);
    AWS_ASSERT(options);
    AWS_ASSERT(options->event_loop_group);

    struct aws_client_bootstrap *bootstrap = aws_mem_calloc(allocator, 1, sizeof(struct aws_client_bootstrap));
    if (!bootstrap) {
        return NULL;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Initializing client bootstrap with event-loop group %p",
        (void *)bootstrap,
        (void *)options->event_loop_group);

    bootstrap->allocator = allocator;
    bootstrap->event_loop_group = aws_event_loop_group_acquire(options->event_loop_group);
    bootstrap->on_protocol_negotiated = NULL;
    aws_ref_count_init(
        &bootstrap->ref_count, bootstrap, (aws_simple_completion_callback *)s_client_bootstrap_destroy_impl);
    bootstrap->host_resolver = aws_host_resolver_acquire(options->host_resolver);
    bootstrap->on_shutdown_complete = options->on_shutdown_complete;
    bootstrap->user_data = options->user_data;

    if (options->host_resolution_config) {
        bootstrap->host_resolver_config = *options->host_resolution_config;
    } else {
        bootstrap->host_resolver_config = aws_host_resolver_init_default_resolution_config();
    }

    return bootstrap;
}

int aws_client_bootstrap_set_alpn_callback(
    struct aws_client_bootstrap *bootstrap,
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated) {
    AWS_ASSERT(on_protocol_negotiated);

    AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: Setting ALPN callback", (void *)bootstrap);
    bootstrap->on_protocol_negotiated = on_protocol_negotiated;
    return AWS_OP_SUCCESS;
}

struct client_channel_data {
    struct aws_channel *channel;
    struct aws_socket *socket;
    struct aws_tls_connection_options tls_options;
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated;
    aws_tls_on_data_read_fn *user_on_data_read;
    aws_tls_on_negotiation_result_fn *user_on_negotiation_result;
    aws_tls_on_error_fn *user_on_error;
    void *tls_user_data;
    bool use_tls;
};

struct client_connection_args {
    struct aws_client_bootstrap *bootstrap;
    aws_client_bootstrap_on_channel_event_fn *creation_callback;
    aws_client_bootstrap_on_channel_event_fn *setup_callback;
    aws_client_bootstrap_on_channel_event_fn *shutdown_callback;
    struct client_channel_data channel_data;
    struct aws_socket_options outgoing_options;
    uint32_t outgoing_port;
    struct aws_string *host_name;
    void *user_data;
    uint8_t addresses_count;
    uint8_t failed_count;
    bool connection_chosen;
    bool setup_called;
    bool enable_read_back_pressure;
    struct aws_event_loop *requested_event_loop;

    /*
     * It is likely that all reference adjustments to the connection args take place in a single event loop
     * thread and are thus thread-safe. I can imagine some complex future scenarios where that might not hold true
     * and so it seems reasonable to switch now to a safe pattern.
     *
     */
    struct aws_ref_count ref_count;
};

static struct client_connection_args *s_client_connection_args_acquire(struct client_connection_args *args) {
    if (args != NULL) {
        AWS_LOGF_TRACE(AWS_LS_IO_CHANNEL_BOOTSTRAP, "acquiring client connection args, args=%p", (void *)args);
        aws_ref_count_acquire(&args->ref_count);
    }

    return args;
}

static void s_client_connection_args_destroy(struct client_connection_args *args) {
    AWS_ASSERT(args);
    AWS_LOGF_TRACE(AWS_LS_IO_CHANNEL_BOOTSTRAP, "destroying client connection args, args=%p", (void *)args);

    struct aws_allocator *allocator = args->bootstrap->allocator;
    aws_client_bootstrap_release(args->bootstrap);
    if (args->host_name) {
        aws_string_destroy(args->host_name);
    }

    if (args->channel_data.use_tls) {
        aws_tls_connection_options_clean_up(&args->channel_data.tls_options);
    }

    aws_mem_release(allocator, args);
}

static void s_client_connection_args_release(struct client_connection_args *args) {
    if (args != NULL) {
        AWS_LOGF_TRACE(AWS_LS_IO_CHANNEL_BOOTSTRAP, "releasing client connection args, args=%p", (void *)args);
        aws_ref_count_release(&args->ref_count);
    }
}

static struct aws_event_loop *s_get_connection_event_loop(struct client_connection_args *args) {
    if (args == NULL) {
        return NULL;
    }

    if (args->requested_event_loop != NULL) {
        return args->requested_event_loop;
    }

    return aws_event_loop_group_get_next_loop(args->bootstrap->event_loop_group);
}

static void s_connect_args_setup_callback_safe(
    struct client_connection_args *args,
    int error_code,
    struct aws_channel *channel) {

    AWS_FATAL_ASSERT(
        (args->requested_event_loop == NULL) || aws_event_loop_thread_is_callers_thread(args->requested_event_loop));

    /* setup_callback is always called exactly once */
    AWS_FATAL_ASSERT(!args->setup_called);

    AWS_ASSERT((error_code == AWS_OP_SUCCESS) == (channel != NULL));
    aws_client_bootstrap_on_channel_event_fn *setup_callback = args->setup_callback;
    setup_callback(args->bootstrap, error_code, channel, args->user_data);
    args->setup_called = true;
    /* if setup_callback is called with an error, we will not call shutdown_callback */
    if (error_code) {
        args->shutdown_callback = NULL;
    }
    s_client_connection_args_release(args);
}

struct aws_connection_args_setup_callback_task {
    struct aws_allocator *allocator;
    struct aws_task task;
    struct client_connection_args *args;
    int error_code;
    struct aws_channel *channel;
};

static void s_aws_connection_args_setup_callback_task_delete(struct aws_connection_args_setup_callback_task *task) {
    if (task == NULL) {
        return;
    }

    s_client_connection_args_release(task->args);
    if (task->channel) {
        aws_channel_release_hold(task->channel);
    }

    aws_mem_release(task->allocator, task);
}

void s_aws_connection_args_setup_callback_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_connection_args_setup_callback_task *callback_task = arg;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        s_connect_args_setup_callback_safe(callback_task->args, callback_task->error_code, callback_task->channel);
    }

    s_aws_connection_args_setup_callback_task_delete(callback_task);
}

static struct aws_connection_args_setup_callback_task *s_aws_connection_args_setup_callback_task_new(
    struct aws_allocator *allocator,
    struct client_connection_args *args,
    int error_code,
    struct aws_channel *channel) {

    struct aws_connection_args_setup_callback_task *task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_connection_args_setup_callback_task));
    task->allocator = allocator;
    task->args = s_client_connection_args_acquire(args);
    task->error_code = error_code;
    task->channel = channel;
    if (channel != NULL) {
        aws_channel_acquire_hold(channel);
    }

    aws_task_init(
        &task->task, s_aws_connection_args_setup_callback_task_fn, task, "safe connection args setup callback");

    return task;
}

static void s_connection_args_setup_callback(
    struct client_connection_args *args,
    int error_code,
    struct aws_channel *channel) {

    if (args->requested_event_loop == NULL || aws_event_loop_thread_is_callers_thread(args->requested_event_loop)) {
        s_connect_args_setup_callback_safe(args, error_code, channel);
    } else {
        struct aws_connection_args_setup_callback_task *callback_task =
            s_aws_connection_args_setup_callback_task_new(args->bootstrap->allocator, args, error_code, channel);
        aws_event_loop_schedule_task_now(args->requested_event_loop, &callback_task->task);
    }
}

static void s_connection_args_creation_callback(struct client_connection_args *args, struct aws_channel *channel) {

    AWS_FATAL_ASSERT(channel != NULL);

    if (args->creation_callback) {
        args->creation_callback(args->bootstrap, AWS_ERROR_SUCCESS, channel, args->user_data);
    }
}

static void s_connection_args_shutdown_callback(
    struct client_connection_args *args,
    int error_code,
    struct aws_channel *channel) {

    if (!args->setup_called) {
        /* if setup_callback was not called yet, an error occurred, ensure we tell the user *SOMETHING* */
        error_code = (error_code) ? error_code : AWS_ERROR_UNKNOWN;
        s_connection_args_setup_callback(args, error_code, NULL);
        return;
    }

    aws_client_bootstrap_on_channel_event_fn *shutdown_callback = args->shutdown_callback;
    if (shutdown_callback) {
        shutdown_callback(args->bootstrap, error_code, channel, args->user_data);
    }
}

static void s_tls_client_on_negotiation_result(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err_code,
    void *user_data) {
    struct client_connection_args *connection_args = user_data;

    if (connection_args->channel_data.user_on_negotiation_result) {
        connection_args->channel_data.user_on_negotiation_result(
            handler, slot, err_code, connection_args->channel_data.tls_user_data);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: tls negotiation result %d on channel %p",
        (void *)connection_args->bootstrap,
        err_code,
        (void *)slot->channel);

    /* if an error occurred, the user callback will be delivered in shutdown */
    if (err_code) {
        aws_channel_shutdown(slot->channel, err_code);
        return;
    }

    struct aws_channel *channel = connection_args->channel_data.channel;
    s_connection_args_setup_callback(connection_args, AWS_ERROR_SUCCESS, channel);
}

/* in the context of a channel bootstrap, we don't care about these, but since we're hooking into these APIs we have to
 * provide a proxy for the user actually receiving their callbacks. */
static void s_tls_client_on_data_read(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *buffer,
    void *user_data) {
    struct client_connection_args *connection_args = user_data;

    if (connection_args->channel_data.user_on_data_read) {
        connection_args->channel_data.user_on_data_read(
            handler, slot, buffer, connection_args->channel_data.tls_user_data);
    }
}

/* in the context of a channel bootstrap, we don't care about these, but since we're hooking into these APIs we have to
 * provide a proxy for the user actually receiving their callbacks. */
static void s_tls_client_on_error(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err,
    const char *message,
    void *user_data) {
    struct client_connection_args *connection_args = user_data;

    if (connection_args->channel_data.user_on_error) {
        connection_args->channel_data.user_on_error(
            handler, slot, err, message, connection_args->channel_data.tls_user_data);
    }
}

static inline int s_setup_client_tls(struct client_connection_args *connection_args, struct aws_channel *channel) {
    struct aws_channel_slot *tls_slot = aws_channel_slot_new(channel);

    /* as far as cleanup goes, since this stuff is being added to a channel, the caller will free this memory
       when they clean up the channel. */
    if (!tls_slot) {
        return AWS_OP_ERR;
    }

    struct aws_channel_handler *tls_handler = aws_tls_client_handler_new(
        connection_args->bootstrap->allocator, &connection_args->channel_data.tls_options, tls_slot);

    if (!tls_handler) {
        aws_mem_release(connection_args->bootstrap->allocator, (void *)tls_slot);
        return AWS_OP_ERR;
    }

    aws_channel_slot_insert_end(channel, tls_slot);
    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Setting up client TLS on channel %p with handler %p on slot %p",
        (void *)connection_args->bootstrap,
        (void *)channel,
        (void *)tls_handler,
        (void *)tls_slot);

    if (aws_channel_slot_set_handler(tls_slot, tls_handler) != AWS_OP_SUCCESS) {
        return AWS_OP_ERR;
    }

    if (connection_args->channel_data.on_protocol_negotiated) {
        struct aws_channel_slot *alpn_slot = aws_channel_slot_new(channel);

        if (!alpn_slot) {
            return AWS_OP_ERR;
        }

        struct aws_channel_handler *alpn_handler = aws_tls_alpn_handler_new(
            connection_args->bootstrap->allocator,
            connection_args->channel_data.on_protocol_negotiated,
            connection_args->user_data);

        if (!alpn_handler) {
            aws_mem_release(connection_args->bootstrap->allocator, (void *)alpn_slot);
            return AWS_OP_ERR;
        }

        AWS_LOGF_TRACE(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: Setting up ALPN handler on channel "
            "%p with handler %p on slot %p",
            (void *)connection_args->bootstrap,
            (void *)channel,
            (void *)alpn_handler,
            (void *)alpn_slot);

        aws_channel_slot_insert_right(tls_slot, alpn_slot);
        if (aws_channel_slot_set_handler(alpn_slot, alpn_handler) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }
    }

    if (aws_tls_client_handler_start_negotiation(tls_handler) != AWS_OP_SUCCESS) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_on_client_channel_on_setup_completed(struct aws_channel *channel, int error_code, void *user_data) {
    struct client_connection_args *connection_args = user_data;
    int err_code = error_code;

    if (!err_code) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: channel %p setup succeeded: bootstrapping.",
            (void *)connection_args->bootstrap,
            (void *)channel);

        struct aws_channel_slot *socket_slot = aws_channel_slot_new(channel);

        if (!socket_slot) {
            err_code = aws_last_error();
            goto error;
        }

        struct aws_channel_handler *socket_channel_handler = aws_socket_handler_new(
            connection_args->bootstrap->allocator,
            connection_args->channel_data.socket,
            socket_slot,
            g_aws_channel_max_fragment_size);

        if (!socket_channel_handler) {
            err_code = aws_last_error();
            aws_channel_slot_remove(socket_slot);
            socket_slot = NULL;
            goto error;
        }

        AWS_LOGF_TRACE(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: Setting up socket handler on channel "
            "%p with handler %p on slot %p.",
            (void *)connection_args->bootstrap,
            (void *)channel,
            (void *)socket_channel_handler,
            (void *)socket_slot);

        if (aws_channel_slot_set_handler(socket_slot, socket_channel_handler)) {
            err_code = aws_last_error();
            goto error;
        }

        if (connection_args->channel_data.use_tls) {
            /* we don't want to notify the user that the channel is ready yet, since tls is still negotiating, wait
             * for the negotiation callback and handle it then.*/
            if (s_setup_client_tls(connection_args, channel)) {
                err_code = aws_last_error();
                goto error;
            }
        } else {
            s_connection_args_setup_callback(connection_args, AWS_OP_SUCCESS, channel);
        }

        return;
    }

error:
    AWS_LOGF_ERROR(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: channel %p setup failed with error %d.",
        (void *)connection_args->bootstrap,
        (void *)channel,
        err_code);
    aws_channel_shutdown(channel, err_code);
    /* the channel shutdown callback will clean the channel up */
}

static void s_on_client_channel_on_shutdown(struct aws_channel *channel, int error_code, void *user_data) {
    struct client_connection_args *connection_args = user_data;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: channel %p shutdown with error %d.",
        (void *)connection_args->bootstrap,
        (void *)channel,
        error_code);

    /* note it's not safe to reference the bootstrap after the callback. */
    struct aws_allocator *allocator = connection_args->bootstrap->allocator;
    s_connection_args_shutdown_callback(connection_args, error_code, channel);

    aws_channel_destroy(channel);
    aws_socket_clean_up(connection_args->channel_data.socket);
    aws_mem_release(allocator, connection_args->channel_data.socket);
    s_client_connection_args_release(connection_args);
}

static bool s_aws_socket_domain_uses_dns(enum aws_socket_domain domain) {
    return domain == AWS_SOCKET_IPV4 || domain == AWS_SOCKET_IPV6;
}

static void s_on_client_connection_established(struct aws_socket *socket, int error_code, void *user_data) {
    struct client_connection_args *connection_args = user_data;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: client connection on socket %p completed with error %d.",
        (void *)connection_args->bootstrap,
        (void *)socket,
        error_code);

    if (error_code) {
        connection_args->failed_count++;
    }

    if (error_code || connection_args->connection_chosen) {
        if (s_aws_socket_domain_uses_dns(connection_args->outgoing_options.domain) && error_code) {
            struct aws_host_address host_address;
            host_address.host = connection_args->host_name;
            host_address.address =
                aws_string_new_from_c_str(connection_args->bootstrap->allocator, socket->remote_endpoint.address);
            host_address.record_type = connection_args->outgoing_options.domain == AWS_SOCKET_IPV6
                                           ? AWS_ADDRESS_RECORD_TYPE_AAAA
                                           : AWS_ADDRESS_RECORD_TYPE_A;

            if (host_address.address) {
                AWS_LOGF_DEBUG(
                    AWS_LS_IO_CHANNEL_BOOTSTRAP,
                    "id=%p: recording bad address %s.",
                    (void *)connection_args->bootstrap,
                    socket->remote_endpoint.address);
                aws_host_resolver_record_connection_failure(connection_args->bootstrap->host_resolver, &host_address);
                aws_string_destroy((void *)host_address.address);
            }
        }

        AWS_LOGF_TRACE(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: releasing socket %p either because we already have a "
            "successful connection or because it errored out.",
            (void *)connection_args->bootstrap,
            (void *)socket);
        aws_socket_close(socket);

        aws_socket_clean_up(socket);
        aws_mem_release(connection_args->bootstrap->allocator, socket);

        /* if this is the last attempted connection and it failed, notify the user */
        if (connection_args->failed_count == connection_args->addresses_count) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_CHANNEL_BOOTSTRAP,
                "id=%p: Connection failed with error_code %d.",
                (void *)connection_args->bootstrap,
                error_code);
            /* connection_args will be released after setup_callback */
            s_connection_args_setup_callback(connection_args, error_code, NULL);
        }

        /* every connection task adds a ref, so every failure or cancel needs to dec one */
        s_client_connection_args_release(connection_args);
        return;
    }

    connection_args->connection_chosen = true;
    connection_args->channel_data.socket = socket;

    struct aws_channel_options args = {
        .on_setup_completed = s_on_client_channel_on_setup_completed,
        .setup_user_data = connection_args,
        .shutdown_user_data = connection_args,
        .on_shutdown_completed = s_on_client_channel_on_shutdown,
    };

    args.enable_read_back_pressure = connection_args->enable_read_back_pressure;
    args.event_loop = aws_socket_get_event_loop(socket);

    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Successful connection, creating a new channel using socket %p.",
        (void *)connection_args->bootstrap,
        (void *)socket);

    connection_args->channel_data.channel = aws_channel_new(connection_args->bootstrap->allocator, &args);

    if (!connection_args->channel_data.channel) {
        aws_socket_clean_up(socket);
        aws_mem_release(connection_args->bootstrap->allocator, connection_args->channel_data.socket);
        connection_args->failed_count++;

        /* if this is the last attempted connection and it failed, notify the user */
        if (connection_args->failed_count == connection_args->addresses_count) {
            s_connection_args_setup_callback(connection_args, error_code, NULL);
        }
    } else {
        s_connection_args_creation_callback(connection_args, connection_args->channel_data.channel);
    }
}

struct connection_task_data {
    struct aws_task task;
    struct aws_socket_endpoint endpoint;
    struct aws_socket_options options;
    struct aws_host_address host_address;
    struct client_connection_args *args;
    struct aws_event_loop *connect_loop;
};

static void s_attempt_connection(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct connection_task_data *task_data = arg;
    struct aws_allocator *allocator = task_data->args->bootstrap->allocator;
    int err_code = 0;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto task_cancelled;
    }

    struct aws_socket *outgoing_socket = aws_mem_acquire(allocator, sizeof(struct aws_socket));
    if (aws_socket_init(outgoing_socket, allocator, &task_data->options)) {
        goto socket_init_failed;
    }

    if (aws_socket_connect(
            outgoing_socket,
            &task_data->endpoint,
            task_data->connect_loop,
            s_on_client_connection_established,
            task_data->args)) {

        goto socket_connect_failed;
    }

    goto cleanup_task;

socket_connect_failed:
    aws_host_resolver_record_connection_failure(task_data->args->bootstrap->host_resolver, &task_data->host_address);
    aws_socket_clean_up(outgoing_socket);
socket_init_failed:
    aws_mem_release(allocator, outgoing_socket);
task_cancelled:
    err_code = aws_last_error();
    task_data->args->failed_count++;
    /* if this is the last attempted connection and it failed, notify the user */
    if (task_data->args->failed_count == task_data->args->addresses_count) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: Last attempt failed to create socket with error %d",
            (void *)task_data->args->bootstrap,
            err_code);
        s_connection_args_setup_callback(task_data->args, err_code, NULL);
    } else {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: Socket connect attempt %d/%d failed with error %d. More attempts ongoing...",
            (void *)task_data->args->bootstrap,
            task_data->args->failed_count,
            task_data->args->addresses_count,
            err_code);
    }

    s_client_connection_args_release(task_data->args);

cleanup_task:
    aws_host_address_clean_up(&task_data->host_address);
    aws_mem_release(allocator, task_data);
}

static void s_on_host_resolved(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    int err_code,
    const struct aws_array_list *host_addresses,
    void *user_data) {
    (void)resolver;
    (void)host_name;

    struct client_connection_args *client_connection_args = user_data;
    struct aws_allocator *allocator = client_connection_args->bootstrap->allocator;

    if (err_code) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: dns resolution failed, or all socket connections to the endpoint failed.",
            (void *)client_connection_args->bootstrap);
        s_connection_args_setup_callback(client_connection_args, err_code, NULL);
        return;
    }

    size_t host_addresses_len = aws_array_list_length(host_addresses);
    AWS_FATAL_ASSERT(host_addresses_len > 0);
    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: dns resolution completed. Kicking off connections"
        " on %llu addresses. First one back wins.",
        (void *)client_connection_args->bootstrap,
        (unsigned long long)host_addresses_len);
    /* use this event loop for all outgoing connection attempts (only one will ultimately win). */
    struct aws_event_loop *connect_loop = s_get_connection_event_loop(client_connection_args);
    client_connection_args->addresses_count = (uint8_t)host_addresses_len;

    /* allocate all the task data first, in case it fails... */
    AWS_VARIABLE_LENGTH_ARRAY(struct connection_task_data *, tasks, host_addresses_len);
    for (size_t i = 0; i < host_addresses_len; ++i) {
        struct connection_task_data *task_data = tasks[i] =
            aws_mem_calloc(allocator, 1, sizeof(struct connection_task_data));
        bool failed = task_data == NULL;
        if (!failed) {
            struct aws_host_address *host_address_ptr = NULL;
            aws_array_list_get_at_ptr(host_addresses, (void **)&host_address_ptr, i);

            task_data->endpoint.port = client_connection_args->outgoing_port;
            AWS_ASSERT(sizeof(task_data->endpoint.address) >= host_address_ptr->address->len + 1);
            memcpy(
                task_data->endpoint.address,
                aws_string_bytes(host_address_ptr->address),
                host_address_ptr->address->len);
            task_data->endpoint.address[host_address_ptr->address->len] = 0;

            task_data->options = client_connection_args->outgoing_options;
            task_data->options.domain =
                host_address_ptr->record_type == AWS_ADDRESS_RECORD_TYPE_AAAA ? AWS_SOCKET_IPV6 : AWS_SOCKET_IPV4;

            failed = aws_host_address_copy(host_address_ptr, &task_data->host_address) != AWS_OP_SUCCESS;
            task_data->args = client_connection_args;
            task_data->connect_loop = connect_loop;
        }

        if (failed) {
            for (size_t j = 0; j <= i; ++j) {
                if (tasks[j]) {
                    aws_host_address_clean_up(&tasks[j]->host_address);
                    aws_mem_release(allocator, tasks[j]);
                }
            }
            int alloc_err_code = aws_last_error();
            AWS_LOGF_ERROR(
                AWS_LS_IO_CHANNEL_BOOTSTRAP,
                "id=%p: failed to allocate connection task data: err=%d",
                (void *)client_connection_args->bootstrap,
                alloc_err_code);
            s_connection_args_setup_callback(client_connection_args, alloc_err_code, NULL);
            return;
        }
    }

    /* ...then schedule all the tasks, which cannot fail */
    for (size_t i = 0; i < host_addresses_len; ++i) {
        struct connection_task_data *task_data = tasks[i];
        /**
         * Acquire on the connection args to make sure connection args outlive the tasks to attempt connection.
         *
         * Once upon a time, the connection attempt tasks were scheduled right after acquiring the connection args,
         * which lead to a crash that when the attempt connection tasks run and the attempt connection succeed and
         * closed before the other tasks can acquire on the connection args, the connection args had be destroyed before
         * acquire and lead to a crash.
         */
        s_client_connection_args_acquire(task_data->args);
    }

    for (size_t i = 0; i < host_addresses_len; ++i) {
        struct connection_task_data *task_data = tasks[i];
        aws_task_init(&task_data->task, s_attempt_connection, task_data, "attempt_connection");
        aws_event_loop_schedule_task_now(connect_loop, &task_data->task);
    }
}

static bool s_does_event_loop_belong_to_event_loop_group(
    struct aws_event_loop *loop,
    struct aws_event_loop_group *elg) {
    if (loop == NULL || elg == NULL) {
        return false;
    }

    size_t loop_count = aws_event_loop_group_get_loop_count(elg);
    for (size_t i = 0; i < loop_count; ++i) {
        struct aws_event_loop *elg_loop = aws_event_loop_group_get_loop_at(elg, i);
        if (elg_loop == loop) {
            return true;
        }
    }

    return false;
}

int aws_client_bootstrap_new_socket_channel(struct aws_socket_channel_bootstrap_options *options) {

    struct aws_client_bootstrap *bootstrap = options->bootstrap;
    AWS_FATAL_ASSERT(options->setup_callback);
    AWS_FATAL_ASSERT(options->shutdown_callback);
    AWS_FATAL_ASSERT(bootstrap);

    const struct aws_socket_options *socket_options = options->socket_options;
    AWS_FATAL_ASSERT(socket_options != NULL);

    const struct aws_tls_connection_options *tls_options = options->tls_options;

    AWS_FATAL_ASSERT(tls_options == NULL || socket_options->type == AWS_SOCKET_STREAM);
    aws_io_fatal_assert_library_initialized();

    if (options->requested_event_loop != NULL) {
        /* If we're asking for a specific event loop, verify it belongs to the bootstrap's event loop group */
        if (!(s_does_event_loop_belong_to_event_loop_group(
                options->requested_event_loop, bootstrap->event_loop_group))) {
            return aws_raise_error(AWS_ERROR_IO_PINNED_EVENT_LOOP_MISMATCH);
        }
    }

    struct client_connection_args *client_connection_args =
        aws_mem_calloc(bootstrap->allocator, 1, sizeof(struct client_connection_args));

    if (!client_connection_args) {
        return AWS_OP_ERR;
    }

    const char *host_name = options->host_name;
    uint32_t port = options->port;

    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: attempting to initialize a new client channel to %s:%u",
        (void *)bootstrap,
        host_name,
        port);

    aws_ref_count_init(
        &client_connection_args->ref_count,
        client_connection_args,
        (aws_simple_completion_callback *)s_client_connection_args_destroy);
    client_connection_args->user_data = options->user_data;
    client_connection_args->bootstrap = aws_client_bootstrap_acquire(bootstrap);
    client_connection_args->creation_callback = options->creation_callback;
    client_connection_args->setup_callback = options->setup_callback;
    client_connection_args->shutdown_callback = options->shutdown_callback;
    client_connection_args->outgoing_options = *socket_options;
    client_connection_args->outgoing_port = port;
    client_connection_args->enable_read_back_pressure = options->enable_read_back_pressure;
    client_connection_args->requested_event_loop = options->requested_event_loop;

    if (tls_options) {
        if (aws_tls_connection_options_copy(&client_connection_args->channel_data.tls_options, tls_options)) {
            goto error;
        }
        client_connection_args->channel_data.use_tls = true;

        client_connection_args->channel_data.on_protocol_negotiated = bootstrap->on_protocol_negotiated;
        client_connection_args->channel_data.tls_user_data = tls_options->user_data;

        /* in order to honor any callbacks a user may have installed on their tls_connection_options,
         * we need to wrap them if they were set.*/
        if (bootstrap->on_protocol_negotiated) {
            client_connection_args->channel_data.tls_options.advertise_alpn_message = true;
        }

        if (tls_options->on_data_read) {
            client_connection_args->channel_data.user_on_data_read = tls_options->on_data_read;
            client_connection_args->channel_data.tls_options.on_data_read = s_tls_client_on_data_read;
        }

        if (tls_options->on_error) {
            client_connection_args->channel_data.user_on_error = tls_options->on_error;
            client_connection_args->channel_data.tls_options.on_error = s_tls_client_on_error;
        }

        if (tls_options->on_negotiation_result) {
            client_connection_args->channel_data.user_on_negotiation_result = tls_options->on_negotiation_result;
        }

        client_connection_args->channel_data.tls_options.on_negotiation_result = s_tls_client_on_negotiation_result;
        client_connection_args->channel_data.tls_options.user_data = client_connection_args;
    }

    if (s_aws_socket_domain_uses_dns(socket_options->domain)) {
        client_connection_args->host_name = aws_string_new_from_c_str(bootstrap->allocator, host_name);

        if (!client_connection_args->host_name) {
            goto error;
        }

        const struct aws_host_resolution_config *host_resolution_config = &bootstrap->host_resolver_config;
        if (options->host_resolution_override_config) {
            host_resolution_config = options->host_resolution_override_config;
        }

        if (aws_host_resolver_resolve_host(
                bootstrap->host_resolver,
                client_connection_args->host_name,
                s_on_host_resolved,
                host_resolution_config,
                client_connection_args)) {
            goto error;
        }
    } else {
        /* ensure that the pipe/domain socket name will fit in the endpoint address */
        const size_t host_name_len = strlen(host_name);
        if (host_name_len >= AWS_ADDRESS_MAX_LEN) {
            aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
            goto error;
        }

        struct aws_socket_endpoint endpoint;
        AWS_ZERO_STRUCT(endpoint);
        memcpy(endpoint.address, host_name, host_name_len);
        if (socket_options->domain == AWS_SOCKET_VSOCK) {
            endpoint.port = port;
        } else {
            endpoint.port = 0;
        }

        struct aws_socket *outgoing_socket = aws_mem_acquire(bootstrap->allocator, sizeof(struct aws_socket));

        if (!outgoing_socket) {
            goto error;
        }

        if (aws_socket_init(outgoing_socket, bootstrap->allocator, socket_options)) {
            aws_mem_release(bootstrap->allocator, outgoing_socket);
            goto error;
        }

        client_connection_args->addresses_count = 1;

        struct aws_event_loop *connect_loop = s_get_connection_event_loop(client_connection_args);

        s_client_connection_args_acquire(client_connection_args);
        if (aws_socket_connect(
                outgoing_socket, &endpoint, connect_loop, s_on_client_connection_established, client_connection_args)) {
            aws_socket_clean_up(outgoing_socket);
            aws_mem_release(client_connection_args->bootstrap->allocator, outgoing_socket);
            s_client_connection_args_release(client_connection_args);
            goto error;
        }
    }

    return AWS_OP_SUCCESS;

error:
    if (client_connection_args) {
        /* tls opt will also be freed when we clean up the connection arg */
        s_client_connection_args_release(client_connection_args);
    }
    return AWS_OP_ERR;
}

void s_server_bootstrap_destroy_impl(struct aws_server_bootstrap *bootstrap) {
    AWS_ASSERT(bootstrap);
    aws_event_loop_group_release(bootstrap->event_loop_group);
    aws_mem_release(bootstrap->allocator, bootstrap);
}

struct aws_server_bootstrap *aws_server_bootstrap_acquire(struct aws_server_bootstrap *bootstrap) {
    if (bootstrap != NULL) {
        aws_ref_count_acquire(&bootstrap->ref_count);
    }

    return bootstrap;
}

void aws_server_bootstrap_release(struct aws_server_bootstrap *bootstrap) {
    /* if destroy is being called, the user intends to not use the bootstrap anymore
     * so we clean up the thread local state while the event loop thread is
     * still alive */
    AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: releasing server bootstrap reference", (void *)bootstrap);
    if (bootstrap != NULL) {
        aws_ref_count_release(&bootstrap->ref_count);
    }
}

struct aws_server_bootstrap *aws_server_bootstrap_new(
    struct aws_allocator *allocator,
    struct aws_event_loop_group *el_group) {
    AWS_ASSERT(allocator);
    AWS_ASSERT(el_group);

    struct aws_server_bootstrap *bootstrap = aws_mem_calloc(allocator, 1, sizeof(struct aws_server_bootstrap));
    if (!bootstrap) {
        return NULL;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Initializing server bootstrap with event-loop group %p",
        (void *)bootstrap,
        (void *)el_group);

    bootstrap->allocator = allocator;
    bootstrap->event_loop_group = aws_event_loop_group_acquire(el_group);
    bootstrap->on_protocol_negotiated = NULL;
    aws_ref_count_init(
        &bootstrap->ref_count, bootstrap, (aws_simple_completion_callback *)s_server_bootstrap_destroy_impl);

    return bootstrap;
}

struct server_connection_args {
    struct aws_server_bootstrap *bootstrap;
    struct aws_socket listener;
    aws_server_bootstrap_on_accept_channel_setup_fn *incoming_callback;
    aws_server_bootstrap_on_accept_channel_shutdown_fn *shutdown_callback;
    aws_server_bootstrap_on_server_listener_destroy_fn *destroy_callback;
    struct aws_tls_connection_options tls_options;
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated;
    aws_tls_on_data_read_fn *user_on_data_read;
    aws_tls_on_negotiation_result_fn *user_on_negotiation_result;
    aws_tls_on_error_fn *user_on_error;
    struct aws_task listener_destroy_task;
    void *tls_user_data;
    void *user_data;
    bool use_tls;
    bool enable_read_back_pressure;
    struct aws_ref_count ref_count;
};

struct server_channel_data {
    struct aws_channel *channel;
    struct aws_socket *socket;
    struct server_connection_args *server_connection_args;
    bool incoming_called;
};

static struct server_connection_args *s_server_connection_args_acquire(struct server_connection_args *args) {
    if (args != NULL) {
        aws_ref_count_acquire(&args->ref_count);
    }

    return args;
}

static void s_server_connection_args_destroy(struct server_connection_args *args) {
    if (args == NULL) {
        return;
    }

    /* fire the destroy callback */
    if (args->destroy_callback) {
        args->destroy_callback(args->bootstrap, args->user_data);
    }

    struct aws_allocator *allocator = args->bootstrap->allocator;
    aws_server_bootstrap_release(args->bootstrap);
    if (args->use_tls) {
        aws_tls_connection_options_clean_up(&args->tls_options);
    }

    aws_mem_release(allocator, args);
}

static void s_server_connection_args_release(struct server_connection_args *args) {
    if (args != NULL) {
        aws_ref_count_release(&args->ref_count);
    }
}

static void s_server_incoming_callback(
    struct server_channel_data *channel_data,
    int error_code,
    struct aws_channel *channel) {
    /* incoming_callback is always called exactly once for each channel */
    AWS_ASSERT(!channel_data->incoming_called);
    struct server_connection_args *args = channel_data->server_connection_args;
    args->incoming_callback(args->bootstrap, error_code, channel, args->user_data);
    channel_data->incoming_called = true;
}

static void s_tls_server_on_negotiation_result(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err_code,
    void *user_data) {
    struct server_channel_data *channel_data = user_data;
    struct server_connection_args *connection_args = channel_data->server_connection_args;

    if (connection_args->user_on_negotiation_result) {
        connection_args->user_on_negotiation_result(handler, slot, err_code, connection_args->tls_user_data);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: tls negotiation result %d on channel %p",
        (void *)connection_args->bootstrap,
        err_code,
        (void *)slot->channel);

    struct aws_channel *channel = slot->channel;
    if (err_code) {
        /* shut down the channel */
        aws_channel_shutdown(channel, err_code);
    } else {
        s_server_incoming_callback(channel_data, err_code, channel);
    }
}

/* in the context of a channel bootstrap, we don't care about these, but since we're hooking into these APIs we have to
 * provide a proxy for the user actually receiving their callbacks. */
static void s_tls_server_on_data_read(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *buffer,
    void *user_data) {
    struct server_connection_args *connection_args = user_data;

    if (connection_args->user_on_data_read) {
        connection_args->user_on_data_read(handler, slot, buffer, connection_args->tls_user_data);
    }
}

/* in the context of a channel bootstrap, we don't care about these, but since we're hooking into these APIs we have to
 * provide a proxy for the user actually receiving their callbacks. */
static void s_tls_server_on_error(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err,
    const char *message,
    void *user_data) {
    struct server_connection_args *connection_args = user_data;

    if (connection_args->user_on_error) {
        connection_args->user_on_error(handler, slot, err, message, connection_args->tls_user_data);
    }
}

static inline int s_setup_server_tls(struct server_channel_data *channel_data, struct aws_channel *channel) {
    struct aws_channel_slot *tls_slot = NULL;
    struct aws_channel_handler *tls_handler = NULL;
    struct server_connection_args *connection_args = channel_data->server_connection_args;

    /* as far as cleanup goes here, since we're adding things to a channel, if a slot is ever successfully
       added to the channel, we leave it there. The caller will clean up the channel and it will clean this memory
       up as well. */
    tls_slot = aws_channel_slot_new(channel);

    if (!tls_slot) {
        return AWS_OP_ERR;
    }

    /* Shallow-copy tls_options so we can override the user_data, making it specific to this channel */
    struct aws_tls_connection_options tls_options = connection_args->tls_options;
    tls_options.user_data = channel_data;
    tls_handler = aws_tls_server_handler_new(connection_args->bootstrap->allocator, &tls_options, tls_slot);

    if (!tls_handler) {
        aws_mem_release(connection_args->bootstrap->allocator, tls_slot);
        return AWS_OP_ERR;
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Setting up server TLS on channel %p with handler %p on slot %p",
        (void *)connection_args->bootstrap,
        (void *)channel,
        (void *)tls_handler,
        (void *)tls_slot);

    aws_channel_slot_insert_end(channel, tls_slot);

    if (aws_channel_slot_set_handler(tls_slot, tls_handler)) {
        return AWS_OP_ERR;
    }

    if (connection_args->on_protocol_negotiated) {
        struct aws_channel_slot *alpn_slot = NULL;
        struct aws_channel_handler *alpn_handler = NULL;
        alpn_slot = aws_channel_slot_new(channel);

        if (!alpn_slot) {
            return AWS_OP_ERR;
        }

        alpn_handler = aws_tls_alpn_handler_new(
            connection_args->bootstrap->allocator, connection_args->on_protocol_negotiated, connection_args->user_data);

        if (!alpn_handler) {
            aws_channel_slot_remove(alpn_slot);
            return AWS_OP_ERR;
        }

        AWS_LOGF_TRACE(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: Setting up ALPN handler on channel "
            "%p with handler %p on slot %p",
            (void *)connection_args->bootstrap,
            (void *)channel,
            (void *)alpn_handler,
            (void *)alpn_slot);

        aws_channel_slot_insert_right(tls_slot, alpn_slot);

        if (aws_channel_slot_set_handler(alpn_slot, alpn_handler)) {
            return AWS_OP_ERR;
        }
    }

    /*
     * Server-side channels can reach this point in execution and actually have the CLIENT_HELLO payload already
     * on the socket in a signalled state, but there was no socket handler or read handler at the time of signal.
     * So we need to manually trigger a read here to cover that case, otherwise the negotiation will time out because
     * we will not receive any more data/notifications (unless we read and react).
     */
    if (aws_channel_trigger_read(channel)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_on_server_channel_on_setup_completed(struct aws_channel *channel, int error_code, void *user_data) {
    struct server_channel_data *channel_data = user_data;

    int err_code = error_code;
    if (err_code) {
        /* channel fail to set up no destroy callback will fire */
        AWS_LOGF_ERROR(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: channel %p setup failed with error %d.",
            (void *)channel_data->server_connection_args->bootstrap,
            (void *)channel,
            err_code);

        aws_channel_destroy(channel);
        struct aws_allocator *allocator = channel_data->socket->allocator;
        aws_socket_clean_up(channel_data->socket);
        aws_mem_release(allocator, (void *)channel_data->socket);
        s_server_incoming_callback(channel_data, err_code, NULL);
        aws_mem_release(channel_data->server_connection_args->bootstrap->allocator, channel_data);
        /* no shutdown call back will be fired, we release the ref_count of connection arg here */
        s_server_connection_args_release(channel_data->server_connection_args);
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: channel %p setup succeeded: bootstrapping.",
        (void *)channel_data->server_connection_args->bootstrap,
        (void *)channel);

    struct aws_channel_slot *socket_slot = aws_channel_slot_new(channel);

    if (!socket_slot) {
        err_code = aws_last_error();
        goto error;
    }

    struct aws_channel_handler *socket_channel_handler = aws_socket_handler_new(
        channel_data->server_connection_args->bootstrap->allocator,
        channel_data->socket,
        socket_slot,
        g_aws_channel_max_fragment_size);

    if (!socket_channel_handler) {
        err_code = aws_last_error();
        aws_channel_slot_remove(socket_slot);
        socket_slot = NULL;
        goto error;
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: Setting up socket handler on channel "
        "%p with handler %p on slot %p.",
        (void *)channel_data->server_connection_args->bootstrap,
        (void *)channel,
        (void *)socket_channel_handler,
        (void *)socket_slot);

    if (aws_channel_slot_set_handler(socket_slot, socket_channel_handler)) {
        err_code = aws_last_error();
        goto error;
    }

    if (channel_data->server_connection_args->use_tls) {
        /* incoming callback will be invoked upon the negotiation completion so don't do it
         * here. */
        if (s_setup_server_tls(channel_data, channel)) {
            err_code = aws_last_error();
            goto error;
        }
    } else {
        s_server_incoming_callback(channel_data, AWS_OP_SUCCESS, channel);
    }
    return;

error:
    /* shut down the channel */
    aws_channel_shutdown(channel, err_code);
}

static void s_on_server_channel_on_shutdown(struct aws_channel *channel, int error_code, void *user_data) {
    struct server_channel_data *channel_data = user_data;
    struct server_connection_args *args = channel_data->server_connection_args;
    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: channel %p shutdown with error %d.",
        (void *)args->bootstrap,
        (void *)channel,
        error_code);

    void *server_shutdown_user_data = args->user_data;
    struct aws_server_bootstrap *server_bootstrap = args->bootstrap;
    struct aws_allocator *allocator = server_bootstrap->allocator;

    if (!channel_data->incoming_called) {
        error_code = (error_code) ? error_code : AWS_ERROR_UNKNOWN;
        s_server_incoming_callback(channel_data, error_code, NULL);
    } else {
        args->shutdown_callback(server_bootstrap, error_code, channel, server_shutdown_user_data);
    }

    aws_channel_destroy(channel);
    aws_socket_clean_up(channel_data->socket);
    aws_mem_release(allocator, channel_data->socket);
    s_server_connection_args_release(channel_data->server_connection_args);

    aws_mem_release(allocator, channel_data);
}

void s_on_server_connection_result(
    struct aws_socket *socket,
    int error_code,
    struct aws_socket *new_socket,
    void *user_data) {
    (void)socket;
    struct server_connection_args *connection_args = user_data;

    s_server_connection_args_acquire(connection_args);
    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: server connection on socket %p completed with error %d.",
        (void *)connection_args->bootstrap,
        (void *)socket,
        error_code);

    if (!error_code) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_CHANNEL_BOOTSTRAP,
            "id=%p: creating a new channel for incoming "
            "connection using socket %p.",
            (void *)connection_args->bootstrap,
            (void *)socket);
        struct server_channel_data *channel_data =
            aws_mem_calloc(connection_args->bootstrap->allocator, 1, sizeof(struct server_channel_data));
        if (!channel_data) {
            goto error_cleanup;
        }
        channel_data->incoming_called = false;
        channel_data->socket = new_socket;
        channel_data->server_connection_args = connection_args;

        struct aws_event_loop *event_loop =
            aws_event_loop_group_get_next_loop(connection_args->bootstrap->event_loop_group);

        struct aws_channel_options channel_args = {
            .on_setup_completed = s_on_server_channel_on_setup_completed,
            .setup_user_data = channel_data,
            .shutdown_user_data = channel_data,
            .on_shutdown_completed = s_on_server_channel_on_shutdown,
        };

        channel_args.event_loop = event_loop;
        channel_args.enable_read_back_pressure = channel_data->server_connection_args->enable_read_back_pressure;

        if (aws_socket_assign_to_event_loop(new_socket, event_loop)) {
            aws_mem_release(connection_args->bootstrap->allocator, (void *)channel_data);
            goto error_cleanup;
        }

        channel_data->channel = aws_channel_new(connection_args->bootstrap->allocator, &channel_args);

        if (!channel_data->channel) {
            aws_mem_release(connection_args->bootstrap->allocator, (void *)channel_data);
            goto error_cleanup;
        }
    } else {
        /* no channel is created */
        connection_args->incoming_callback(connection_args->bootstrap, error_code, NULL, connection_args->user_data);
        s_server_connection_args_release(connection_args);
    }

    return;

error_cleanup:
    /* no channel is created */
    connection_args->incoming_callback(connection_args->bootstrap, aws_last_error(), NULL, connection_args->user_data);

    struct aws_allocator *allocator = new_socket->allocator;
    aws_socket_clean_up(new_socket);
    aws_mem_release(allocator, (void *)new_socket);
    s_server_connection_args_release(connection_args);
}

static void s_listener_destroy_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)status;
    (void)task;
    struct server_connection_args *server_connection_args = arg;

    aws_socket_stop_accept(&server_connection_args->listener);
    aws_socket_clean_up(&server_connection_args->listener);
    s_server_connection_args_release(server_connection_args);
}

struct aws_socket *aws_server_bootstrap_new_socket_listener(
    const struct aws_server_socket_channel_bootstrap_options *bootstrap_options) {
    AWS_PRECONDITION(bootstrap_options);
    AWS_PRECONDITION(bootstrap_options->bootstrap);
    AWS_PRECONDITION(bootstrap_options->incoming_callback);
    AWS_PRECONDITION(bootstrap_options->shutdown_callback);

    struct server_connection_args *server_connection_args =
        aws_mem_calloc(bootstrap_options->bootstrap->allocator, 1, sizeof(struct server_connection_args));
    if (!server_connection_args) {
        return NULL;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "id=%p: attempting to initialize a new "
        "server socket listener for %s:%u",
        (void *)bootstrap_options->bootstrap,
        bootstrap_options->host_name,
        bootstrap_options->port);

    aws_ref_count_init(
        &server_connection_args->ref_count,
        server_connection_args,
        (aws_simple_completion_callback *)s_server_connection_args_destroy);
    server_connection_args->user_data = bootstrap_options->user_data;
    server_connection_args->bootstrap = aws_server_bootstrap_acquire(bootstrap_options->bootstrap);
    server_connection_args->shutdown_callback = bootstrap_options->shutdown_callback;
    server_connection_args->incoming_callback = bootstrap_options->incoming_callback;
    server_connection_args->destroy_callback = bootstrap_options->destroy_callback;
    server_connection_args->on_protocol_negotiated = bootstrap_options->bootstrap->on_protocol_negotiated;
    server_connection_args->enable_read_back_pressure = bootstrap_options->enable_read_back_pressure;

    aws_task_init(
        &server_connection_args->listener_destroy_task,
        s_listener_destroy_task,
        server_connection_args,
        "listener socket destroy");

    if (bootstrap_options->tls_options) {
        AWS_LOGF_INFO(
            AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: using tls on listener", (void *)bootstrap_options->tls_options);
        if (aws_tls_connection_options_copy(&server_connection_args->tls_options, bootstrap_options->tls_options)) {
            goto cleanup_server_connection_args;
        }

        server_connection_args->use_tls = true;

        server_connection_args->tls_user_data = bootstrap_options->tls_options->user_data;

        /* in order to honor any callbacks a user may have installed on their tls_connection_options,
         * we need to wrap them if they were set.*/
        if (bootstrap_options->bootstrap->on_protocol_negotiated) {
            server_connection_args->tls_options.advertise_alpn_message = true;
        }

        if (bootstrap_options->tls_options->on_data_read) {
            server_connection_args->user_on_data_read = bootstrap_options->tls_options->on_data_read;
            server_connection_args->tls_options.on_data_read = s_tls_server_on_data_read;
        }

        if (bootstrap_options->tls_options->on_error) {
            server_connection_args->user_on_error = bootstrap_options->tls_options->on_error;
            server_connection_args->tls_options.on_error = s_tls_server_on_error;
        }

        if (bootstrap_options->tls_options->on_negotiation_result) {
            server_connection_args->user_on_negotiation_result = bootstrap_options->tls_options->on_negotiation_result;
        }

        server_connection_args->tls_options.on_negotiation_result = s_tls_server_on_negotiation_result;
        server_connection_args->tls_options.user_data = server_connection_args;
    }

    struct aws_event_loop *connection_loop =
        aws_event_loop_group_get_next_loop(bootstrap_options->bootstrap->event_loop_group);

    if (aws_socket_init(
            &server_connection_args->listener,
            bootstrap_options->bootstrap->allocator,
            bootstrap_options->socket_options)) {
        goto cleanup_server_connection_args;
    }

    struct aws_socket_endpoint endpoint;
    AWS_ZERO_STRUCT(endpoint);
    size_t host_name_len = 0;
    if (aws_secure_strlen(bootstrap_options->host_name, sizeof(endpoint.address), &host_name_len)) {
        goto cleanup_server_connection_args;
    }

    memcpy(endpoint.address, bootstrap_options->host_name, host_name_len);
    endpoint.port = bootstrap_options->port;

    if (aws_socket_bind(&server_connection_args->listener, &endpoint)) {
        goto cleanup_listener;
    }

    if (aws_socket_listen(&server_connection_args->listener, 1024)) {
        goto cleanup_listener;
    }

    if (aws_socket_start_accept(
            &server_connection_args->listener,
            connection_loop,
            s_on_server_connection_result,
            server_connection_args)) {
        goto cleanup_listener;
    }

    return &server_connection_args->listener;

cleanup_listener:
    aws_socket_clean_up(&server_connection_args->listener);

cleanup_server_connection_args:
    s_server_connection_args_release(server_connection_args);

    return NULL;
}

void aws_server_bootstrap_destroy_socket_listener(struct aws_server_bootstrap *bootstrap, struct aws_socket *listener) {
    struct server_connection_args *server_connection_args =
        AWS_CONTAINER_OF(listener, struct server_connection_args, listener);

    AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: releasing bootstrap reference", (void *)bootstrap);
    aws_event_loop_schedule_task_now(listener->event_loop, &server_connection_args->listener_destroy_task);
}

int aws_server_bootstrap_set_alpn_callback(
    struct aws_server_bootstrap *bootstrap,
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated) {
    AWS_ASSERT(on_protocol_negotiated);
    AWS_LOGF_DEBUG(AWS_LS_IO_CHANNEL_BOOTSTRAP, "id=%p: Setting ALPN callback", (void *)bootstrap);
    bootstrap->on_protocol_negotiated = on_protocol_negotiated;
    return AWS_OP_SUCCESS;
}
