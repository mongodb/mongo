#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/mqtt/v5/mqtt5_client.h>
#include <aws/mqtt/v5/mqtt5_types.h>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            /**
             * MQTT message delivery quality of service.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901234) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_QOS_AT_MOST_ONCE</TD><TD>https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901235</TD>
             * <TR><TD>AWS_MQTT5_QOS_AT_LEAST_ONCE</TD><TD>https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901236</TD>
             * <TR><TD>AWS_MQTT5_QOS_EXACTLY_ONCE</TD><TD>https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901237</TD>
             * </TABLE>
             *
             */
            using QOS = aws_mqtt5_qos;

            /**
             * Server return code for connect attempts.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901079) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_CRC_SUCCESS</TD><TD>0</TD>
             * <TR><TD>AWS_MQTT5_CRC_UNSPECIFIED_ERROR</TD><TD>128</TD>
             * <TR><TD>AWS_MQTT5_CRC_MALFORMED_PACKET</TD><TD>129</TD>
             * <TR><TD>AWS_MQTT5_CRC_PROTOCOL_ERROR</TD><TD>130</TD>
             * <TR><TD>AWS_MQTT5_CRC_IMPLEMENTATION_SPECIFIC_ERROR</TD><TD>131</TD>
             * <TR><TD>AWS_MQTT5_CRC_UNSUPPORTED_PROTOCOL_VERSION</TD><TD>132</TD>
             * <TR><TD>AWS_MQTT5_CRC_CLIENT_IDENTIFIER_NOT_VALID</TD><TD>133</TD>
             * <TR><TD>AWS_MQTT5_CRC_BAD_USERNAME_OR_PASSWORD</TD><TD>134</TD>
             * <TR><TD>AWS_MQTT5_CRC_NOT_AUTHORIZED</TD><TD>135</TD>
             * <TR><TD>AWS_MQTT5_CRC_SERVER_UNAVAILABLE</TD><TD>136</TD>
             * <TR><TD>AWS_MQTT5_CRC_SERVER_BUSY</TD><TD>137</TD>
             * <TR><TD>AWS_MQTT5_CRC_BANNED</TD><TD>138</TD>
             * <TR><TD>AWS_MQTT5_CRC_BAD_AUTHENTICATION_METHOD</TD><TD>140</TD>
             * <TR><TD>AWS_MQTT5_CRC_TOPIC_NAME_INVALID</TD><TD>144</TD>
             * <TR><TD>AWS_MQTT5_CRC_PACKET_TOO_LARGE</TD><TD>149</TD>
             * <TR><TD>AWS_MQTT5_CRC_QUOTA_EXCEEDED</TD><TD>151</TD>
             * <TR><TD>AWS_MQTT5_CRC_PAYLOAD_FORMAT_INVALID</TD><TD>153</TD>
             * <TR><TD>AWS_MQTT5_CRC_RETAIN_NOT_SUPPORTED</TD><TD>154</TD>
             * <TR><TD>AWS_MQTT5_CRC_QOS_NOT_SUPPORTED</TD><TD>155</TD>
             * <TR><TD>AWS_MQTT5_CRC_USE_ANOTHER_SERVER</TD><TD>156</TD>
             * <TR><TD>AWS_MQTT5_CRC_SERVER_MOVED</TD><TD>157</TD>
             * <TR><TD>AWS_MQTT5_CRC_CONNECTION_RATE_EXCEEDED</TD><TD>159</TD>
             * </TABLE>
             *
             *
             */
            using ConnectReasonCode = aws_mqtt5_connect_reason_code;

            /**
             * Reason code inside DISCONNECT packets.  Helps determine why a connection was terminated.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901208) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_DRC_NORMAL_DISCONNECTION</TD><TD>0</TD>
             * <TR><TD>AWS_MQTT5_DRC_DISCONNECT_WITH_WILL_MESSAGE</TD><TD>4</TD>
             * <TR><TD>AWS_MQTT5_DRC_UNSPECIFIED_ERROR</TD><TD>128</TD>
             * <TR><TD>AWS_MQTT5_DRC_MALFORMED_PACKET</TD><TD>129</TD>
             * <TR><TD>AWS_MQTT5_DRC_PROTOCOL_ERROR</TD><TD>130</TD>
             * <TR><TD>AWS_MQTT5_DRC_IMPLEMENTATION_SPECIFIC_ERROR</TD><TD>131</TD>
             * <TR><TD>AWS_MQTT5_DRC_NOT_AUTHORIZED</TD><TD>135</TD>
             * <TR><TD>AWS_MQTT5_DRC_SERVER_BUSY</TD><TD>137</TD>
             * <TR><TD>AWS_MQTT5_DRC_SERVER_SHUTTING_DOWN</TD><TD>139</TD>
             * <TR><TD>AWS_MQTT5_DRC_KEEP_ALIVE_TIMEOUT</TD><TD>141</TD>
             * <TR><TD>AWS_MQTT5_DRC_SESSION_TAKEN_OVER</TD><TD>142</TD>
             * <TR><TD>AWS_MQTT5_DRC_TOPIC_FILTER_INVALID</TD><TD>143</TD>
             * <TR><TD>AWS_MQTT5_DRC_TOPIC_NAME_INVALID</TD><TD>144</TD>
             * <TR><TD>AWS_MQTT5_DRC_RECEIVE_MAXIMUM_EXCEEDED</TD><TD>147</TD>
             * <TR><TD>AWS_MQTT5_DRC_TOPIC_ALIAS_INVALID</TD><TD>148</TD>
             * <TR><TD>AWS_MQTT5_DRC_PACKET_TOO_LARGE</TD><TD>149</TD>
             * <TR><TD>AWS_MQTT5_DRC_MESSAGE_RATE_TOO_HIGH</TD><TD>150</TD>
             * <TR><TD>AWS_MQTT5_DRC_QUOTA_EXCEEDED</TD><TD>151</TD>
             * <TR><TD>AWS_MQTT5_DRC_ADMINISTRATIVE_ACTION</TD><TD>152</TD>
             * <TR><TD>AWS_MQTT5_DRC_PAYLOAD_FORMAT_INVALID</TD><TD>153</TD>
             * <TR><TD>AWS_MQTT5_DRC_RETAIN_NOT_SUPPORTED</TD><TD>154</TD>
             * <TR><TD>AWS_MQTT5_DRC_QOS_NOT_SUPPORTED</TD><TD>155</TD>
             * <TR><TD>AWS_MQTT5_DRC_USE_ANOTHER_SERVER</TD><TD>156</TD>
             * <TR><TD>AWS_MQTT5_DRC_SERVER_MOVED</TD><TD>157</TD>
             * <TR><TD>AWS_MQTT5_DRC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED</TD><TD>158</TD>
             * <TR><TD>AWS_MQTT5_DRC_CONNECTION_RATE_EXCEEDED</TD><TD>159</TD>
             * <TR><TD>AWS_MQTT5_DRC_MAXIMUM_CONNECT_TIME</TD><TD>160</TD>
             * <TR><TD>AWS_MQTT5_DRC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED</TD><TD>161</TD>
             * <TR><TD>AWS_MQTT5_DRC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED</TD><TD>162</TD>
             * </TABLE>
             *
             */
            using DisconnectReasonCode = aws_mqtt5_disconnect_reason_code;

            /**
             * Reason code inside PUBACK packets
             *
             * Data model of an [MQTT5
             * PUBACK](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901121) packet
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_PARC_SUCCESS</TD><TD>0</TD>
             * <TR><TD>AWS_MQTT5_PARC_NO_MATCHING_SUBSCRIBERS</TD><TD>16</TD>
             * <TR><TD>AWS_MQTT5_PARC_UNSPECIFIED_ERROR</TD><TD>128</TD>
             * <TR><TD>AWS_MQTT5_PARC_IMPLEMENTATION_SPECIFIC_ERROR</TD><TD>131</TD>
             * <TR><TD>AWS_MQTT5_PARC_NOT_AUTHORIZED</TD><TD>135</TD>
             * <TR><TD>AWS_MQTT5_PARC_TOPIC_NAME_INVALID</TD><TD>144</TD>
             * <TR><TD>AWS_MQTT5_PARC_PACKET_IDENTIFIER_IN_USE</TD><TD>145</TD>
             * <TR><TD>AWS_MQTT5_PARC_QUOTA_EXCEEDED</TD><TD>151</TD>
             * <TR><TD>AWS_MQTT5_PARC_PAYLOAD_FORMAT_INVALID</TD><TD>153</TD>
             * </TABLE>
             */
            using PubAckReasonCode = aws_mqtt5_puback_reason_code;

            /**
             * Reason code inside PUBACK packets that indicates the result of the associated PUBLISH request.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_PARC_SUCCESS</TD><TD>0</TD>
             * <TR><TD>AWS_MQTT5_PARC_NO_MATCHING_SUBSCRIBERS</TD><TD>16</TD>
             * <TR><TD>AWS_MQTT5_PARC_UNSPECIFIED_ERROR</TD><TD>128</TD>
             * <TR><TD>AWS_MQTT5_PARC_IMPLEMENTATION_SPECIFIC_ERROR</TD><TD>131</TD>
             * <TR><TD>AWS_MQTT5_PARC_NOT_AUTHORIZED</TD><TD>135</TD>
             * <TR><TD>AWS_MQTT5_PARC_TOPIC_NAME_INVALID</TD><TD>144</TD>
             * <TR><TD>AWS_MQTT5_PARC_PACKET_IDENTIFIER_IN_USE</TD><TD>145</TD>
             * <TR><TD>AWS_MQTT5_PARC_QUOTA_EXCEEDED</TD><TD>151</TD>
             * <TR><TD>AWS_MQTT5_PARC_PAYLOAD_FORMAT_INVALID</TD><TD>153</TD>
             * </TABLE>
             */
            using SubAckReasonCode = aws_mqtt5_suback_reason_code;

            /**
             * Reason codes inside UNSUBACK packet payloads that specify the results for each topic filter in the
             * associated UNSUBSCRIBE packet.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901194) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_UARC_SUCCESS</TD><TD>0</TD>
             * <TR><TD>AWS_MQTT5_UARC_NO_SUBSCRIPTION_EXISTED</TD><TD>17</TD>
             * <TR><TD>AWS_MQTT5_UARC_UNSPECIFIED_ERROR</TD><TD>128</TD>
             * <TR><TD>AWS_MQTT5_UARC_IMPLEMENTATION_SPECIFIC_ERROR</TD><TD>131</TD>
             * <TR><TD>AWS_MQTT5_UARC_NOT_AUTHORIZED</TD><TD>135</TD>
             * <TR><TD>AWS_MQTT5_UARC_TOPIC_FILTER_INVALID</TD><TD>143</TD>
             * <TR><TD>AWS_MQTT5_UARC_PACKET_IDENTIFIER_IN_USE</TD><TD>145</TD>
             * </TABLE>
             *
             */
            using UnSubAckReasonCode = aws_mqtt5_unsuback_reason_code;

            /**
             * Controls how the MQTT5 client should behave with respect to MQTT sessions.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_CSBT_DEFAULT</TD><TD>Maps to AWS_MQTT5_CSBT_CLEAN</TD></TR>
             * <TR><TD>AWS_MQTT5_CSBT_CLEAN</TD><TD>Always join a new, clean session</TD></TR>
             * <TR><TD>AWS_MQTT5_CSBT_REJOIN_POST_SUCCESS</TD><TD>Always attempt to rejoin an existing session after an
             * initial connection success.</TD></TR> <TR><TD>AWS_MQTT5_CSBT_REJOIN_ALWAYS</TD><TD>Always attempt to
             * rejoin an existing session. Since the client does not support durable session persistence, this option is
             * not guaranteed to be spec compliant because any unacknowledged qos1 publishes (which are part of the
             * client session state) will not be present on the initial connection. Until we support durable session
             * resumption, this option is technically spec-breaking, but useful.</TD></TR>
             * </TABLE>
             */
            using ClientSessionBehaviorType = aws_mqtt5_client_session_behavior_type;

            /**
             * Additional controls for client behavior with respect to operation validation and flow control; these
             * checks go beyond the MQTT5 spec to respect limits of specific MQTT brokers.
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_EVAFCO_NONE</TD><TD>Do not do any additional validation or flow control outside of the
             * MQTT5 spec</TD></TR> <TR><TD>AWS_MQTT5_EVAFCO_AWS_IOT_CORE_DEFAULTS</TD><TD>Apply additional client-side
             * operational flow control that respects the default AWS IoT Core limits. Applies the following flow
             * control: (1) Outbound throughput throttled to 512KB/s (2) Outbound publish TPS throttled to 100</TD></TR>
             * </TABLE>
             *
             */
            using ClientExtendedValidationAndFlowControl = aws_mqtt5_extended_validation_and_flow_control_options;

            /**
             * Controls how disconnects affect the queued and in-progress operations tracked by the client.  Also
             * controls how operations are handled while the client is not connected.  In particular, if the client is
             * not connected, then any operation that would be failed on disconnect (according to these rules) will be
             * rejected.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_COQBT_DEFAULT</TD><TD>Maps to AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT</TD></TR>
             * <TR><TD>AWS_MQTT5_COQBT_FAIL_NON_QOS1_PUBLISH_ON_DISCONNECT</TD><TD>Requeues QoS 1+ publishes on
             * disconnect; unacked publishes go to the front, unprocessed publishes stay in place. All other operations
             * (QoS 0 publishes, subscribe, unsubscribe) are failed.</TD></TR>
             * <TR><TD>AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT</TD><TD>Qos 0 publishes that are not complete at
             * the time of disconnection are failed. Unacked QoS 1+ publishes are requeued at the head of the line for
             * immediate retransmission on a session resumption. All other operations are requeued in the original order
             * behind any retransmissions.</TD></TR> <TR><TD>AWS_MQTT5_COQBT_FAIL_ALL_ON_DISCONNECT</TD><TD>All
             * operations that are not complete at the time of disconnection are failed, except those operations that
             * the MQTT 5 spec requires to be retransmitted (unacked QoS 1+ publishes).</TD></TR>
             * </TABLE>
             *
             */
            using ClientOperationQueueBehaviorType = aws_mqtt5_client_operation_queue_behavior_type;

            /**
             * Controls how the reconnect delay is modified in order to smooth out the distribution of reconnection
             * attempt timepoints for a large set of reconnecting clients.
             *
             * See [Exponential Backoff and
             * Jitter](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT</TD><TD>Uses AWS_EXPONENTIAL_BACKOFF_JITTER_FULL</TD></TR>
             * <TR><TD>AWS_EXPONENTIAL_BACKOFF_JITTER_NONE</TD><TD>No jitter is applied to the exponential
             * backoff</TD></TR> <TR><TD>AWS_EXPONENTIAL_BACKOFF_JITTER_FULL</TD><TD>Full jitter is applied to the
             * exponential backoff</TD></TR> <TR><TD>AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED</TD><TD>Jitter is
             * decorrelated from the backoff sequence</TD></TR>
             * </TABLE>
             *
             */
            using ExponentialBackoffJitterMode = aws_exponential_backoff_jitter_mode;

            /** @deprecated JitterMode is deprecated, please use  Aws::Crt::Mqtt5::ExponentialBackoffJitterMode */
            using JitterMode = ExponentialBackoffJitterMode;

            /**
             * Optional property describing a PUBLISH payload's format.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901111) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_PFI_BYTES</TD><TD>Bytes format.</TD></TR>
             * <TR><TD>AWS_MQTT5_PFI_UTF8</TD><TD>UTF-8 format.</TD></TR>
             * </TABLE>
             */
            using PayloadFormatIndicator = aws_mqtt5_payload_format_indicator;

            /**
             * Configures how retained messages should be handled when subscribing with a topic filter that matches
             * topics with associated retained messages.
             *
             * Enum values match [MQTT5
             * spec](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169) encoding values.
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE</TD><TD>Server should send all retained messages on topics that
             * match the subscription's filter.</TD></TR> <TR><TD>AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE_IF_NEW</TD><TD>Server
             * should send all retained messages on topics that match the subscription's filter, where this is the first
             * (relative to connection) subscription filter that matches the topic with a retained message.</TD></TR>
             * <TR><TD>AWS_MQTT5_RHT_DONT_SEND</TD><TD>Subscribe must not trigger any retained message publishes from
             * the server.</TD></TR>
             * </TABLE>
             */
            using RetainHandlingType = aws_mqtt5_retain_handling_type;

            /**
             * Type of mqtt packet.
             * Enum values match mqtt spec encoding values.
             *
             * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901022
             *
             * <TABLE>
             * <TR><TH colspan="2">Enumerator</TH>
             * <TR><TD>AWS_MQTT5_PT_NONE</TD><TD>Internal indicator that the associated packet is null.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_RESERVED</TD><TD>Reserved.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_CONNECT</TD><TD>CONNECT packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_CONNACK</TD><TD>CONNACK packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PUBLISH</TD><TD>PUBLISH packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PUBACK</TD><TD>PUBACK packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PUBREC</TD><TD>PUBREC packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PUBREL</TD><TD>PUBREL packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PUBCOMP</TD><TD>PUBCOMP packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_SUBSCRIBE</TD><TD>SUBSCRIBE packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_SUBACK</TD><TD>SUBACK packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_UNSUBSCRIBE</TD><TD>UNSUBSCRIBE packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_UNSUBACK</TD><TD>UNSUBACK packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PINGREQ</TD><TD>PINGREQ packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_PINGRESP</TD><TD>PINGRESP packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_DISCONNECT</TD><TD>DISCONNECT packet.</TD></TR>
             * <TR><TD>AWS_MQTT5_PT_AUTH</TD><TD>AUTH packet.</TD></TR>
             * </TABLE>
             *
             */
            using PacketType = aws_mqtt5_packet_type;

        } // namespace Mqtt5

    } // namespace Crt
} // namespace Aws
