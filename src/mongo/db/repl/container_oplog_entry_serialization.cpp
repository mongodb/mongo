// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/container_oplog_entry_serialization.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/demangle.h"
#include "mongo/util/overloaded_visitor.h"

#include <algorithm>
#include <iterator>

namespace mongo::repl {
namespace {

std::span<const char> getBinDataSpan(const BSONElement& elem) {
    int len = 0;
    const char* data = elem.binData(len);
    invariant(len >= 0);
    return {data, static_cast<size_t>(len)};
}

// Unpack a BSONObject or BSONArray of BinData StringKeys
std::vector<std::span<const char>> unpackBinDataValues(const auto& arr) {
    std::vector<std::span<const char>> keys;
    keys.reserve(arr.nFields());  // Pre-allocate based on number of array elements
    std::transform(arr.begin(), arr.end(), std::back_inserter(keys), [](const BSONElement& val) {
        uassert(ErrorCodes::TypeMismatch,
                "Container array must be BinData Generic",
                val.isBinData(BinDataType::BinDataGeneral));
        return getBinDataSpan(val);
    });
    return keys;
}

void appendBinDataArray(std::string_view fieldName,
                        std::span<const std::span<const char>> elems,
                        BSONObjBuilder* builder) {
    BSONArrayBuilder bab(builder->subarrayStart(fieldName));
    for (const auto& elem : elems) {
        bab.appendBinData(elem.size(), BinDataType::BinDataGeneral, elem.data());
    }
}

template <typename T>
const T& assertedGet(auto&& gettable) {
    const auto* gotten = std::get_if<T>(&gettable);
    massert(13064103,
            std::format("Container variant did not hold expected alternative {}",
                        demangleName(typeid(T))),
            gotten != nullptr);
    return *gotten;
}

}  // namespace

ContainerKey ContainerKey::parse(const BSONElement& elem) {
    switch (elem.type()) {
        case BSONType::array:
            // Pass as an embedded object to avoid reasserting the type and heap allocation from
            // Array()
            return ContainerKey(unpackBinDataValues(elem.embeddedObject()));
        case BSONType::numberLong:
            return ContainerKey(elem.Long());
        case BSONType::binData: {
            return ContainerKey(getBinDataSpan(elem));
        }
        default:
            uasserted(12270900,
                      str::stream()
                          << "Expected container key to be Array or NumberLong or BinData, but got "
                          << typeName(elem.type()));
    }
}

bool ContainerKey::isPacked(const BSONElement& elem) {
    // Only an array holds more than one key; NumberLong and BinData are both single keys.
    return elem.type() == BSONType::array;
}

void ContainerKey::serialize(std::string_view fieldName, BSONObjBuilder* builder) const {
    std::visit(OverloadedVisitor{
                   [&](const std::vector<std::span<const char>>& keys) {
                       appendBinDataArray(fieldName, keys, builder);
                   },
                   [&](int64_t key) { builder->append(fieldName, key); },
                   [&](std::span<const char> key) {
                       builder->appendBinData(
                           fieldName, key.size(), BinDataType::BinDataGeneral, key.data());
                   },
               },
               _key);
}


const std::vector<std::span<const char>>& ContainerKey::getArrayKey() const {
    return assertedGet<std::vector<std::span<const char>>>(_key);
}

int64_t ContainerKey::getIntKey() const {
    return assertedGet<int64_t>(_key);
}

std::span<const char> ContainerKey::getBytesKey() const {
    return assertedGet<std::span<const char>>(_key);
}

size_t ContainerVal::count() const {
    // Provides a count of values wrapped, can be used to determine if any value is needed when
    // serializing into an oplog object.
    OverloadedVisitor visitor(
        [](std::span<const std::span<const char>> data) -> size_t { return data.size(); },
        [](std::span<const char> data) -> size_t { return data.size() > 0 ? 1ULL : 0ULL; });

    return std::visit(visitor, _data);
}

ContainerVal ContainerVal::parse(const BSONElement& elem) {
    switch (elem.type()) {
        case BSONType::array:
            // Pass as an embedded object to avoid reasserting the type and heap allocation from
            // Array()
            return ContainerVal(unpackBinDataValues(elem.embeddedObject()));
        case BSONType::binData:
            return ContainerVal(getBinDataSpan(elem));
        default:
            uasserted(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected container value to be BinData or Array, but got "
                                    << typeName(elem.type()));
    }
}

bool ContainerVal::isPacked(const BSONElement& elem) {
    // Only an array holds more than one value; a lone BinData is a single value.
    return elem.type() == BSONType::array;
}

void ContainerVal::serialize(std::string_view fieldName, BSONObjBuilder* builder) const {
    std::visit(OverloadedVisitor{
                   [&](const std::vector<std::span<const char>>& values) {
                       appendBinDataArray(fieldName, values, builder);
                   },
                   [&](std::span<const char> value) {
                       builder->appendBinData(
                           fieldName, value.size(), BinDataType::BinDataGeneral, value.data());
                   },
               },
               _data);
}


const std::vector<std::span<const char>>& ContainerVal::getArrayVal() const {
    return assertedGet<std::vector<std::span<const char>>>(_data);
}

std::span<const char> ContainerVal::data() const {
    return assertedGet<std::span<const char>>(_data);
}

}  // namespace mongo::repl
