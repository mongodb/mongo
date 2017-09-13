/*    Copyright 2015 MongoDB Inc.
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

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/debug_util.h"

namespace mongo {

/**
 * This include exists so that mongo/base/status_with.h can use the invariant macro without causing
 * a circular include chain. It should never be included directly in any other file other than that
 * one (and assert_util.h).
 */

#if !defined(MONGO_INCLUDE_INVARIANT_H_WHITELISTED)
#error "Include assert_util.h instead of invariant.h."
#endif

MONGO_COMPILER_NORETURN void invariantFailed(const char* expr,
                                             const char* file,
                                             unsigned line) noexcept;

#define MONGO_invariant(_Expression)                                    \
    do {                                                                \
        if (MONGO_unlikely(!(_Expression))) {                           \
            ::mongo::invariantFailed(#_Expression, __FILE__, __LINE__); \
        }                                                               \
    } while (false)

#define invariant MONGO_invariant

// Behaves like invariant in debug builds and is compiled out in release. Use for checks, which can
// potentially be slow or on a critical path.
#define MONGO_dassert(x) \
    if (kDebugBuild)     \
    invariant(x)

#define dassert MONGO_dassert

}  // namespace mongo
