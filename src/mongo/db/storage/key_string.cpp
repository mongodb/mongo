// key_string.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <boost/scoped_array.hpp>

#include "mongo/base/data_view.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace {
        namespace CType {
            // canonical types namespace. (would be enum class CType: uint8_t in C++11)
            // Note 0 and 255 are disallowed and reserved for value encodings
            const uint8_t kMinKey = 10;
            const uint8_t kUndefined = 15;
            const uint8_t kNullish = 20;
            const uint8_t kNumeric = 30;
            const uint8_t kString = 40;
            const uint8_t kObject = 50;
            const uint8_t kArray = 60;
            const uint8_t kBinData = 70;
            const uint8_t kOID = 80;
            const uint8_t kBool = 90;
            const uint8_t kDate = 100;
            const uint8_t kTimestamp = 110;
            const uint8_t kRegEx = 120;
            const uint8_t kDBRef = 130;
            const uint8_t kCode = 140;
            const uint8_t kCodeWithScope = 150;
            const uint8_t kMaxKey = 240;
        } // namespace CType

        uint8_t bsonTypeToKeyStringType(BSONType type) {
            switch (type) {
            case MinKey:
                return CType::kMinKey;

            case EOO:
            case jstNULL:
                return CType::kNullish;

            case Undefined:
                return CType::kUndefined;

            case NumberDouble:
            case NumberInt:
            case NumberLong:
                return CType::kNumeric;

            case mongo::String:
            case Symbol:
                return CType::kString;

            case Object: return CType::kObject;
            case Array: return CType::kArray;
            case BinData: return CType::kBinData;
            case jstOID: return CType::kOID;
            case Bool: return CType::kBool;
            case Date: return CType::kDate;
            case Timestamp: return CType::kTimestamp;
            case RegEx: return CType::kRegEx;
            case DBRef: return CType::kDBRef;

            case Code: return CType::kCode;
            case CodeWScope: return CType::kCodeWithScope;

            case MaxKey: return CType::kMaxKey;
            default:
                invariant(false);
            }
        }

        // Where doubles go integer-only - i.e. no decimals.
        const int64_t kMinLargeInt64 = 1ll << 52;

        // First double that isn't an int64.
        const double kMinLargeDouble = 9223372036854775808.0; // 1ULL<<63

        const uint8_t kEnd = 0x4;

        const uint8_t kEqual = 0x2;
        const uint8_t kLess = kEqual - 1;
        const uint8_t kGreater = kEqual + 1;

        const uint8_t kNaN = 0x00;
        const uint8_t kZero = 0x80;
        const uint8_t kSmallDouble = kZero + 1;
        const uint8_t kLargeInt64 = kZero + 2;
        const uint8_t kLargeDouble = kZero + 3;
    } // namespace

    // some utility functions
    namespace {
        void memcpy_flipBits(void* dst, const void* src, size_t bytes) {
            const char* input = static_cast<const char*>(src);
            char* output = static_cast<char*>(dst);
            const char* const end = input + bytes;
            while (input != end) {
                *output++ = ~(*input++);
            }
        }

        template <typename T> T readType(BufReader* reader, bool inverted) {
            // TODO for C++11 to static_assert that T is integral
            T t = ConstDataView(static_cast<const char*>(reader->skip(sizeof(T)))).readNative<T>();
            if (inverted)
                return ~t;
            return t;
        }

        StringData readCString(BufReader* reader) {
            const char* start = static_cast<const char*>(reader->pos());
            const char* end = static_cast<const char*>(memchr(start, 0x0, reader->remaining()));
            invariant(end);
            size_t actualBytes = end - start;
            invariant(actualBytes < KeyString::kMaxBufferSize);
            reader->skip(1 + actualBytes);
            return StringData(start, actualBytes);
        }

        string readInvertedCString(BufReader* reader) {
            const char* start = static_cast<const char*>(reader->pos());
            const char* end = static_cast<const char*>(memchr(start, 0xFF, reader->remaining()));
            invariant(end);
            size_t actualBytes = end - start;
            invariant(actualBytes < KeyString::kMaxBufferSize);
            string s(start, actualBytes);
            for (size_t i = 0; i < s.size(); i++) {
                s[i] = ~s[i];
            }
            reader->skip(1 + actualBytes);
            return s;
        }
    } // namespace

    KeyString KeyString::make(const BSONObj& obj, Ordering ord, RecordId recordId) {
        KeyString out;
        out._appendAllElementsForIndexing(obj, ord);
        out._appendRecordId(recordId);
        return out;
    }

    KeyString KeyString::make(const BSONObj& obj, Ordering ord) {
        KeyString out;
        out._appendAllElementsForIndexing(obj, ord);
        return out;
    }

    // ----------------------------------------------------------------------
    // -----------   APPEND CODE  -------------------------------------------
    // ----------------------------------------------------------------------

    void KeyString::_appendAllElementsForIndexing(const BSONObj& obj, Ordering ord) {
        int elemCount = 0;
        BSONForEach(elem, obj) {
            const int elemIdx = elemCount++;
            const bool invert = (ord.get(elemIdx) == -1);

            _appendBsonValue(elem, invert, NULL);

            dassert(elem.fieldNameSize() < 3); // fieldNameSize includes the NUL

            // These are used in IndexEntryComparison::makeQueryObject()
            switch (*elem.fieldName()) {
            case '\0': _append(kEqual, false); break;
            case 'l':  _append(kLess, false); break;
            case 'g':  _append(kGreater, false); break;
            }
        }
        _append(kEnd, false);
    }

    void KeyString::_appendRecordId(RecordId loc) {
        int64_t raw = loc.repr();
        if (raw < 0) {
            // Note: we encode RecordId::min() and RecordId() the same
            // which is ok, as they are never stored.
            invariant(raw == RecordId::min().repr());
            raw = 0;
        }
        uint64_t value = static_cast<uint64_t>(raw);
        _append(endian::nativeToBig(value), false); // never invert RecordIds
    }

    void KeyString::_appendBool(bool val, bool invert) {
        _append(int8_t(val ? 1 : 0), invert);
    }

    void KeyString::_appendDate(Date_t val, bool invert) {
        // see: http://en.wikipedia.org/wiki/Offset_binary
        uint64_t encoded = static_cast<uint64_t>(val.asInt64());
        encoded ^= (1LL << 63); // flip highest bit (equivalent to bias encoding)
        _append(endian::nativeToBig(encoded), invert);
    }

    void KeyString::_appendTimestamp(OpTime val, bool invert) {
        _append(endian::nativeToBig(val.asLL()), invert);
    }

    void KeyString::_appendOID(OID val, bool invert) {
        _appendBytes(val.view().view(), OID::kOIDSize, invert);
    }

    void KeyString::_appendString(StringData val, bool invert) {
        _appendStringLike(val, invert);
    }

    void KeyString::_appendSymbol(StringData val, bool invert) {
        _appendStringLike(val, invert);
    }

    void KeyString::_appendCode(StringData val, bool invert) {
        _appendStringLike(val, invert);
    }

    void KeyString::_appendCodeWString(const BSONCodeWScope& val, bool invert) {
        _appendStringLike(val.code, invert);
        _appendBson(val.scope, invert);
    }

    void KeyString::_appendBinData(const BSONBinData& val, bool invert) {
        _append(endian::nativeToBig(int32_t(val.length)), invert);
        _append(uint8_t(val.type), invert);
        _appendBytes(val.data, val.length, invert);
    }

    void KeyString::_appendRegex(const BSONRegEx& val, bool invert) {
        // note: NULL is not allowed in pattern or flags
        _appendBytes(val.pattern.rawData(), val.pattern.size(), invert);
        _append(int8_t(0), invert);
        _appendBytes(val.flags.rawData(), val.flags.size(), invert);
        _append(int8_t(0), invert);
    }

    void KeyString::_appendDBRef(const BSONDBRef& val, bool invert) {
        _append(endian::nativeToBig(int32_t(val.ns.size())), invert);
        _appendBytes(val.ns.rawData(), val.ns.size(), invert);
        _appendBytes(val.oid.view().view(), OID::kOIDSize, invert);
    }

    void KeyString::_appendArray(const BSONArray& val, bool invert) {
        BSONForEach(elem, val) {
            _appendBsonValue(elem, invert, NULL);
        }
        _append(int8_t(0), invert);
    }

    void KeyString::_appendObject(const BSONObj& val, bool invert) {
        _appendBson(val, invert);
    }

    void KeyString::_appendDouble(const double num, bool invert) {
        // no special cases for Inf,
        // see http://en.wikipedia.org/wiki/IEEE_754-1985#Positive_and_negative_infinity
        if (isNaN(num)) {
            _append(kNaN, invert);
            return;
        }

        if (num == 0.0) {
            // We are collapsing -0.0 and 0.0 to the same value here.
            // This is correct as IEEE-754 specifies that they compare as equal,
            // however this prevents roundtripping -0.0.
            // So if you put a -0.0 in, you'll get 0.0 out.
            // We believe this to be ok.
            _append(kZero, invert);
            return;
        }

        // if negative invert, unless we are inverting positives
        if (num < 0.0)
            invert = !invert;

        const double magnitude = num < 0.0 ? -num : num;
        if (magnitude < kMinLargeInt64) {
            _appendSmallDouble(magnitude, invert);
            return;
        }

        if (magnitude < kMinLargeDouble) {
            _appendLargeInt64(static_cast<long long>(magnitude), invert);
            return;
        }

        _appendLargeDouble(magnitude, invert);
    }

    void KeyString::_appendLongLong(const long long num, bool invert) {
        if (num == 0) {
            _append(kZero, invert);
            return;
        }

        if (num < 0)
            invert = !invert;

        if (num == std::numeric_limits<long long>::min()) {
            // -2**63 is exactly representable as a double and not as a positive int64.
            // Therefore we encode it as a double.
            dassert(-double(num) == kMinLargeDouble);
            _appendLargeDouble(-double(num), invert);
            return;
        }

        const long long magnitude = num < 0 ? -num : num;
        if (magnitude < kMinLargeInt64) {
            _appendSmallDouble(double(magnitude), invert);
            return;
        }

        _appendLargeInt64(magnitude, invert);
    }

    void KeyString::_appendInt(const int num, bool invert) {
        if (num == 0) {
            _append(kZero, invert);
            return;
        }

        if (num < 0)
            invert = !invert;

        const double magnitude = num < 0 ? -double(num) : double(num);
        _appendSmallDouble(magnitude, invert);
    }

    void KeyString::_appendBsonValue(const BSONElement& elem,
                                     bool invert,
                                     const StringData* name) {

        _append(bsonTypeToKeyStringType(elem.type()), invert);

        if (name) {
            _appendBytes(name->rawData(), name->size() + 1, invert); // + 1 for NUL
        }

        switch (elem.type()) {
        case MinKey:
        case MaxKey:
        case EOO:
        case Undefined:
        case jstNULL:
            break;

        case NumberDouble: _appendDouble(elem._numberDouble(), invert); break;
        case String: _appendString(elem.valueStringData(), invert); break;
        case Object: _appendObject(elem.Obj(), invert); break;
        case Array: _appendArray(BSONArray(elem.Obj()), invert); break;
        case BinData: {
            int len;
            const char* data = elem.binData(len);
            _appendBinData(BSONBinData(data, len, elem.binDataType()), invert);
            break;
        }

        case jstOID: _appendOID(elem.__oid(), invert); break;
        case Bool: _appendBool(elem.boolean(), invert); break;
        case Date: _appendDate(elem.date(), invert); break;

        case RegEx: _appendRegex(BSONRegEx(elem.regex(), elem.regexFlags()), invert); break;
        case DBRef: _appendDBRef(BSONDBRef(elem.dbrefNS(), elem.dbrefOID()), invert); break;
        case Symbol: _appendSymbol(elem.valueStringData(), invert); break;
        case Code: _appendCode(elem.valueStringData(), invert); break;
        case CodeWScope: {
            _appendCodeWString(BSONCodeWScope(StringData(elem.codeWScopeCode(),
                                                         elem.codeWScopeCodeLen()-1),
                                              BSONObj(elem.codeWScopeScopeData())),
                               invert);
            break;
        }
        case NumberInt: _appendInt(elem._numberInt(), invert); break;
        case Timestamp: _appendTimestamp(elem._opTime(), invert); break;
        case NumberLong: _appendLongLong(elem._numberLong(), invert); break;

        default:
            invariant(false);
        }
    }


    /// -- lowest level

    void KeyString::_appendStringLike(StringData str, bool invert) {
        while (true) {
            size_t firstNul = strnlen(str.rawData(), str.size());
            // No NULs in string.
            _appendBytes(str.rawData(), firstNul, invert);
            if (firstNul == str.size() || firstNul == std::string::npos) {
                _append(int8_t(0), invert);
                break;
            }
            invariant(!invert); // TODO readInvertedCString not done yet for this

            // replace "\x00" with "\x00\xFF"
            _appendBytes("\x00\xFF", 2, invert);
            str = str.substr(firstNul + 1); // skip over the NUL byte
        }
    }

    void KeyString::_appendBson(const BSONObj& obj, bool invert) {
        BSONForEach(elem, obj) {
            StringData name = elem.fieldNameStringData();
            _appendBsonValue(elem, invert, &name);
        }
        _append(int8_t(0), invert);
    }

    void KeyString::_appendSmallDouble(double magnitude, bool invert) {
        _append(kSmallDouble, invert);
        uint64_t data;
        memcpy(&data, &magnitude, sizeof(data));
        _append(endian::nativeToBig(data), invert);
    }

    void KeyString::_appendLargeDouble(double magnitude, bool invert) {
        _append(uint8_t(kLargeDouble), invert);
        uint64_t data;
        memcpy(&data, &magnitude, sizeof(data));
        _append(endian::nativeToBig(data), invert);
    }

    void KeyString::_appendLargeInt64(long long magnitude, bool invert) {
        _append(uint8_t(kLargeInt64), invert);
        _append(endian::nativeToBig(magnitude), invert);
    }

    template <typename T>
    void KeyString::_append(const T& thing, bool invert) {
        _appendBytes(&thing, sizeof(thing), invert);
    }

    void KeyString::_appendBytes(const void* source, size_t bytes, bool invert) {
        invariant(_size + bytes < kMaxBufferSize);
        char* const base = _buffer + _size;
        _size += bytes;

        if (invert) {
            memcpy_flipBits(base, source, bytes);
        } else {
            memcpy(base, source, bytes);
        }
    }


    // ----------------------------------------------------------------------
    // -----------   TO BSON CODE -------------------------------------------
    // ----------------------------------------------------------------------

    namespace {
        void toBsonValue(uint8_t type,
                         BufReader* reader,
                         bool inverted,
                         BSONObjBuilderValueStream* stream);

        void toBson(BufReader* reader, bool inverted, BSONObjBuilder* builder) {
            uint8_t type;
            while ((type = readType<uint8_t>(reader, inverted)) != 0) {
                if (inverted) {
                    std::string name = readInvertedCString(reader);
                    BSONObjBuilderValueStream& stream = *builder << name;
                    toBsonValue(type, reader, inverted, &stream);
                }
                else {
                    StringData name = readCString(reader);
                    BSONObjBuilderValueStream& stream = *builder << name;
                    toBsonValue(type, reader, inverted, &stream);
                }
            }
        }

        void toBsonValue(uint8_t type,
                         BufReader* reader,
                         bool inverted,
                         BSONObjBuilderValueStream* stream) {
            switch (type) {
            case CType::kMinKey: *stream << MINKEY; break;
            case CType::kMaxKey: *stream << MAXKEY; break;
            case CType::kNullish: *stream << BSONNULL; break;
            case CType::kUndefined: *stream << BSONUndefined; break;

            case CType::kBool:
                *stream << bool(readType<uint8_t>(reader, inverted));
                break;

            case CType::kDate:
                *stream << Date_t(endian::bigToNative(readType<uint64_t>(reader,
                                                                         inverted)) ^ (1LL << 63));
                break;

            case CType::kTimestamp:
                *stream << OpTime(endian::bigToNative(readType<uint64_t>(reader, inverted)));
                break;

            case CType::kOID:
                if (inverted) {
                    char buf[OID::kOIDSize];
                    memcpy_flipBits(buf, reader->skip(OID::kOIDSize), OID::kOIDSize);
                    *stream << OID::from(buf);
                }
                else {
                    *stream << OID::from(reader->skip(OID::kOIDSize));
                }
                break;

            case CType::kString:
                if (inverted)
                    *stream << readInvertedCString(reader);
                else
                    *stream << readCString(reader);
                break;

            case CType::kCode: {
                if (inverted) {
                    *stream << BSONCode(readInvertedCString(reader));
                }
                else {
                    *stream << BSONCode(readCString(reader));
                }
                break;
            }

            case CType::kCodeWithScope: {
                string save;
                StringData code;
                if (inverted) {
                    save = readInvertedCString(reader);
                    code = save;
                }
                else {
                    code = readCString(reader);
                }
                // Not going to optimize CodeWScope.
                BSONObjBuilder scope;
                toBson(reader, inverted, &scope);
                *stream << BSONCodeWScope(code, scope.done());
                break;
            }

            case CType::kBinData: {
                size_t size = endian::bigToNative(readType<uint32_t>(reader, inverted));
                BinDataType type = BinDataType(readType<uint8_t>(reader, inverted));
                const void* ptr = reader->skip(size);
                if (!inverted) {
                    *stream << BSONBinData(ptr, size, type);
                }
                else {
                    boost::scoped_array<char> flipped(new char[size]);
                    memcpy_flipBits(flipped.get(), ptr, size);
                    *stream << BSONBinData(flipped.get(), size, type);
                }
                break;
            }

            case CType::kRegEx: {
                if (inverted) {
                    string pattern = readInvertedCString(reader);
                    string flags = readInvertedCString(reader);
                    *stream << BSONRegEx(pattern, flags);
                }
                else {
                    StringData pattern = readCString(reader);
                    StringData flags = readCString(reader);
                    *stream << BSONRegEx(pattern, flags);
                }
                break;
            }

            case CType::kDBRef: {
                size_t size = endian::bigToNative(readType<uint32_t>(reader, inverted));
                if (inverted) {
                    boost::scoped_array<char> ns(new char[size]);
                    memcpy_flipBits(ns.get(), reader->skip(size), size);
                    char oidBytes[OID::kOIDSize];
                    memcpy_flipBits(oidBytes, reader->skip(OID::kOIDSize), OID::kOIDSize);
                    OID oid = OID::from(oidBytes);
                    *stream << BSONDBRef(StringData(ns.get(), size), oid);
                }
                else {
                    const char* ns = static_cast<const char*>(reader->skip(size));
                    OID oid = OID::from(reader->skip(OID::kOIDSize));
                    *stream << BSONDBRef(StringData(ns, size), oid);
                }
                break;
            }

            case CType::kObject: {
                BSONObjBuilder subObj(stream->subobjStart());
                toBson(reader, inverted, &subObj);
                break;
            }

            case CType::kArray: {
                BSONObjBuilder subArr(stream->subarrayStart());
                int index = 0;
                uint8_t type;
                while ((type = readType<uint8_t>(reader, inverted)) != 0) {
                    toBsonValue(type,
                                reader,
                                inverted,
                                &(subArr << BSONObjBuilder::numStr(index++)));
                }
                break;
            }

            case CType::kNumeric: {
                // Using doubles for everything except kLargeInt64
                switch (uint8_t kind = readType<uint8_t>(reader, false)) {
                case kNaN: *stream << std::numeric_limits<double>::quiet_NaN(); break;
                case kZero: *stream << 0.0; break;
                case kLargeInt64: {
                    long long ll = endian::bigToNative(readType<long long>(reader, false));
                    if (inverted)
                        ll = -ll;
                    *stream << ll;
                    break;
                }
                case kSmallDouble:
                case kLargeDouble: {
                    uint64_t encoded = readType<uint64_t>(reader, false);
                    encoded = endian::bigToNative(encoded);
                    double d;
                    memcpy(&d, &encoded, sizeof(d));
                    if (inverted)
                        *stream << -d;
                    else
                        *stream << d;
                    break;
                }

                default: {
                    switch (uint8_t(~kind)) {
                    case kNaN: *stream << std::numeric_limits<double>::quiet_NaN(); break;
                    case kZero: *stream << 0.0; break;
                    case kLargeInt64: {
                        long long ll = ~endian::bigToNative(readType<long long>(reader, false));
                        if (!inverted)
                            ll = -ll;
                        *stream << ll;
                        break;
                    }

                    case kSmallDouble:
                    case kLargeDouble: {
                        uint64_t encoded;
                        memcpy_flipBits(&encoded, reader->pos(), sizeof(encoded));
                        reader->skip(sizeof(encoded));
                        encoded = endian::bigToNative(encoded);
                        double d;
                        memcpy(&d, &encoded, sizeof(d));
                        if(inverted)
                            *stream << d;
                        else
                            *stream << -d;
                        break;
                    }
                    default: invariant(false);
                    }
                    break;
                }
                }
                break;
            }
            default: invariant(false);
            }
        }
    } // namespace

    BSONObj KeyString::toBson(const char* buffer, size_t len, Ordering ord) {
        BSONObjBuilder builder;
        BufReader reader(buffer, len);
        for (int i = 0; reader.remaining(); i++) {
            const bool invert = (ord.get(i) == -1);
            uint8_t type = readType<uint8_t>(&reader, invert);
            if (type == kEnd)
                break;
            toBsonValue(type, &reader, invert, &(builder << ""));

            // discriminator
            // Note: this should probably affect the BSON key name, but it must be read
            // *after* the value so it isn't possible.
            reader.read<uint8_t>();
        }
        return builder.obj();
    }

    BSONObj KeyString::toBson(StringData data, Ordering ord) {
        return toBson(data.rawData(), data.size(), ord);
    }

    BSONObj KeyString::toBson(Ordering ord) const {
        return toBson(_buffer, _size, ord);
    }

    // ----------------------------------------------------------------------
    //  --------- MISC class utils --------
    // ----------------------------------------------------------------------

    std::string KeyString::toString() const {
        return toHex(getBuffer(), getSize());
    }

    int KeyString::compare(const KeyString& other) const {
        int a = getSize();
        int b = other.getSize();

        int min = std::min(a, b);

        int cmp = memcmp(_buffer, other._buffer, min);

        if (cmp) {
            if (cmp < 0)
                return -1;
            return 1;
        }

        // keys match

        if (a == b)
            return 0;

        return a < b ? -1 : 1;
    }

} // namespace mongo
