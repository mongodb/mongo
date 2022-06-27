/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/util/pcre.h"

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
pcre::CompileOptions flagsToOptions(StringData optionFlags, StringData opName = "");

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
std::string quoteMeta(StringData str);

}  // namespace mongo::pcre_util
