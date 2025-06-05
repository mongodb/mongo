/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/crypto/libbsonvalue.h"

#include "mongo/crypto/mongocryptbuffer.h"
#include "mongo/rpc/object_check.h"

#include <bson/bson.h>
#include <fmt/format.h>

extern "C" {
#include <mongocrypt-buffer-private.h>
}


namespace mongo {

LibBSONValue::LibBSONValue() : _value(new bson_value_t) {
    _value->value_type = BSON_TYPE_EOD;
}

LibBSONValue::LibBSONValue(const BSONElement& elem) : LibBSONValue() {
    switch (elem.type()) {
        case BSONType::eoo:
            // Do nothing. _value is already EOO.
            break;
        case BSONType::numberDouble:
            _value->value.v_double = elem.Double();
            _value->value_type = BSON_TYPE_DOUBLE;
            break;
        case BSONType::string: {
            auto str = elem.valueStringData();
            _value->value.v_utf8.str = reinterpret_cast<char*>(bson_malloc(str.size() + 1));
            _value->value.v_utf8.len = str.size();
            std::memcpy(_value->value.v_utf8.str, str.data(), str.size());
            _value->value.v_utf8.str[str.size()] = 0;
            _value->value_type = BSON_TYPE_UTF8;
            break;
        }
        case BSONType::object:
        case BSONType::array: {
            auto obj = elem.Obj();
            _value->value.v_doc.data = reinterpret_cast<std::uint8_t*>(bson_malloc(obj.objsize()));
            std::memcpy(_value->value.v_doc.data, obj.objdata(), obj.objsize());
            _value->value.v_doc.data_len = obj.objsize();
            _value->value_type =
                (elem.type() == BSONType::object) ? BSON_TYPE_DOCUMENT : BSON_TYPE_ARRAY;
            break;
        }
        case BSONType::binData: {
            int len = 0;
            const auto* data = reinterpret_cast<const std::uint8_t*>(elem.binData(len));
            _value->value.v_binary.subtype = static_cast<bson_subtype_t>(elem.binDataType());
            _value->value.v_binary.data = reinterpret_cast<std::uint8_t*>(bson_malloc(len));
            std::memcpy(_value->value.v_binary.data, data, len);
            _value->value.v_binary.data_len = len;
            _value->value_type = BSON_TYPE_BINARY;
            break;
        }
        case BSONType::oid: {
            auto oid = elem.OID();
            bson_oid_init_from_string(&(_value->value.v_oid), oid.toString().c_str());
            _value->value_type = BSON_TYPE_OID;
            break;
        }
        case BSONType::boolean:
            _value->value.v_bool = elem.Bool();
            _value->value_type = BSON_TYPE_BOOL;
            break;
        case BSONType::date:
            _value->value.v_datetime = elem.Date().toMillisSinceEpoch();
            _value->value_type = BSON_TYPE_DATE_TIME;
            break;
        case BSONType::null:
            _value->value_type = BSON_TYPE_NULL;
            break;
        case BSONType::regEx:
            _value->value.v_regex.regex = bson_strdup(elem.regex());
            _value->value.v_regex.options = bson_strdup(elem.regexFlags());
            _value->value_type = BSON_TYPE_REGEX;
            break;
        case BSONType::code: {
            auto str = elem._asCode();
            _value->value.v_code.code = reinterpret_cast<char*>(bson_malloc(str.size() + 1));
            std::memcpy(_value->value.v_code.code, str.c_str(), str.size());
            _value->value.v_code.code[str.size()] = 0;
            _value->value.v_code.code_len = str.size();
            _value->value_type = BSON_TYPE_CODE;
            break;
        }
        case BSONType::numberInt:
            _value->value.v_int32 = elem.Int();
            _value->value_type = BSON_TYPE_INT32;
            break;
        case BSONType::timestamp: {
            auto timestamp = elem.timestamp();
            _value->value.v_timestamp.timestamp = timestamp.getSecs();
            _value->value.v_timestamp.increment = timestamp.getInc();
            _value->value_type = BSON_TYPE_TIMESTAMP;
            break;
        }
        case BSONType::numberLong:
            _value->value.v_int64 = elem.Long();
            _value->value_type = BSON_TYPE_INT64;
            break;
        case BSONType::numberDecimal: {
            auto dec128 = elem.Decimal().getValue();
            _value->value.v_decimal128.high = dec128.high64;
            _value->value.v_decimal128.low = dec128.low64;
            _value->value_type = BSON_TYPE_DECIMAL128;
            break;
        }
        default:
            uasserted(ErrorCodes::BadValue, fmt::format("Unknown BSON value type {}", elem.type()));
    }
}

LibBSONValue::LibBSONValue(const bson_value_t& value) : LibBSONValue() {
    bson_value_copy(&value, _value.get());
}

LibBSONValue::~LibBSONValue() {
    bson_value_destroy(_value.get());
}

LibBSONValue& LibBSONValue::operator=(const LibBSONValue& src) {
    bson_value_destroy(_value.get());
    _value->value_type = BSON_TYPE_EOD;
    bson_value_copy(src._value.get(), _value.get());
    return *this;
}

LibBSONValue& LibBSONValue::operator=(LibBSONValue&& src) {
    if (this != &src) {
        bson_value_destroy(_value.get());

        _value = std::move(src._value);

        src._value = std::make_unique<bson_value_t>();
        src._value->value_type = BSON_TYPE_EOD;
    }

    return *this;
}

LibBSONValue::LibBSONValue(BSONType bsonType, ConstDataRange cdr) : LibBSONValue() {
    auto buffer = MongoCryptBuffer::borrow(cdr);
    uassert(ErrorCodes::OperationFailed,
            "Failed parsing BSON value",
            _mongocrypt_buffer_to_bson_value(
                buffer.get(), static_cast<uint8_t>(bsonType), _value.get()));
}

namespace {
// Generalize serializer between BSONObjBuilder and BSONArrayBuilder.
template <typename BinDataFunc, typename RegexFunc, typename NullFunc, typename AppendFunc>
void doSerialize(const bson_value_t& value,
                 BinDataFunc appendBinData,
                 RegexFunc appendRegex,
                 NullFunc appendNull,
                 AppendFunc append) {
    const auto& v = value.value;
    switch (value.value_type) {
        case BSON_TYPE_EOD:
            // Append nothing.
            break;
        case BSON_TYPE_DOUBLE:
            append(v.v_double);
            break;
        case BSON_TYPE_UTF8:
            append(StringData(v.v_utf8.str, v.v_utf8.len));
            break;
        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY: {
            auto obj =
                ConstDataRange(v.v_doc.data, v.v_doc.data_len).read<Validated<BSONObj>>().val;
            if (value.value_type == BSON_TYPE_DOCUMENT) {
                append(obj);
            } else {
                append(BSONArray(obj));
            }
            break;
        }
        case BSON_TYPE_BINARY:
            appendBinData(static_cast<BinDataType>(v.v_binary.subtype),
                          ConstDataRange(v.v_binary.data, v.v_binary.data_len));
            break;
        case BSON_TYPE_OID:
            append(OID(v.v_oid.bytes));
            break;
        case BSON_TYPE_BOOL:
            append(v.v_bool);
            break;
        case BSON_TYPE_DATE_TIME:
            append(Date_t::fromMillisSinceEpoch(v.v_datetime));
            break;
        case BSON_TYPE_NULL:
            appendNull();
            break;
        case BSON_TYPE_REGEX:
            appendRegex(StringData(v.v_regex.regex), StringData(v.v_regex.options));
            break;
        case BSON_TYPE_CODE:
            append(BSONCode(std::string(v.v_code.code)));
            break;
        case BSON_TYPE_INT32:
            append(v.v_int32);
            break;
        case BSON_TYPE_TIMESTAMP:
            append(Timestamp(Seconds{v.v_timestamp.timestamp}, v.v_timestamp.increment));
            break;
        case BSON_TYPE_INT64:
            append(v.v_int64);
            break;
        case BSON_TYPE_DECIMAL128:
            append(Decimal128(
                Decimal128::Value{.low64 = v.v_decimal128.low, .high64 = v.v_decimal128.high}));
            break;
        default:
            uasserted(ErrorCodes::BadValue,
                      fmt::format("Unknown BSON value type {}", fmt::underlying(value.value_type)));
    }
}
}  // namespace

void LibBSONValue::serialize(StringData fieldName, BSONObjBuilder* builder) const {
    const auto& appendBinData = [&](BinDataType binType, ConstDataRange binData) {
        builder->appendBinData(fieldName, binData.length(), binType, binData.data());
    };
    const auto& appendRegex = [&](StringData regex, StringData options) {
        builder->appendRegex(fieldName, regex, options);
    };
    const auto& appendNull = [&]() {
        builder->appendNull(fieldName);
    };
    const auto& append = [&](const auto& val) {
        builder->append(fieldName, val);
    };
    doSerialize(*_value, appendBinData, appendRegex, appendNull, append);
}


void LibBSONValue::serialize(BSONArrayBuilder* builder) const {
    const auto& appendBinData = [&](BinDataType binType, ConstDataRange binData) {
        builder->appendBinData(binData.length(), binType, binData.data());
    };
    const auto& appendRegex = [&](StringData regex, StringData options) {
        builder->appendRegex(regex, options);
    };
    const auto& appendNull = [&]() {
        builder->appendNull();
    };
    const auto& append = [&](const auto& val) {
        builder->append(val);
    };
    doSerialize(*_value, appendBinData, appendRegex, appendNull, append);
}

BSONObj LibBSONValue::getObject() const {
    BSONObjBuilder ret;
    serialize(""_sd, &ret);
    return ret.obj();
}

}  // namespace mongo
