// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Unicode string prepare options.
 * By default, unassigned codepoints in the input string will result in an error.
 * Using the AllowUnassigned option will pass them through without change,
 * which may not turn out to be appropriate in later Unicode standards.
 */
enum UStringPrepOptions {
    kUStringPrepDefault = 0,
    kUStringPrepAllowUnassigned = 1,
};

/**
 * Attempt to apply RFC4013 saslPrep to the target string.
 * Normalizes unicode sequences for SCRAM authentication.
 */
StatusWith<std::string> icuSaslPrep(std::string_view str, UStringPrepOptions = kUStringPrepDefault);

/**
 * Attempt to apply RFC4518 string prep to the target string, this normalizes an X509 DN
 * so it can be compared against other X509 DNs
 */
StatusWith<std::string> icuX509DNPrep(std::string_view str);

// Similar to mk_wcswidth, but use the larger unicode database for character lookup.
int icuGetStringWidth(std::string_view str, bool ambiguousAsFullWidth, bool expandEmojiSequence);

/**
 * Returns a Unicode case-folded copy of `str`.
 * Case folding maps characters to a canonical lowercase form suitable for case-insensitive
 * comparison.
 */
std::string icuCaseFold(std::string_view str);

}  // namespace mongo
