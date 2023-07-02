/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstdint>
#include <cstring>  // strlen

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/time_support.h"

namespace mongo {
class BSONObj;
struct BSONArray;

/**
 * Helper class to interpret the 'value' component of BSONElement without requiring a full
 * BSONElement binary with type byte and field name.
 *
 * No type checking is performed on the access methods and it is the callers responsibility to
 * interpret the value as the correct type.
 */
class BSONElementValue {
public:
    BSONElementValue() = default;
    explicit BSONElementValue(const char* value) : _value(value) {}

    /**
     * Constants for various element offsets.
     */
    static constexpr int kCountBytes = 4;
    static constexpr int kBinDataSubTypeBytes = 1;
    static constexpr int kStringTerminatorBytes = 1;

    /**
     * Raw data of the element's value
     */
    const char* value() const {
        return _value;
    }

    /**
     * NumberDouble (0x01)
     */
    double Double() const {
        return ConstDataView(value()).read<LittleEndian<double>>();
    }

    /**
     * String (0x02)
     */
    StringData String() const {
        // String count includes null terminator.
        return StringData(
            _CString(), ConstDataView(value()).read<LittleEndian<int>>() - kStringTerminatorBytes);
    }

    /**
     * Object, Array (0x03, 0x04)
     */
    BSONObj Obj() const;
    BSONArray Array() const;

    /**
     * BinData (0x05)
     */
    BSONBinData BinData() const {
        uint8_t subtype = ConstDataView(value() + kCountBytes).read<LittleEndian<uint8_t>>();
        return {value() + kCountBytes + kBinDataSubTypeBytes,
                ConstDataView(value()).read<LittleEndian<int>>(),
                static_cast<BinDataType>(subtype)};
    }

    /**
     * ObjectId / jstOID (0x7)
     */
    OID ObjectID() const {
        return OID::from(value());
    }

    /**
     * Bool (0x08)
     */
    bool Boolean() const {
        return *value() ? true : false;
    }

    /**
     * Date (0x09)
     */
    Date_t Date() const {
        return Date_t::fromMillisSinceEpoch(ConstDataView(value()).read<LittleEndian<long long>>());
    }

    /**
     * Regex (0x0B)
     */
    BSONRegEx Regex() const {
        const char* pattern = RegexPattern();
        const char* flags = RegexFlags();
        return BSONRegEx(StringData(pattern, flags - pattern - kStringTerminatorBytes),
                         StringData(flags));
    }
    const char* RegexPattern() const {
        return value();
    }
    const char* RegexFlags() const {
        const char* p = RegexPattern();
        return p + strlen(p) + kStringTerminatorBytes;
    }

    /**
     * DBRef (0x0C)
     */
    BSONDBRef DBRef() const {
        StringData ns = String();
        return BSONDBRef(ns, mongo::OID::from(ns.end() + kStringTerminatorBytes));
    }
    const char* DBRefNS() const {
        return _CString();
    }
    OID DBRefObjectID() const {
        const char* start = value();
        start += kCountBytes + ConstDataView(start).read<LittleEndian<int>>();
        return mongo::OID::from(start);
    }

    /**
     * Code (0x0D)
     */
    BSONCode Code() const {
        return BSONCode(String());
    }

    /**
     * Symbol (0x0E)
     */
    BSONSymbol Symbol() const {
        return BSONSymbol(String());
    }

    /**
     * CodeWScope (0x0F)
     */
    BSONCodeWScope CodeWScope() const {
        StringData code = CodeWScopeCode();
        return BSONCodeWScope(code, _codeWScopeObj(code.size() + kStringTerminatorBytes));
    }
    const char* CodeWScopeCode() const {
        // two ints precede code, first for entire code_w_scope and the second for the string count
        // of the scope name (see BSON spec)
        return value() + kCountBytes + kCountBytes;
    }
    BSONObj CodeWScopeObj() const;

    /**
     * NumberInt (0x10)
     */
    int Int32() const {
        return ConstDataView(value()).read<LittleEndian<int>>();
    }

    /**
     * Timestamp / bsonTimestamp (0x11)
     */
    Timestamp timestamp() const {
        return Timestamp(ConstDataView(value()).read<LittleEndian<unsigned long long>>());
    }
    unsigned long long TimestampValue() const {
        return ConstDataView(value()).read<LittleEndian<unsigned long long>>();
    }

    /**
     * NumberLong (0x12)
     */
    long long Int64() const {
        return ConstDataView(value()).read<LittleEndian<long long>>();
    }

    /**
     * NumberDecimal (0x13)
     */
    Decimal128 Decimal() const {
        uint64_t low = ConstDataView(value()).read<LittleEndian<long long>>();
        uint64_t high = ConstDataView(value() + sizeof(long long)).read<LittleEndian<long long>>();
        return Decimal128(Decimal128::Value({low, high}));
    }

private:
    // BSON Strings are always null terminated so this is seemingly safe. But in case the user
    // embeds null characters we will only read a partial string with this interface.
    const char* _CString() const {
        return value() + kCountBytes;
    }

    BSONObj _codeWScopeObj(int codeSizeWithNull) const;

    const char* _value = nullptr;
};

}  // namespace mongo
