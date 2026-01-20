/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/convert_utils.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/overloaded_visitor.h"

#include <cstdint>
#include <stack>

#include <nlohmann/json.hpp>

namespace mongo::exec::expression::convert_utils {

namespace {
using json = nlohmann::json;

class BsonSaxConsumer : public json::json_sax_t {
public:
    BsonSaxConsumer(int maxSize) : _maxSize(maxSize) {}

    bool null() override {
        append(Value(BSONNULL));
        return true;
    }

    bool boolean(bool val) override {
        append(val);
        return true;
    }

    bool number_integer(number_integer_t val) override {
        if (std::numeric_limits<int>::min() <= val && val <= std::numeric_limits<int>::max()) {
            append(static_cast<int>(val));
        } else {
            append(static_cast<long long>(val));
        }
        return true;
    }

    bool number_unsigned(number_unsigned_t val) override {
        // BSON doesn't have unsigned integer type. Explicitly convert to double if val is too
        // large.
        if (val <= static_cast<number_unsigned_t>(std::numeric_limits<number_integer_t>::max())) {
            return number_integer(val);
        } else {
            return number_float(val, "");
        }
    }

    bool number_float(number_float_t val, const string_t&) override {
        append(val);
        return true;
    }

    bool string(string_t& val) override {
        append(std::move(val));
        return true;
    }

    bool start_object(std::size_t) override {
        _builders.emplace(std::make_unique<MutableDocument>());
        uassertDepthLimit();
        return true;
    }

    bool end_object() override {
        endArrayOrObj([](ObjBuilder& builder) { return builder->freezeToValue(); });
        return true;
    }

    bool start_array(std::size_t) override {
        _builders.push(ArrBuilder{});
        uassertDepthLimit();
        return true;
    }

    bool end_array() override {
        endArrayOrObj([](ArrBuilder& builder) { return Value(std::move(builder)); });
        return true;
    }

    bool key(string_t& val) override {
        // BSONBuilder performs a similar check, but we need to throw 'CoversionFailure' to make
        // sure the "onError" $convert argument gets invoked.
        uassertInvalidJson("Illegal embedded null byte", val.find('\0') == std::string::npos);
        _keys.push(std::move(val));
        return true;
    }

    bool binary(json::binary_t&) override {
        MONGO_UNREACHABLE_TASSERT(10508702);
    }

    bool parse_error(std::size_t position,
                     const std::string& last_token,
                     const json::exception& ex) override {
        uassertInvalidJson(""_sd, false);
        return false;
    }

    Value releaseResult() {
        tassert(10508701, "Not done yet", _done);
        uassertSizeLimit();
        return std::move(_result);
    }

private:
    // MutableDocument can't be copy or move constructed which prevents it from being able to be
    // placed inside a variant directly. Wrap it in a unique_ptr as a workaround.
    using ObjBuilder = std::unique_ptr<MutableDocument>;
    using ArrBuilder = std::vector<Value>;
    using Builder = std::variant<ObjBuilder, ArrBuilder>;

    template <typename... Fs>
    auto visitCurrentBuilder(Fs&&... fs) {
        return visit(OverloadedVisitor{std::forward<Fs>(fs)...}, _builders.top());
    }

    template <typename Val>
    void append(Val val) {
        uassertInvalidJson("Unexpected standalone value", !_builders.empty());

        Value docValue{std::move(val)};
        visitCurrentBuilder(
            [&](ObjBuilder& builder) {
                tassert(10508707, "Missing key", !_keys.empty());
                builder->setField(std::move(_keys.top()), std::move(docValue));
                _keys.pop();
            },
            [&](ArrBuilder& builder) { builder.push_back(std::move(docValue)); });
    }

    template <typename F>
    void endArrayOrObj(F&& releaseResult) {
        Value result =
            visitCurrentBuilder(std::forward<F>(releaseResult), [](auto& builder) -> Value {
                tasserted(10508705, "Unexpected type");
            });

        _builders.pop();

        if (_builders.empty()) {
            tassert(10508704, "Expected keys to be empty", _keys.empty());
            // If this is the last builder left, we're at the root.
            _result = Value(std::move(result));
            _done = true;
        } else {
            // Otherwise, append to parent.
            append(std::move(result));
        }
    }

    void uassertDepthLimit() const {
        static auto depthLimit = BSONDepth::getMaxAllowableDepth();
        uassertInvalidJson(str::stream() << "Result exceeds maximum depth limit of " << depthLimit
                                         << " levels of nesting",
                           _builders.size() <= depthLimit);
    }

    void uassertSizeLimit() const {
        if (_result.getApproximateSize() < static_cast<size_t>(_maxSize)) {
            return;
        }

        try {
            // Unfortunately there is no way to accurately check the size of the serialized value
            // without serializing it.
            //
            // We wouldn't have to this separately if we used the BSONBuilders instead of
            // MutableDocument/Value, but the desired behavior for duplicate keys (keep last value)
            // is not possible to implement efficiently with BSONBuilders. The JS/$function
            // equivalent also ends up doing an extra (de)serialization step between JS and
            // BSON objects, so this extra work shouldn't be catastrophically bad in comparison.
            Document::validateDocumentBSONSize(BSON_ARRAY(_result), _maxSize);
        } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>& ex) {
            uassertInvalidJson(ex.reason(), false);
        }
    }

    void uassertInvalidJson(StringData reason, bool cond) const {
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Input doesn't represent valid JSON"
                              << (reason.empty() ? "" : ": ") << reason,
                cond);
    }

    const int _maxSize;

    Value _result;
    bool _done{false};
    std::stack<std::string> _keys;
    std::stack<Builder> _builders;
};
}  // namespace

Value parseJson(StringData data, boost::optional<BSONType> expectedType) {
    // The sax consumer handles errors by uasserting. It should never return false.
    BsonSaxConsumer sax{BSONObjMaxUserSize};
    tassert(10508706, "Unexpected parsing error", json::sax_parse(data, &sax));

    Value result = sax.releaseResult();
    if (expectedType) {
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "Input doesn't match expected type '" << typeName(*expectedType)
                              << "'",
                result.getType() == *expectedType);
    }

    return result;
}

std::vector<Value> convertBinDataVectorToArray(const Value& val, bool isLittleEndian) {
    auto binData = val.getBinData();
    tassert(11300101, "Expected binData with vector subtype", binData.type == BinDataType::Vector);

    if (binData.length == 0) {
        // If bindata is empty, we return an empty array.
        return {};
    }

    // If the binData isn't empty, it must contain a dType and a padding byte.
    uassert(10506601, "binData length invalid", binData.length >= 2);
    const std::byte* dataPointer = static_cast<const std::byte*>(binData.data);

    // First byte is the type.
    auto dTypeCur = [&]() {
        if (dataPointer[0] == kPackedBitDataTypeByte)
            return dType::PACKED_BIT;
        if (dataPointer[0] == kInt8DataTypeByte)
            return dType::INT8;
        if (dataPointer[0] == kFloat32DataTypeByte)
            return dType::FLOAT32;
        uasserted(10506600,
                  "Invalid dType for provided BinData vector. Valid options are 0x10 for "
                  "PACKED_BIT, 0x03 for INT8, or 0x27 for FLOAT32.");
    }();

    // Second byte is the padding.
    uint8_t paddingByte;
    std::memcpy(&paddingByte, &(dataPointer[1]), sizeof(paddingByte));
    int padding = static_cast<int>(paddingByte);
    uassert(10506606,
            "Padding must be between 0 and 7 for PACKED_BIT vectors, or 0 otherwise",
            padding == 0 || (dTypeCur == dType::PACKED_BIT && padding <= 7));

    // The rest of the binData vector is the elements.
    std::vector<Value> results;
    int i = 2;
    while (i < binData.length) {
        uassert(10506602,
                "BinData vector of type FLOAT32 was malformed - expected to have at least four "
                "bytes left to read a FLOAT32 array element.",
                (dTypeCur != dType::FLOAT32) ||
                    (i <= (binData.length - static_cast<int>(sizeof(float)))));

        // Padding only applies if this is the last element and we're in PACKED_BIT.
        int thisPadding = 0;
        if (dTypeCur == dType::PACKED_BIT && i == binData.length - 1) {
            thisPadding = padding;  // Number of least-significant bits that should be discarded at
                                    // the end of the byte.
        }

        switch (dTypeCur) {
            case dType::PACKED_BIT: {
                std::byte thisByte = dataPointer[i];
                std::byte comparisonByte{0b10000000};
                // Remove the extra bits.
                thisByte = thisByte >> thisPadding;
                comparisonByte = comparisonByte >> thisPadding;
                for (int numDone = 0; numDone < 8 - thisPadding; ++numDone) {
                    results.push_back(Value((thisByte & comparisonByte) != std::byte{0}));
                    // Move the comparison byte to check the next bit. The values left of the
                    // comparison will be zero-d out.
                    comparisonByte = comparisonByte >> 1;
                }
                ++i;
                break;
            }
            case dType::INT8: {
                std::int8_t convertedVal;
                std::memcpy(&convertedVal, &(dataPointer[i]), sizeof(convertedVal));
                results.push_back(Value(static_cast<int>(convertedVal)));
                i += sizeof(convertedVal);
                break;
            }
            case dType::FLOAT32: {
                ConstDataView dataView(reinterpret_cast<const char*>(&dataPointer[i]));
                float convertedVal = isLittleEndian ? dataView.read<LittleEndian<float>>()
                                                    : dataView.read<BigEndian<float>>();
                results.push_back(Value(static_cast<double>(convertedVal)));
                i += sizeof(convertedVal);
                break;
            }
        }
    }
    return results;
}

}  // namespace mongo::exec::expression::convert_utils
