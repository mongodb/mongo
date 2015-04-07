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

#pragma once

#include <type_traits>

#include "mongo/base/data_cursor.h"
#include "mongo/base/data_view.h"
#include "mongo/platform/cstdint.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"

namespace mongo {
    class StringData;

    /**
     * Timestamp: A combination of current second plus an ordinal value, held together in a
     * single 64-bit integer, stored in memory as little endian, regardless of local endianness.
     */
    class Timestamp {
     public:

        Timestamp() = default;

        explicit Timestamp(Date_t date) {
            _data = endian::nativeToLittle(date.millis);
            dassert(static_cast<int>(getSecs()) >= 0);
        }

        Timestamp(unsigned secs, unsigned inc) {
            DataCursor(reinterpret_cast<char*>(&_data))
                .writeLEAndAdvance<uint32_t>(inc)
                .writeLE<uint32_t>(secs);
        }

        // Maximum Timestamp value.
        static Timestamp max();

        unsigned getSecs() const {
            static_assert(sizeof(unsigned) == sizeof(uint32_t), "unsigned must be uint32");
            return ConstDataCursor(reinterpret_cast<const char*>(&_data))
                .skip<uint32_t>()
                .readLE<uint32_t>();
        }

        unsigned getInc() const {
            static_assert(sizeof(unsigned) == sizeof(uint32_t), "unsigned must be uint32");
            return ConstDataCursor(reinterpret_cast<const char*>(&_data))
                .readLE<uint32_t>();
        }

        unsigned long long asULL() const {
            return endian::littleToNative(_data);
        }

        long long asLL() const {
            const unsigned long long val = endian::littleToNative(_data);
            return static_cast<long long>(val);
        }

        bool isNull() const {
            return getSecs() == 0;
        }

        // Append the BSON representation of this Timestamp to the given BufBuilder with the given
        // name. This lives here because Timestamp manages its own serialization format.
        void append(BufBuilder& builder, const StringData& fieldName) const;

        // Set the value of this Timestamp to match that of the pointed to bytes. The
        // return value points to the first byte not consumed by the read operation.
        const void* readFrom(const void* bytes);

        std::string toStringLong() const;

        std::string toStringPretty() const;

        std::string toString() const;

    private:
        uint64_t _data = 0;
    };

    inline bool operator==(const Timestamp& lhs, const Timestamp& rhs) {
        return (lhs.getInc() == rhs.getInc()) && (lhs.getSecs() == rhs.getSecs());
    }

    inline bool operator!=(const Timestamp& lhs, const Timestamp& rhs) {
        return !(lhs == rhs);
    }

    inline bool operator<(const Timestamp& lhs, const Timestamp& rhs) {
        if ( lhs.getSecs() != rhs.getSecs() ) {
            return lhs.getSecs() < rhs.getSecs();
        }
        return lhs.getInc() < rhs.getInc();
    }

    inline bool operator<=(const Timestamp& lhs, const Timestamp& rhs) {
        return (lhs < rhs) || (lhs == rhs);
    }

    inline bool operator>(const Timestamp& lhs, const Timestamp& rhs) {
        return !(lhs <= rhs);
    }

    inline bool operator>=(const Timestamp& lhs, const Timestamp& rhs) {
        return !(lhs < rhs);
    }

} // namespace mongo
