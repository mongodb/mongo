/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <system_error>
#include <type_traits>

#include "mongo/base/error_codes.h"

namespace mongo {

const std::error_category& mongoErrorCategory();

// The next two functions are explicitly named (contrary to our naming style) so that they can be
// picked up by ADL.
std::error_code make_error_code(ErrorCodes::Error code);

std::error_condition make_error_condition(ErrorCodes::Error code);

}  // namespace mongo

namespace std {

/**
 * Allows a std::error_condition to be implicitly constructed from a mongo::ErrorCodes::Error.
 * We specialize this instead of is_error_code_enum as our ErrorCodes are platform independent.
 */
template <>
struct is_error_condition_enum<mongo::ErrorCodes::Error> : public true_type {};

}  // namespace std
