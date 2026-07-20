/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


std::vector<std::span<const char>> ContainerKey::getArrayKey() const {
    return assertedGet<std::vector<std::span<const char>>>(_key);
}

int64_t ContainerKey::getIntKey() const {
    return assertedGet<int64_t>(_key);
}

std::span<const char> ContainerKey::getBytesKey() const {
    return assertedGet<std::span<const char>>(_key);
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


std::vector<std::span<const char>> ContainerVal::getArrayVal() const {
    return assertedGet<std::vector<std::span<const char>>>(_data);
}

std::span<const char> ContainerVal::data() const {
    return assertedGet<std::span<const char>>(_data);
}

}  // namespace mongo::repl
