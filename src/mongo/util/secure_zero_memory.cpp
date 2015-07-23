/*    Copyright 2014 MongoDB Inc.
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

#include "mongo/config.h"

#if defined(MONGO_CONFIG_HAVE_MEMSET_S)
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include "mongo/platform/basic.h"

#include <cstring>

#include "mongo/util/assert_util.h"

namespace mongo {

void secureZeroMemory(void* mem, size_t size) {
    if (mem == nullptr) {
        fassert(28751, size == 0);
        return;
    }

#if defined(_WIN32)
    // Windows provides a simple function for zeroing memory
    SecureZeroMemory(mem, size);
#elif defined(MONGO_CONFIG_HAVE_MEMSET_S)
    // Some C11 libraries provide a variant of memset which is guaranteed to not be optimized away
    fassert(28752, memset_s(mem, size, 0, size) == 0);
#else
    // fall back to using volatile pointer
    volatile char* p = reinterpret_cast<volatile char*>(mem);
    while (size--) {
        *p++ = 0;
    }
#endif
}

}  // namespace mongo
