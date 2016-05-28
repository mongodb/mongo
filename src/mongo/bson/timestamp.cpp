/*    Copyright 2009 10gen Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/bson/bsontypes.h"

#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <sstream>

#include "mongo/util/time_support.h"

namespace mongo {

Timestamp Timestamp::max() {
    unsigned int t = static_cast<unsigned int>(std::numeric_limits<uint32_t>::max());
    unsigned int i = std::numeric_limits<uint32_t>::max();
    return Timestamp(t, i);
}

void Timestamp::append(BufBuilder& builder, const StringData& fieldName) const {
    // No endian conversions needed, since we store in-memory representation
    // in little endian format, regardless of target endian.
    builder.appendNum(static_cast<char>(bsonTimestamp));
    builder.appendStr(fieldName);
    builder.appendNum(asULL());
}

std::string Timestamp::toStringLong() const {
    std::stringstream ss;
    ss << time_t_to_String_short(secs) << ' ';
    ss << std::hex << secs << ':' << i;
    return ss.str();
}

std::string Timestamp::toStringPretty() const {
    std::stringstream ss;
    ss << time_t_to_String_short(secs) << ':' << std::hex << i;
    return ss.str();
}

std::string Timestamp::toString() const {
    std::stringstream ss;
    ss << std::hex << secs << ':' << i;
    return ss.str();
}
}
