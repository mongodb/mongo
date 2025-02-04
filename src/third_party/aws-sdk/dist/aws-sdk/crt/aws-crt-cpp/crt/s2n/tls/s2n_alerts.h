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

#include <stdint.h>

#include "tls/s2n_connection.h"

#define S2N_TLS_ALERT_LEVEL_WARNING 1
#define S2N_TLS_ALERT_LEVEL_FATAL   2

typedef enum {
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *# enum {
     *#     close_notify(0),
     *#     unexpected_message(10),
     *#     bad_record_mac(20),
     *#     record_overflow(22),
     *#     handshake_failure(40),
     */
    S2N_TLS_ALERT_CLOSE_NOTIFY = 0,
    S2N_TLS_ALERT_UNEXPECTED_MESSAGE = 10,
    S2N_TLS_ALERT_BAD_RECORD_MAC = 20,
    S2N_TLS_ALERT_RECORD_OVERFLOW = 22,
    S2N_TLS_ALERT_HANDSHAKE_FAILURE = 40,
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *#     bad_certificate(42),
     *#     unsupported_certificate(43),
     *#     certificate_revoked(44),
     *#     certificate_expired(45),
     *#     certificate_unknown(46),
     */
    S2N_TLS_ALERT_BAD_CERTIFICATE = 42,
    S2N_TLS_ALERT_UNSUPPORTED_CERTIFICATE = 43,
    S2N_TLS_ALERT_CERTIFICATE_REVOKED = 44,
    S2N_TLS_ALERT_CERTIFICATE_EXPIRED = 45,
    S2N_TLS_ALERT_CERTIFICATE_UNKNOWN = 46,
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *#     illegal_parameter(47),
     *#     unknown_ca(48),
     *#     access_denied(49),
     *#     decode_error(50),
     *#     decrypt_error(51),
     */
    S2N_TLS_ALERT_ILLEGAL_PARAMETER = 47,
    S2N_TLS_ALERT_UNKNOWN_CA = 48,
    S2N_TLS_ALERT_ACCESS_DENIED = 49,
    S2N_TLS_ALERT_DECODE_ERROR = 50,
    S2N_TLS_ALERT_DECRYPT_ERROR = 51,
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *#     protocol_version(70),
     *#     insufficient_security(71),
     *#     internal_error(80),
     *#     inappropriate_fallback(86),
     *#     user_canceled(90),
     */
    S2N_TLS_ALERT_PROTOCOL_VERSION = 70,
    S2N_TLS_ALERT_INSUFFICIENT_SECURITY = 71,
    S2N_TLS_ALERT_INTERNAL_ERROR = 80,
    S2N_TLS_ALERT_INAPPROPRIATE_FALLBACK = 86,
    S2N_TLS_ALERT_USER_CANCELED = 90,
    /*
     *= https://www.rfc-editor.org/rfc/rfc5246#section-7.2
     *#     no_renegotiation(100),
     */
    S2N_TLS_ALERT_NO_RENEGOTIATION = 100,
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *#     missing_extension(109),
     *#     unsupported_extension(110),
     *#     unrecognized_name(112),
     *#     bad_certificate_status_response(113),
     *#     unknown_psk_identity(115),
     */
    S2N_TLS_ALERT_MISSING_EXTENSION = 109,
    S2N_TLS_ALERT_UNSUPPORTED_EXTENSION = 110,
    S2N_TLS_ALERT_UNRECOGNIZED_NAME = 112,
    S2N_TLS_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE = 113,
    S2N_TLS_ALERT_UNKNOWN_PSK_IDENTITY = 115,
    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6
     *#     certificate_required(116),
     *#     no_application_protocol(120),
     *#     (255)
     *# } AlertDescription;
     */
    S2N_TLS_ALERT_CERTIFICATE_REQUIRED = 116,
    S2N_TLS_ALERT_NO_APPLICATION_PROTOCOL = 120,
} s2n_tls_alert_code;

int s2n_process_alert_fragment(struct s2n_connection *conn);
int s2n_queue_reader_unsupported_protocol_version_alert(struct s2n_connection *conn);
int s2n_queue_reader_handshake_failure_alert(struct s2n_connection *conn);
S2N_RESULT s2n_queue_reader_no_renegotiation_alert(struct s2n_connection *conn);
S2N_RESULT s2n_alerts_write_error_or_close_notify(struct s2n_connection *conn);
S2N_RESULT s2n_alerts_write_warning(struct s2n_connection *conn);
