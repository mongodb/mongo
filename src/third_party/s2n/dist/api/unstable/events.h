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

#pragma once

#include <s2n.h>
#include <stdint.h>

/* This is a special value assigned to handshake_start_epoch_ns to indicate that
 * it has already been sent to the application and should not be sent again.
 */
#define HANDSHAKE_EVENT_SENT UINT64_C(1) << 63

struct s2n_event_handshake {
    /**
     * The negotiated protocol version
     * 
     * This will be one of the protocol version constants defined in s2n.h 
     */
    int protocol_version;
    /* static memory */
    const char *cipher;
    /* static memory */
    const char *group;
    /* the amount of time inside the synchronous s2n_negotiate method */
    uint64_t handshake_time_ns;
    /**
     * The start of the handshake. This is not an interpretable time, and only has
     * meaning in reference to handshake_end_ns. 
     *
     * This is also used as a flag to ensure that the same event isn't emitted 
     * twice. After the event has been emitted this is set to HANDSHAKE_EVENT_SENT
     */
    uint64_t handshake_start_ns;
    uint64_t handshake_end_ns;
};

typedef void (*s2n_event_on_handshake_cb)(struct s2n_connection *conn, void *subscriber, struct s2n_event_handshake *event);

S2N_API extern int s2n_config_set_subscriber(struct s2n_config *config, void *subscriber);
/**
 * Set a callback to receive a handshake event.
 * 
 * The `struct s2n_event_handshake *event` is only valid over the lifetime of the 
 * callbacks, and must not be referenced after the callback returned.
 * 
 * An event is not emitted if the handshake fails.
 */
S2N_API extern int s2n_config_set_handshake_event(struct s2n_config *config, s2n_event_on_handshake_cb callback);
