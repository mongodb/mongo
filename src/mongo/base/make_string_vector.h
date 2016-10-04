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

#pragma once

#include <string>
#include <vector>

/**
 * Utility macro to construct a std::vector<std::string> from a sequence of C-style
 * strings.
 *
 * Usage:  MONGO_MAKE_STRING_VECTOR("a", "b", "c") returns a vector containing
 * std::strings "a", "b", "c", in that order.
 */
#define MONGO_MAKE_STRING_VECTOR(...) ::mongo::_makeStringVector(0, __VA_ARGS__, NULL)

namespace mongo {

/**
 * Create a vector of strings from varargs of C-style strings.
 *
 * WARNING: Only intended for use by MONGO_MAKE_STRING_VECTOR macro, defined above.  Aborts
 * ungracefully if you misuse it, so stick to the macro.
 *
 * The first parameter is ignored in all circumstances. The subsequent parameters must be
 * const char* C-style strings, or NULL. Of these parameters, at least one must be
 * NULL. Parameters at and beyond the NULL are not inserted. Typically, the NULL will be
 * the last parameter. The MONGO_MAKE_STRING_VECTOR macro enforces this.
 *
 * Returns a vector of std::strings.
 */
std::vector<std::string> _makeStringVector(int ignored, ...);

}  // namespace mongo
