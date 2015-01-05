// key_string.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/optime.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/bson_db.h"
#include "mongo/db/record_id.h"

namespace mongo {

    class KeyString {
    public:
        static const uint64_t kMaxBufferSize = 2048;

        KeyString() : _size(0) {}

        static BSONObj toBson(const char* buffer, size_t len, Ordering ord);
        static BSONObj toBson(StringData data, Ordering ord);

        static size_t numBytesForRecordIdStartingAt(const void* ptr) {
            return 2 + (*static_cast<const unsigned char*>(ptr) >> 5); // stored in high 3 bits.
        }
        static size_t numBytesForRecordIdEndingAt(const void* ptr) {
            return 2 + (*static_cast<const unsigned char*>(ptr) & 7); // stored in low 3 bits.
        }
        static RecordId decodeRecordIdEndingAt(const void* ptr) {
            const char* base = static_cast<const char*>(ptr);
            base -= numBytesForRecordIdEndingAt(ptr) - 1;
            return decodeRecordIdStartingAt(base);
        }
        static RecordId decodeRecordIdStartingAt(const void* ptr);

        static KeyString make(const BSONObj& obj,
                              Ordering ord,
                              RecordId recordId);

        static KeyString make(const BSONObj& obj,
                              Ordering ord);

        static KeyString make(RecordId rid) {
            KeyString ks;
            ks.appendRecordId(rid);
            return ks;
        }

        void appendRecordId(RecordId loc);

        /**
         * Resets to an empty state.
         * Equivalent to but faster than *this = KeyString()
         */
        void reset() { _size = 0; }

        const char* getBuffer() const { return _buffer; }
        size_t getSize() const { return _size; }

        int compare(const KeyString& other) const;

        BSONObj toBson(Ordering ord) const;

        /**
         * @return a hex encoding of this key
         */
        std::string toString() const;

    private:

        void _appendAllElementsForIndexing(const BSONObj& obj, Ordering ord);

        void _appendBool(bool val, bool invert);
        void _appendDate(Date_t val, bool invert);
        void _appendTimestamp(OpTime val, bool invert);
        void _appendOID(OID val, bool invert);
        void _appendString(StringData val, bool invert);
        void _appendSymbol(StringData val, bool invert);
        void _appendCode(StringData val, bool invert);
        void _appendCodeWString(const BSONCodeWScope& val, bool invert);
        void _appendBinData(const BSONBinData& val, bool invert);
        void _appendRegex(const BSONRegEx& val, bool invert);
        void _appendDBRef(const BSONDBRef& val, bool invert);
        void _appendArray(const BSONArray& val, bool invert);
        void _appendObject(const BSONObj& val, bool invert);

        void _appendDouble(const double num, bool invert);
        void _appendLongLong(const long long num, bool invert);
        void _appendInt(const int num, bool invert);

        /**
         * @param name - optional, can be NULL
         *              if NULL, not included in encoding
         *              if not NULL, put in after type, before value
         */
        void _appendBsonValue(const BSONElement& elem,
                              bool invert,
                              const StringData* name);

        void _appendStringLike(StringData str, bool invert);
        void _appendBson(const BSONObj& obj, bool invert);
        void _appendSmallDouble(double magnitude, bool invert);
        void _appendLargeDouble(double magnitude, bool invert);
        void _appendLargeInt64(long long magnitude, bool invert);

        template <typename T> void _append(const T& thing, bool invert);
        void _appendBytes(const void* source, size_t bytes, bool invert);

        size_t _size;
        char _buffer[kMaxBufferSize];
    };

    inline bool operator<(const KeyString& lhs, const KeyString& rhs) {
        return lhs.compare(rhs) < 0;
    }

    inline bool operator<=(const KeyString& lhs, const KeyString& rhs) {
        return lhs.compare(rhs) <= 0;
    }

    inline bool operator==(const KeyString& lhs, const KeyString& rhs) {
        return lhs.compare(rhs) == 0;
    }

    inline bool operator>(const KeyString& lhs, const KeyString& rhs) {
        return lhs.compare(rhs) > 0;
    }

    inline bool operator>=(const KeyString& lhs, const KeyString& rhs) {
        return lhs.compare(rhs) >= 0;
    }

    inline bool operator!=(const KeyString& lhs, const KeyString& rhs) {
        return !(lhs == rhs);
    }

    inline std::ostream& operator<<(std::ostream& stream, const KeyString& value) {
        return stream << value.toString();
    }

} // namespace mongo
