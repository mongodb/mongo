/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace messaging
{

/**
  The number of messages sent, received, or processed in the scope of the batching operation.
  <p>
  Instrumentations SHOULD NOT set @code messaging.batch.message_count @endcode on spans that operate
  with a single message. When a messaging client library supports both batch and single-message API
  for the same operation, instrumentations SHOULD use @code messaging.batch.message_count @endcode
  for batching APIs and SHOULD NOT use it for single-message APIs.
 */
static constexpr const char *kMessagingBatchMessageCount = "messaging.batch.message_count";

/**
  A unique identifier for the client that consumes or produces a message.
 */
static constexpr const char *kMessagingClientId = "messaging.client.id";

/**
  The name of the consumer group with which a consumer is associated.
  <p>
  Semantic conventions for individual messaging systems SHOULD document whether @code
  messaging.consumer.group.name @endcode is applicable and what it means in the context of that
  system.
 */
static constexpr const char *kMessagingConsumerGroupName = "messaging.consumer.group.name";

/**
  A boolean that is true if the message destination is anonymous (could be unnamed or have
  auto-generated name).
 */
static constexpr const char *kMessagingDestinationAnonymous = "messaging.destination.anonymous";

/**
  The message destination name
  <p>
  Destination name SHOULD uniquely identify a specific queue, topic or other entity within the
  broker. If the broker doesn't have such notion, the destination name SHOULD uniquely identify the
  broker.
 */
static constexpr const char *kMessagingDestinationName = "messaging.destination.name";

/**
  The identifier of the partition messages are sent to or received from, unique within the @code
  messaging.destination.name @endcode.
 */
static constexpr const char *kMessagingDestinationPartitionId =
    "messaging.destination.partition.id";

/**
  The name of the destination subscription from which a message is consumed.
  <p>
  Semantic conventions for individual messaging systems SHOULD document whether @code
  messaging.destination.subscription.name @endcode is applicable and what it means in the context of
  that system.
 */
static constexpr const char *kMessagingDestinationSubscriptionName =
    "messaging.destination.subscription.name";

/**
  Low cardinality representation of the messaging destination name
  <p>
  Destination names could be constructed from templates. An example would be a destination name
  involving a user name or product id. Although the destination name in this case is of high
  cardinality, the underlying template is of low cardinality and can be effectively used for
  grouping and aggregation.
 */
static constexpr const char *kMessagingDestinationTemplate = "messaging.destination.template";

/**
  A boolean that is true if the message destination is temporary and might not exist anymore after
  messages are processed.
 */
static constexpr const char *kMessagingDestinationTemporary = "messaging.destination.temporary";

/**
  Deprecated, no replacement at this time.

  @deprecated
  {"note": "Removed. No replacement at this time.", "reason": "obsoleted"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingDestinationPublishAnonymous =
    "messaging.destination_publish.anonymous";

/**
  Deprecated, no replacement at this time.

  @deprecated
  {"note": "Removed. No replacement at this time.", "reason": "obsoleted"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingDestinationPublishName =
    "messaging.destination_publish.name";

/**
  Deprecated, use @code messaging.consumer.group.name @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.consumer.group.name @endcode.", "reason": "renamed",
  "renamed_to": "messaging.consumer.group.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingEventhubsConsumerGroup =
    "messaging.eventhubs.consumer.group";

/**
  The UTC epoch seconds at which the message has been accepted and stored in the entity.
 */
static constexpr const char *kMessagingEventhubsMessageEnqueuedTime =
    "messaging.eventhubs.message.enqueued_time";

/**
  The ack deadline in seconds set for the modify ack deadline request.
 */
static constexpr const char *kMessagingGcpPubsubMessageAckDeadline =
    "messaging.gcp_pubsub.message.ack_deadline";

/**
  The ack id for a given message.
 */
static constexpr const char *kMessagingGcpPubsubMessageAckId =
    "messaging.gcp_pubsub.message.ack_id";

/**
  The delivery attempt for a given message.
 */
static constexpr const char *kMessagingGcpPubsubMessageDeliveryAttempt =
    "messaging.gcp_pubsub.message.delivery_attempt";

/**
  The ordering key for a given message. If the attribute is not present, the message does not have
  an ordering key.
 */
static constexpr const char *kMessagingGcpPubsubMessageOrderingKey =
    "messaging.gcp_pubsub.message.ordering_key";

/**
  Deprecated, use @code messaging.consumer.group.name @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.consumer.group.name @endcode.", "reason": "renamed",
  "renamed_to": "messaging.consumer.group.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingKafkaConsumerGroup =
    "messaging.kafka.consumer.group";

/**
  Deprecated, use @code messaging.destination.partition.id @endcode instead.

  @deprecated
  {"note": "Record string representation of the partition id in @code
  messaging.destination.partition.id @endcode attribute.", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingKafkaDestinationPartition =
    "messaging.kafka.destination.partition";

/**
  Message keys in Kafka are used for grouping alike messages to ensure they're processed on the same
  partition. They differ from @code messaging.message.id @endcode in that they're not unique. If the
  key is @code null @endcode, the attribute MUST NOT be set. <p> If the key type is not string, it's
  string representation has to be supplied for the attribute. If the key has no unambiguous,
  canonical string form, don't include its value.
 */
static constexpr const char *kMessagingKafkaMessageKey = "messaging.kafka.message.key";

/**
  Deprecated, use @code messaging.kafka.offset @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.kafka.offset @endcode.", "reason": "renamed", "renamed_to":
  "messaging.kafka.offset"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingKafkaMessageOffset =
    "messaging.kafka.message.offset";

/**
  A boolean that is true if the message is a tombstone.
 */
static constexpr const char *kMessagingKafkaMessageTombstone = "messaging.kafka.message.tombstone";

/**
  The offset of a record in the corresponding Kafka partition.
 */
static constexpr const char *kMessagingKafkaOffset = "messaging.kafka.offset";

/**
  The size of the message body in bytes.
  <p>
  This can refer to both the compressed or uncompressed body size. If both sizes are known, the
  uncompressed body size should be used.
 */
static constexpr const char *kMessagingMessageBodySize = "messaging.message.body.size";

/**
  The conversation ID identifying the conversation to which the message belongs, represented as a
  string. Sometimes called "Correlation ID".
 */
static constexpr const char *kMessagingMessageConversationId = "messaging.message.conversation_id";

/**
  The size of the message body and metadata in bytes.
  <p>
  This can refer to both the compressed or uncompressed size. If both sizes are known, the
  uncompressed size should be used.
 */
static constexpr const char *kMessagingMessageEnvelopeSize = "messaging.message.envelope.size";

/**
  A value used by the messaging system as an identifier for the message, represented as a string.
 */
static constexpr const char *kMessagingMessageId = "messaging.message.id";

/**
  Deprecated, use @code messaging.operation.type @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.operation.type @endcode.", "reason": "renamed",
  "renamed_to": "messaging.operation.type"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingOperation = "messaging.operation";

/**
  The system-specific name of the messaging operation.
 */
static constexpr const char *kMessagingOperationName = "messaging.operation.name";

/**
  A string identifying the type of the messaging operation.
  <p>
  If a custom value is used, it MUST be of low cardinality.
 */
static constexpr const char *kMessagingOperationType = "messaging.operation.type";

/**
  RabbitMQ message routing key.
 */
static constexpr const char *kMessagingRabbitmqDestinationRoutingKey =
    "messaging.rabbitmq.destination.routing_key";

/**
  RabbitMQ message delivery tag
 */
static constexpr const char *kMessagingRabbitmqMessageDeliveryTag =
    "messaging.rabbitmq.message.delivery_tag";

/**
  Deprecated, use @code messaging.consumer.group.name @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.consumer.group.name @endcode on the consumer spans. No
  replacement for producer spans.\n", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessagingRocketmqClientGroup =
    "messaging.rocketmq.client_group";

/**
  Model of message consumption. This only applies to consumer spans.
 */
static constexpr const char *kMessagingRocketmqConsumptionModel =
    "messaging.rocketmq.consumption_model";

/**
  The delay time level for delay message, which determines the message delay time.
 */
static constexpr const char *kMessagingRocketmqMessageDelayTimeLevel =
    "messaging.rocketmq.message.delay_time_level";

/**
  The timestamp in milliseconds that the delay message is expected to be delivered to consumer.
 */
static constexpr const char *kMessagingRocketmqMessageDeliveryTimestamp =
    "messaging.rocketmq.message.delivery_timestamp";

/**
  It is essential for FIFO message. Messages that belong to the same message group are always
  processed one by one within the same consumer group.
 */
static constexpr const char *kMessagingRocketmqMessageGroup = "messaging.rocketmq.message.group";

/**
  Key(s) of message, another way to mark message besides message id.
 */
static constexpr const char *kMessagingRocketmqMessageKeys = "messaging.rocketmq.message.keys";

/**
  The secondary classifier of message besides topic.
 */
static constexpr const char *kMessagingRocketmqMessageTag = "messaging.rocketmq.message.tag";

/**
  Type of message.
 */
static constexpr const char *kMessagingRocketmqMessageType = "messaging.rocketmq.message.type";

/**
  Namespace of RocketMQ resources, resources in different namespaces are individual.
 */
static constexpr const char *kMessagingRocketmqNamespace = "messaging.rocketmq.namespace";

/**
  Deprecated, use @code messaging.destination.subscription.name @endcode instead.

  @deprecated
  {"note": "Replaced by @code messaging.destination.subscription.name @endcode.", "reason":
  "renamed", "renamed_to": "messaging.destination.subscription.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char
    *kMessagingServicebusDestinationSubscriptionName =
        "messaging.servicebus.destination.subscription_name";

/**
  Describes the <a
  href="https://learn.microsoft.com/azure/service-bus-messaging/message-transfers-locks-settlement#peeklock">settlement
  type</a>.
 */
static constexpr const char *kMessagingServicebusDispositionStatus =
    "messaging.servicebus.disposition_status";

/**
  Number of deliveries that have been attempted for this message.
 */
static constexpr const char *kMessagingServicebusMessageDeliveryCount =
    "messaging.servicebus.message.delivery_count";

/**
  The UTC epoch seconds at which the message has been accepted and stored in the entity.
 */
static constexpr const char *kMessagingServicebusMessageEnqueuedTime =
    "messaging.servicebus.message.enqueued_time";

/**
  The messaging system as identified by the client instrumentation.
  <p>
  The actual messaging system may differ from the one known by the client. For example, when using
  Kafka client libraries to communicate with Azure Event Hubs, the @code messaging.system @endcode
  is set to @code kafka @endcode based on the instrumentation's best knowledge.
 */
static constexpr const char *kMessagingSystem = "messaging.system";

namespace MessagingOperationTypeValues
{
/**
  A message is created. "Create" spans always refer to a single message and are used to provide a
  unique creation context for messages in batch sending scenarios.
 */
static constexpr const char *kCreate = "create";

/**
  One or more messages are provided for sending to an intermediary. If a single message is sent, the
  context of the "Send" span can be used as the creation context and no "Create" span needs to be
  created.
 */
static constexpr const char *kSend = "send";

/**
  One or more messages are requested by a consumer. This operation refers to pull-based scenarios,
  where consumers explicitly call methods of messaging SDKs to receive messages.
 */
static constexpr const char *kReceive = "receive";

/**
  One or more messages are processed by a consumer.
 */
static constexpr const char *kProcess = "process";

/**
  One or more messages are settled.
 */
static constexpr const char *kSettle = "settle";

/**
  Deprecated. Use @code process @endcode instead.

  @deprecated
  {"note": "Replaced by @code process @endcode.", "reason": "renamed", "renamed_to": "process"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kDeliver = "deliver";

/**
  Deprecated. Use @code send @endcode instead.

  @deprecated
  {"note": "Replaced by @code send @endcode.", "reason": "renamed", "renamed_to": "send"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kPublish = "publish";

}  // namespace MessagingOperationTypeValues

namespace MessagingRocketmqConsumptionModelValues
{
/**
  Clustering consumption model
 */
static constexpr const char *kClustering = "clustering";

/**
  Broadcasting consumption model
 */
static constexpr const char *kBroadcasting = "broadcasting";

}  // namespace MessagingRocketmqConsumptionModelValues

namespace MessagingRocketmqMessageTypeValues
{
/**
  Normal message
 */
static constexpr const char *kNormal = "normal";

/**
  FIFO message
 */
static constexpr const char *kFifo = "fifo";

/**
  Delay message
 */
static constexpr const char *kDelay = "delay";

/**
  Transaction message
 */
static constexpr const char *kTransaction = "transaction";

}  // namespace MessagingRocketmqMessageTypeValues

namespace MessagingServicebusDispositionStatusValues
{
/**
  Message is completed
 */
static constexpr const char *kComplete = "complete";

/**
  Message is abandoned
 */
static constexpr const char *kAbandon = "abandon";

/**
  Message is sent to dead letter queue
 */
static constexpr const char *kDeadLetter = "dead_letter";

/**
  Message is deferred
 */
static constexpr const char *kDefer = "defer";

}  // namespace MessagingServicebusDispositionStatusValues

namespace MessagingSystemValues
{
/**
  Apache ActiveMQ
 */
static constexpr const char *kActivemq = "activemq";

/**
  Amazon Simple Notification Service (SNS)
 */
static constexpr const char *kAwsSns = "aws.sns";

/**
  Amazon Simple Queue Service (SQS)
 */
static constexpr const char *kAwsSqs = "aws_sqs";

/**
  Azure Event Grid
 */
static constexpr const char *kEventgrid = "eventgrid";

/**
  Azure Event Hubs
 */
static constexpr const char *kEventhubs = "eventhubs";

/**
  Azure Service Bus
 */
static constexpr const char *kServicebus = "servicebus";

/**
  Google Cloud Pub/Sub
 */
static constexpr const char *kGcpPubsub = "gcp_pubsub";

/**
  Java Message Service
 */
static constexpr const char *kJms = "jms";

/**
  Apache Kafka
 */
static constexpr const char *kKafka = "kafka";

/**
  RabbitMQ
 */
static constexpr const char *kRabbitmq = "rabbitmq";

/**
  Apache RocketMQ
 */
static constexpr const char *kRocketmq = "rocketmq";

/**
  Apache Pulsar
 */
static constexpr const char *kPulsar = "pulsar";

}  // namespace MessagingSystemValues

}  // namespace messaging
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
