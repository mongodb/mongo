/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#ifndef __cpp_lib_variant
#include "third_party/variant-1.3.0/include/mpark/variant.hpp"
#else
#include <variant>
#endif

namespace mongo {
namespace stdx {
#if __cplusplus < 201703L || !defined(__cpp_lib_variant)
using ::mpark::variant;
using ::mpark::visit;
using ::mpark::holds_alternative;
using ::mpark::get;
using ::mpark::get_if;

using ::mpark::variant_size;
using ::mpark::variant_size_v;
using ::mpark::variant_alternative;
using ::mpark::variant_alternative_t;

constexpr auto variant_npos = ::mpark::variant_npos;

using ::mpark::operator==;
using ::mpark::operator!=;
using ::mpark::operator<;
using ::mpark::operator>;
using ::mpark::operator<=;
using ::mpark::operator>=;

using ::mpark::monostate;
using ::mpark::bad_variant_access;

#else
using ::std::variant;
using ::std::visit;
using ::std::holds_alternative;
using ::std::get;
using ::std::get_if;

using ::std::variant_size;
using ::std::variant_size_v;
using ::std::variant_alternative;
using ::std::variant_alternative_t;

constexpr auto variant_npos = ::std::variant_npos;

using ::std::operator==;
using ::std::operator!=;
using ::std::operator<;
using ::std::operator>;
using ::std::operator<=;
using ::std::operator>=;

using ::std::monostate;
using ::std::bad_variant_access;

#endif

}  // namespace stdx
}  // namespace mongo
