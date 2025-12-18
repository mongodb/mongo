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
namespace rpc
{

/**
  The <a href="https://connectrpc.com//docs/protocol/#error-codes">error codes</a> of the Connect
  request. Error codes are always string values.
 */
static constexpr const char *kRpcConnectRpcErrorCode = "rpc.connect_rpc.error_code";

/**
  Connect request metadata, @code <key> @endcode being the normalized Connect Metadata key
  (lowercase), the value being the metadata values. <p> Instrumentations SHOULD require an explicit
  configuration of which metadata values are to be captured. Including all request metadata values
  can be a security risk - explicit configuration helps avoid leaking sensitive information. <p> For
  example, a property @code my-custom-key @endcode with value @code ["1.2.3.4", "1.2.3.5"] @endcode
  SHOULD be recorded as the @code rpc.connect_rpc.request.metadata.my-custom-key @endcode attribute
  with value @code ["1.2.3.4", "1.2.3.5"] @endcode
 */
static constexpr const char *kRpcConnectRpcRequestMetadata = "rpc.connect_rpc.request.metadata";

/**
  Connect response metadata, @code <key> @endcode being the normalized Connect Metadata key
  (lowercase), the value being the metadata values. <p> Instrumentations SHOULD require an explicit
  configuration of which metadata values are to be captured. Including all response metadata values
  can be a security risk - explicit configuration helps avoid leaking sensitive information. <p> For
  example, a property @code my-custom-key @endcode with value @code "attribute_value" @endcode
  SHOULD be recorded as the @code rpc.connect_rpc.response.metadata.my-custom-key @endcode attribute
  with value @code ["attribute_value"] @endcode
 */
static constexpr const char *kRpcConnectRpcResponseMetadata = "rpc.connect_rpc.response.metadata";

/**
  gRPC request metadata, @code <key> @endcode being the normalized gRPC Metadata key (lowercase),
  the value being the metadata values. <p> Instrumentations SHOULD require an explicit configuration
  of which metadata values are to be captured. Including all request metadata values can be a
  security risk - explicit configuration helps avoid leaking sensitive information. <p> For example,
  a property @code my-custom-key @endcode with value @code ["1.2.3.4", "1.2.3.5"] @endcode SHOULD be
  recorded as
  @code rpc.grpc.request.metadata.my-custom-key @endcode attribute with value @code ["1.2.3.4",
  "1.2.3.5"] @endcode
 */
static constexpr const char *kRpcGrpcRequestMetadata = "rpc.grpc.request.metadata";

/**
  gRPC response metadata, @code <key> @endcode being the normalized gRPC Metadata key (lowercase),
  the value being the metadata values. <p> Instrumentations SHOULD require an explicit configuration
  of which metadata values are to be captured. Including all response metadata values can be a
  security risk - explicit configuration helps avoid leaking sensitive information. <p> For example,
  a property @code my-custom-key @endcode with value @code ["attribute_value"] @endcode SHOULD be
  recorded as the @code rpc.grpc.response.metadata.my-custom-key @endcode attribute with value @code
  ["attribute_value"] @endcode
 */
static constexpr const char *kRpcGrpcResponseMetadata = "rpc.grpc.response.metadata";

/**
  The <a href="https://github.com/grpc/grpc/blob/v1.33.2/doc/statuscodes.md">numeric status code</a>
  of the gRPC request.
 */
static constexpr const char *kRpcGrpcStatusCode = "rpc.grpc.status_code";

/**
  @code error.code @endcode property of response if it is an error response.
 */
static constexpr const char *kRpcJsonrpcErrorCode = "rpc.jsonrpc.error_code";

/**
  @code error.message @endcode property of response if it is an error response.
 */
static constexpr const char *kRpcJsonrpcErrorMessage = "rpc.jsonrpc.error_message";

/**
  @code id @endcode property of request or response. Since protocol allows id to be int, string,
  @code null @endcode or missing (for notifications), value is expected to be cast to string for
  simplicity. Use empty string in case of @code null @endcode value. Omit entirely if this is a
  notification.
 */
static constexpr const char *kRpcJsonrpcRequestId = "rpc.jsonrpc.request_id";

/**
  Protocol version as in @code jsonrpc @endcode property of request/response. Since JSON-RPC 1.0
  doesn't specify this, the value can be omitted.
 */
static constexpr const char *kRpcJsonrpcVersion = "rpc.jsonrpc.version";

/**
  Compressed size of the message in bytes.
 */
static constexpr const char *kRpcMessageCompressedSize = "rpc.message.compressed_size";

/**
  MUST be calculated as two different counters starting from @code 1 @endcode one for sent messages
  and one for received message. <p> This way we guarantee that the values will be consistent between
  different implementations.
 */
static constexpr const char *kRpcMessageId = "rpc.message.id";

/**
  Whether this is a received or sent message.
 */
static constexpr const char *kRpcMessageType = "rpc.message.type";

/**
  Uncompressed size of the message in bytes.
 */
static constexpr const char *kRpcMessageUncompressedSize = "rpc.message.uncompressed_size";

/**
  This is the logical name of the method from the RPC interface perspective.
 */
static constexpr const char *kRpcMethod = "rpc.method";

/**
  The full (logical) name of the service being called, including its package name, if applicable.
 */
static constexpr const char *kRpcService = "rpc.service";

/**
  A string identifying the remoting system. See below for a list of well-known identifiers.
 */
static constexpr const char *kRpcSystem = "rpc.system";

namespace RpcConnectRpcErrorCodeValues
{

static constexpr const char *kCancelled = "cancelled";

static constexpr const char *kUnknown = "unknown";

static constexpr const char *kInvalidArgument = "invalid_argument";

static constexpr const char *kDeadlineExceeded = "deadline_exceeded";

static constexpr const char *kNotFound = "not_found";

static constexpr const char *kAlreadyExists = "already_exists";

static constexpr const char *kPermissionDenied = "permission_denied";

static constexpr const char *kResourceExhausted = "resource_exhausted";

static constexpr const char *kFailedPrecondition = "failed_precondition";

static constexpr const char *kAborted = "aborted";

static constexpr const char *kOutOfRange = "out_of_range";

static constexpr const char *kUnimplemented = "unimplemented";

static constexpr const char *kInternal = "internal";

static constexpr const char *kUnavailable = "unavailable";

static constexpr const char *kDataLoss = "data_loss";

static constexpr const char *kUnauthenticated = "unauthenticated";

}  // namespace RpcConnectRpcErrorCodeValues

namespace RpcGrpcStatusCodeValues
{
/**
  OK
 */
static constexpr int kOk = 0;

/**
  CANCELLED
 */
static constexpr int kCancelled = 1;

/**
  UNKNOWN
 */
static constexpr int kUnknown = 2;

/**
  INVALID_ARGUMENT
 */
static constexpr int kInvalidArgument = 3;

/**
  DEADLINE_EXCEEDED
 */
static constexpr int kDeadlineExceeded = 4;

/**
  NOT_FOUND
 */
static constexpr int kNotFound = 5;

/**
  ALREADY_EXISTS
 */
static constexpr int kAlreadyExists = 6;

/**
  PERMISSION_DENIED
 */
static constexpr int kPermissionDenied = 7;

/**
  RESOURCE_EXHAUSTED
 */
static constexpr int kResourceExhausted = 8;

/**
  FAILED_PRECONDITION
 */
static constexpr int kFailedPrecondition = 9;

/**
  ABORTED
 */
static constexpr int kAborted = 10;

/**
  OUT_OF_RANGE
 */
static constexpr int kOutOfRange = 11;

/**
  UNIMPLEMENTED
 */
static constexpr int kUnimplemented = 12;

/**
  INTERNAL
 */
static constexpr int kInternal = 13;

/**
  UNAVAILABLE
 */
static constexpr int kUnavailable = 14;

/**
  DATA_LOSS
 */
static constexpr int kDataLoss = 15;

/**
  UNAUTHENTICATED
 */
static constexpr int kUnauthenticated = 16;

}  // namespace RpcGrpcStatusCodeValues

namespace RpcMessageTypeValues
{

static constexpr const char *kSent = "SENT";

static constexpr const char *kReceived = "RECEIVED";

}  // namespace RpcMessageTypeValues

namespace RpcSystemValues
{
/**
  gRPC
 */
static constexpr const char *kGrpc = "grpc";

/**
  Java RMI
 */
static constexpr const char *kJavaRmi = "java_rmi";

/**
  .NET WCF
 */
static constexpr const char *kDotnetWcf = "dotnet_wcf";

/**
  Apache Dubbo
 */
static constexpr const char *kApacheDubbo = "apache_dubbo";

/**
  Connect RPC
 */
static constexpr const char *kConnectRpc = "connect_rpc";

/**
  <a href="https://datatracker.ietf.org/doc/html/rfc5531">ONC RPC (Sun RPC)</a>
 */
static constexpr const char *kOncRpc = "onc_rpc";

/**
  JSON-RPC
 */
static constexpr const char *kJsonrpc = "jsonrpc";

}  // namespace RpcSystemValues

}  // namespace rpc
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
