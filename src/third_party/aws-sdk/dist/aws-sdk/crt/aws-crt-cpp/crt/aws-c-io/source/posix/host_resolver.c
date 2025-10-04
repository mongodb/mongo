/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/host_resolver.h>

#include <aws/io/logging.h>

#include <aws/common/string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

int aws_default_dns_resolve(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    struct aws_array_list *output_addresses,
    void *user_data) {

    (void)user_data;
    struct addrinfo *result = NULL;
    struct addrinfo *iter = NULL;
    /* max string length for ipv6. */
    socklen_t max_len = INET6_ADDRSTRLEN;
    char address_buffer[max_len];

    const char *hostname_cstr = aws_string_c_str(host_name);
    AWS_LOGF_DEBUG(AWS_LS_IO_DNS, "static: resolving host %s", hostname_cstr);

    /* Android would prefer NO HINTS IF YOU DON'T MIND, SIR */
#if defined(ANDROID)
    int err_code = getaddrinfo(hostname_cstr, NULL, NULL, &result);
#else
    struct addrinfo hints;
    AWS_ZERO_STRUCT(hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
#    if !defined(__OpenBSD__)
    hints.ai_flags = AI_ALL | AI_V4MAPPED;
#    endif /* __OpenBSD__ */

    int err_code = getaddrinfo(hostname_cstr, NULL, &hints, &result);
#endif

    if (err_code) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_DNS, "static: getaddrinfo failed with error_code %d: %s", err_code, gai_strerror(err_code));
        goto clean_up;
    }

    for (iter = result; iter != NULL; iter = iter->ai_next) {
        struct aws_host_address host_address;

        AWS_ZERO_ARRAY(address_buffer);

        if (iter->ai_family == AF_INET6) {
            host_address.record_type = AWS_ADDRESS_RECORD_TYPE_AAAA;
            inet_ntop(iter->ai_family, &((struct sockaddr_in6 *)iter->ai_addr)->sin6_addr, address_buffer, max_len);
        } else {
            host_address.record_type = AWS_ADDRESS_RECORD_TYPE_A;
            inet_ntop(iter->ai_family, &((struct sockaddr_in *)iter->ai_addr)->sin_addr, address_buffer, max_len);
        }

        size_t address_len = strlen(address_buffer);
        const struct aws_string *address =
            aws_string_new_from_array(allocator, (const uint8_t *)address_buffer, address_len);

        if (!address) {
            goto clean_up;
        }

        const struct aws_string *host_cpy = aws_string_new_from_string(allocator, host_name);

        if (!host_cpy) {
            aws_string_destroy((void *)address);
            goto clean_up;
        }

        AWS_LOGF_DEBUG(AWS_LS_IO_DNS, "static: resolved record: %s", address_buffer);

        host_address.address = address;
        host_address.weight = 0;
        host_address.allocator = allocator;
        host_address.use_count = 0;
        host_address.connection_failure_count = 0;
        host_address.host = host_cpy;

        if (aws_array_list_push_back(output_addresses, &host_address)) {
            aws_host_address_clean_up(&host_address);
            goto clean_up;
        }
    }

    freeaddrinfo(result);
    return AWS_OP_SUCCESS;

clean_up:
    if (result) {
        freeaddrinfo(result);
    }

    if (err_code) {
        switch (err_code) {
            case EAI_FAIL:
            case EAI_AGAIN:
                return aws_raise_error(AWS_IO_DNS_QUERY_FAILED);
            case EAI_MEMORY:
                return aws_raise_error(AWS_ERROR_OOM);
            case EAI_NONAME:
            case EAI_SERVICE:
                return aws_raise_error(AWS_IO_DNS_INVALID_NAME);
            default:
                return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        }
    }

    return AWS_OP_ERR;
}
