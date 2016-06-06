/*    Copyright 2016 MongoDB, Inc.
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

#include "mongo/util/duration.h"

#include <iostream>

#include "mongo/bson/util/builder.h"

namespace mongo {
namespace {
template <typename Stream>
Stream& streamPut(Stream& os, Nanoseconds ns) {
    return os << ns.count() << "ns";
}

template <typename Stream>
Stream& streamPut(Stream& os, Microseconds us) {
    return os << us.count() << "\xce\xbcs";
}

template <typename Stream>
Stream& streamPut(Stream& os, Milliseconds ms) {
    return os << ms.count() << "ms";
}

template <typename Stream>
Stream& streamPut(Stream& os, Seconds s) {
    return os << s.count() << 's';
}

template <typename Stream>
Stream& streamPut(Stream& os, Minutes min) {
    return os << min.count() << "min";
}

template <typename Stream>
Stream& streamPut(Stream& os, Hours hrs) {
    return os << hrs.count() << "hr";
}

}  // namespace

std::ostream& operator<<(std::ostream& os, Nanoseconds ns) {
    return streamPut(os, ns);
}

std::ostream& operator<<(std::ostream& os, Microseconds us) {
    return streamPut(os, us);
}

std::ostream& operator<<(std::ostream& os, Milliseconds ms) {
    return streamPut(os, ms);
}
std::ostream& operator<<(std::ostream& os, Seconds s) {
    return streamPut(os, s);
}

std::ostream& operator<<(std::ostream& os, Minutes m) {
    return streamPut(os, m);
}

std::ostream& operator<<(std::ostream& os, Hours h) {
    return streamPut(os, h);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Nanoseconds ns) {
    return streamPut(os, ns);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Microseconds us) {
    return streamPut(os, us);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Milliseconds ms) {
    return streamPut(os, ms);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Seconds s) {
    return streamPut(os, s);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Minutes m) {
    return streamPut(os, m);
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Hours h) {
    return streamPut(os, h);
}

template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&,
                                                       Nanoseconds);
template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&,
                                                       Microseconds);
template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&,
                                                       Milliseconds);
template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&, Seconds);
template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&, Minutes);
template StringBuilderImpl<StackAllocator>& operator<<(StringBuilderImpl<StackAllocator>&, Hours);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Nanoseconds);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Microseconds);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Milliseconds);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Seconds);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Minutes);
template StringBuilderImpl<SharedBufferAllocator>& operator<<(
    StringBuilderImpl<SharedBufferAllocator>&, Hours);
}  // namespace mongo
