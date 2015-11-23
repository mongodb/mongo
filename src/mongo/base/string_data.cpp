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

#include "mongo/base/string_data.h"

#include <ostream>
#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"

namespace mongo {

namespace {

template <int SizeofSizeT>
size_t murmur3(StringData str);

template <>
size_t murmur3<4>(StringData str) {
    char hash[4];
    MurmurHash3_x86_32(str.rawData(), str.size(), 0, &hash);
    return ConstDataView(hash).read<LittleEndian<std::uint32_t>>();
}

template <>
size_t murmur3<8>(StringData str) {
    char hash[16];
    MurmurHash3_x64_128(str.rawData(), str.size(), 0, hash);
    return static_cast<size_t>(ConstDataView(hash).read<LittleEndian<std::uint64_t>>());
}

}  // namespace

std::ostream& operator<<(std::ostream& stream, StringData value) {
    return stream.write(value.rawData(), value.size());
}

size_t StringData::Hasher::operator()(StringData str) const {
    return murmur3<sizeof(size_t)>(str);
}

}  // namespace mongo
