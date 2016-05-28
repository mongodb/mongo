/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/platform/process_id.h"

#include <iostream>
#include <limits>
#include <sstream>

namespace mongo {

static_assert(sizeof(NativeProcessId) == sizeof(uint32_t),
              "sizeof(NativeProcessId) == sizeof(uint32_t)");

namespace {
#ifdef _WIN32
inline NativeProcessId getCurrentNativeProcessId() {
    return GetCurrentProcessId();
}
#else
inline NativeProcessId getCurrentNativeProcessId() {
    return getpid();
}
#endif
}  // namespace

ProcessId ProcessId::getCurrent() {
    return fromNative(getCurrentNativeProcessId());
}

int64_t ProcessId::asInt64() const {
    typedef std::numeric_limits<NativeProcessId> limits;
    if (limits::is_signed)
        return _npid;
    else
        return static_cast<int64_t>(static_cast<uint64_t>(_npid));
}

long long ProcessId::asLongLong() const {
    return static_cast<long long>(asInt64());
}

std::string ProcessId::toString() const {
    std::ostringstream os;
    os << *this;
    return os.str();
}

std::ostream& operator<<(std::ostream& os, ProcessId pid) {
    return os << pid.toNative();
}

}  // namespace mongo
