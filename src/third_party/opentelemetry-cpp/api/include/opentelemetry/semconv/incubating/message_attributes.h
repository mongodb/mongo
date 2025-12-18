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
namespace message
{

/**
  Deprecated, use @code rpc.message.compressed_size @endcode instead.

  @deprecated
  {"note": "Replaced by @code rpc.message.compressed_size @endcode.", "reason": "renamed",
  "renamed_to": "rpc.message.compressed_size"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessageCompressedSize =
    "message.compressed_size";

/**
  Deprecated, use @code rpc.message.id @endcode instead.

  @deprecated
  {"note": "Replaced by @code rpc.message.id @endcode.", "reason": "renamed", "renamed_to":
  "rpc.message.id"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessageId = "message.id";

/**
  Deprecated, use @code rpc.message.type @endcode instead.

  @deprecated
  {"note": "Replaced by @code rpc.message.type @endcode.", "reason": "renamed", "renamed_to":
  "rpc.message.type"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessageType = "message.type";

/**
  Deprecated, use @code rpc.message.uncompressed_size @endcode instead.

  @deprecated
  {"note": "Replaced by @code rpc.message.uncompressed_size @endcode.", "reason": "renamed",
  "renamed_to": "rpc.message.uncompressed_size"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMessageUncompressedSize =
    "message.uncompressed_size";

namespace MessageTypeValues
{

static constexpr const char *kSent = "SENT";

static constexpr const char *kReceived = "RECEIVED";

}  // namespace MessageTypeValues

}  // namespace message
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
