// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/pcre.h"

#include <string>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

/**
 * This library collects Mongo-specific PCRE conventions which are useful
 * but shouldn't be part of the main pcre.h library.
 */
namespace mongo::pcre_util {
/**
 * Builds `pcre::CompileOptions` from the input options string.
 * The `pcre::UTF` option is also set by default.
 * Throws `uassert` 51108 on invalid flags including the `opName` in its reason.
 *
 * Valid flags:
 *   'i': CASELESS
 *   'm': MULTILINE
 *   's': DOTALL
 *   'u': UTF (redundant, but accepted)
 *   'x': EXTENDED
 */
pcre::CompileOptions flagsToOptions(std::string_view optionFlags, std::string_view opName = "");

/**
 * Builds an std::string of flag characters from the input 'pcre::CompileOptions'.
 * These flags are the same as those documented in flagsToOptions. They are returned in alphabetical
 * order. Since 'u' is redundant, it will never be output by this function.
 */
std::string optionsToFlags(pcre::CompileOptions opt);

/**
 * Escapes all potentially meaningful regex characters in the provided string.
 * The returned string, used as a `mongo::pcre::Regex`, will match `str`.
 */
std::string quoteMeta(std::string_view str);

}  // namespace mongo::pcre_util
