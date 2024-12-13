// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
// Base64Escape()
//
// Encodes a `src` string into a base64-encoded 'dest' string with padding
// characters. This function conforms with RFC 4648 section 4 (base64) and RFC
// 2045.
OPENTELEMETRY_EXPORT void Base64Escape(opentelemetry::nostd::string_view src, std::string *dest);
OPENTELEMETRY_EXPORT std::string Base64Escape(opentelemetry::nostd::string_view src);

// Base64Unescape()
//
// Converts a `src` string encoded in Base64 (RFC 4648 section 4) to its binary
// equivalent, writing it to a `dest` buffer, returning `true` on success. If
// `src` contains invalid characters, `dest` is cleared and returns `false`.
// If padding is included (note that `Base64Escape()` does produce it), it must
// be correct. In the padding, '=' are treated identically.
OPENTELEMETRY_EXPORT bool Base64Unescape(opentelemetry::nostd::string_view src, std::string *dest);

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
