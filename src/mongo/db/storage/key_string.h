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

        /**
         * Encodes info needed to restore the original BSONTypes from a KeyString. They cannot be
         * stored in place since we don't want them to affect the ordering (1 and 1.0 compare as
         * equal).
         */
        class TypeBits {
        public:
            // Sufficient bytes to encode extra type information for any BSON key that fits in 1KB.
            // The encoding format will need to change if we raise this limit.
            static const uint8_t kMaxBytesNeeded = 127;

            TypeBits() { reset(); }

            /**
             * If there are no bytes remaining, assumes AllZeros. Otherwise, reads bytes out of the
             * BufReader in the format described on the getBuffer() method.
             */
            void resetFromBuffer(BufReader* reader);
            static TypeBits fromBuffer(BufReader* reader) {
                TypeBits out;
                out.resetFromBuffer(reader);
                return out;
            }

            /**
             * If true, no bits have been set to one. This is true if no bits have been set at all.
             */
            bool isAllZeros() const { return _isAllZeros; }
            
            /**
             * These methods return a buffer and size which encodes all of the type bits in this
             * instance.
             *
             * Encoded format:
             * Case 1 (first byte has high bit set to 1):
             *     Remaining bits of first byte encode number of follow-up bytes that are data
             *     bytes. Note that _buf is always maintained in this format but these methods may
             *     return one of the other formats, if possible, by skipping over the first byte.
             *
             * Case 2 (first byte is 0x0):
             *     This encodes the "AllZeros" state which represents an infinite stream of bits set
             *     to 0. Callers may optionally encode this case as an empty buffer if they have
             *     another way to mark the end of the buffer. There are no follow-up bytes.
             *
             * Case 3 (first byte isn't 0x0 but has high bit set to 0):
             *     The first byte is the only data byte. This can represent any 7-bit sequence or an
             *     8-bit sequence if the 8th bit is 0, since the 8th bit is the same as the bit that
             *     is 1 if the first byte is the size byte. There are no follow-up bytes.
             *
             * Within data bytes (ie everything excluding the size byte if there is one), bits are
             * packed in from low to high.
             */
            const uint8_t* getBuffer() const { return getSize() == 1 ? _buf + 1 : _buf; }
            size_t getSize() const {
                if (_isAllZeros) { // Case 2
                    dassert(_buf[1] == 0);
                    return 1;
                }

                uint8_t rawSize = getSizeByte();
                dassert(rawSize >= 1); // 0 should be handled as isAllZeros.
                if (rawSize == 1 && !(_buf[1] & 0x80)) { // Case 3
                    return 1;
                }

                return rawSize + 1; // Case 1
            }
            
            //
            // Everything below is only for use by KeyString.
            //

            // Note: No space is used if all bits are 0 so the most common cases should be 0x0.
            static const uint8_t kString = 0x0;
            static const uint8_t kSymbol = 0x1;

            static const uint8_t kInt = 0x0;
            static const uint8_t kDouble = 0x1;
            static const uint8_t kLong = 0x2;
            static const uint8_t kNegativeZero = 0x3; // decodes as a double

            void reset() {
                _curBit = 0;
                _isAllZeros = true;
                setSizeByte(0);
                _buf[1] = 0;
            }

            void appendString() { appendBit(kString); }
            void appendSymbol() { appendBit(kSymbol); }

            void appendNumberDouble() { appendBit(kDouble & 1); appendBit(kDouble >> 1); }
            void appendNumberInt() { appendBit(kInt & 1); appendBit(kInt >> 1); }
            void appendNumberLong() { appendBit(kLong & 1); appendBit(kLong >> 1); }
            void appendNegativeZero() {
                appendBit(kNegativeZero & 1);
                appendBit(kNegativeZero >> 1);
            }

            class Reader {
            public:
                /**
                 * Passed in TypeBits must outlive this Reader instance.
                 */
                explicit Reader(const TypeBits& typeBits) : _curBit(0), _typeBits(typeBits) {}

                uint8_t readStringLike() { return readBit(); }
                uint8_t readNumeric() {
                    uint8_t lowBit = readBit();
                    return lowBit | (readBit() << 1);
                }

            private:
                uint8_t readBit();

                size_t _curBit;
                const TypeBits& _typeBits;
            };
            
        private:
            /**
             * size only includes data bytes, not the size byte itself.
             */
            uint8_t getSizeByte() const { return _buf[0] & 0x3f; }
            void setSizeByte(uint8_t size) {
                dassert(size < kMaxBytesNeeded);
                _buf[0] = 0x80 | size;
            }

            void appendBit(uint8_t oneOrZero);

            size_t _curBit;
            bool _isAllZeros;

            // See getBuffer()/getSize() documentation for a description of how data is encoded.
            // Currently whole buffer is copied in default copy methods. If they ever show up as hot
            // in profiling, we should add copy operations that only copy the parts of _buf that are
            // in use.
            uint8_t _buf[1/*size*/ + kMaxBytesNeeded];
        };

        static const uint64_t kMaxBufferSize = 2048;

        KeyString() : _size(0) {}

        static BSONObj toBson(StringData data, Ordering ord, const TypeBits& types);
        static BSONObj toBson(const char* buffer, size_t len, Ordering ord,
                              const TypeBits& types);

        /**
         * Decodes a RecordId from the end of a buffer.
         */
        static RecordId decodeRecordIdAtEnd(const void* buf, size_t size);

        /**
         * Decodes a RecordId, consuming all bytes needed from reader.
         */
        static RecordId decodeRecordId(BufReader* reader);

        static KeyString make(const BSONObj& obj,
                              Ordering ord,
                              RecordId recordId) {
            KeyString out;
            out.resetToKey(obj, ord, recordId);
            return out;
        }

        static KeyString make(const BSONObj& obj, Ordering ord) {
            KeyString out;
            out.resetToKey(obj, ord);
            return out;
        }

        static KeyString make(RecordId rid) {
            KeyString ks;
            ks.appendRecordId(rid);
            return ks;
        }

        void appendRecordId(RecordId loc);
        void appendTypeBits(const TypeBits& bits);

        /**
         * Resets to an empty state.
         * Equivalent to but faster than *this = KeyString()
         */
        void resetToEmpty() {
            _size = 0;
            _typeBits.reset();
        }

        void resetToKey(const BSONObj& obj, Ordering ord, RecordId recordId);
        void resetToKey(const BSONObj& obj, Ordering ord);
        void resetFromBuffer(const void* buffer, size_t size) {
            _size = size;
            memcpy(_buffer, buffer, size);
        }

        const char* getBuffer() const { return _buffer; }
        size_t getSize() const { return _size; }
        bool isEmpty() const { return _size == 0; }

        const TypeBits& getTypeBits() const { return _typeBits; }

        int compare(const KeyString& other) const;

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
        void _appendNumberDouble(const double num, bool invert);
        void _appendNumberLong(const long long num, bool invert);
        void _appendNumberInt(const int num, bool invert);

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
        void _appendSmallDouble(double value, bool invert);
        void _appendLargeDouble(double value, bool invert);
        void _appendInteger(const long long num, bool invert);
        void _appendPreshiftedIntegerPortion(uint64_t value, bool isNegative, bool invert);

        template <typename T> void _append(const T& thing, bool invert);
        void _appendBytes(const void* source, size_t bytes, bool invert);

        TypeBits _typeBits;
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
