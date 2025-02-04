/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/socket.h>

#include <aws/io/logging.h>

/* common validation for connect() and bind() */
static int s_socket_validate_port_for_domain(uint32_t port, enum aws_socket_domain domain) {
    switch (domain) {
        case AWS_SOCKET_IPV4:
        case AWS_SOCKET_IPV6:
            if (port > UINT16_MAX) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_SOCKET,
                    "Invalid port=%u for %s. Cannot exceed 65535",
                    port,
                    domain == AWS_SOCKET_IPV4 ? "IPv4" : "IPv6");
                return aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
            }
            break;

        case AWS_SOCKET_LOCAL:
            /* port is ignored */
            break;

        case AWS_SOCKET_VSOCK:
            /* any 32bit port is legal */
            break;

        default:
            AWS_LOGF_ERROR(AWS_LS_IO_SOCKET, "Cannot validate port for unknown domain=%d", domain);
            return aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
    }
    return AWS_OP_SUCCESS;
}

int aws_socket_validate_port_for_connect(uint32_t port, enum aws_socket_domain domain) {
    if (s_socket_validate_port_for_domain(port, domain)) {
        return AWS_OP_ERR;
    }

    /* additional validation */
    switch (domain) {
        case AWS_SOCKET_IPV4:
        case AWS_SOCKET_IPV6:
            if (port == 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_SOCKET,
                    "Invalid port=%u for %s connections. Must use 1-65535",
                    port,
                    domain == AWS_SOCKET_IPV4 ? "IPv4" : "IPv6");
                return aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
            }
            break;

        case AWS_SOCKET_VSOCK:
            if (port == (uint32_t)-1) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_SOCKET, "Invalid port for VSOCK connections. Cannot use VMADDR_PORT_ANY (-1U).");
                return aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
            }
            break;

        default:
            /* no extra validation */
            break;
    }
    return AWS_OP_SUCCESS;
}

int aws_socket_validate_port_for_bind(uint32_t port, enum aws_socket_domain domain) {
    return s_socket_validate_port_for_domain(port, domain);
}
