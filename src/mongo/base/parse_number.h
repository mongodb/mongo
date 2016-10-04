/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Utility functions for parsing numbers from strings.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

namespace mongo {

/**
 * Parses a number out of a StringData.
 *
 * Parses "stringValue", interpreting it as a number of the given "base".  On success, stores
 * the parsed value into "*result" and returns Status::OK().
 *
 * Valid values for "base" are 2-36, with 0 meaning "choose the base by inspecting the prefix
 * on the number", as in strtol.  Returns Status::BadValue if an illegal value is supplied for
 * "base".
 *
 * The entirety of the std::string must consist of digits in the given base, except optionally the
 * first character may be "+" or "-", and hexadecimal numbers may begin "0x".  Same as strtol,
 * without the property of stripping whitespace at the beginning, and fails to parse if there
 * are non-digit characters at the end of the string.
 *
 * See parse_number.cpp for the available instantiations, and add any new instantiations there.
 */
template <typename NumberType>
Status parseNumberFromStringWithBase(StringData stringValue, int base, NumberType* result);

template <typename NumberType>
static Status parseNumberFromString(StringData stringValue, NumberType* result) {
    return parseNumberFromStringWithBase(stringValue, 0, result);
}

}  // namespace mongo
