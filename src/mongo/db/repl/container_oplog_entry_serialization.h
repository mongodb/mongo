// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace mongo::repl {

/**
 * Non-owning view of a container key. Either an int64_t or a span into the source BSONObj's
 * binData. The source BSONObj must outlive this object.
 */
class [[MONGO_MOD_PUBLIC]] ContainerKey {
public:
    ContainerKey() : _key(int64_t{0}) {}
    explicit ContainerKey(int64_t key) : _key(key) {}
    explicit ContainerKey(std::span<const char> key) : _key(key) {}

    static ContainerKey parse(const BSONElement& elem) {
        switch (elem.type()) {
            case BSONType::numberLong:
                return ContainerKey(elem.Long());
            case BSONType::binData: {
                int len = 0;
                const char* data = elem.binData(len);
                return ContainerKey(std::span<const char>{data, static_cast<size_t>(len)});
            }
            default:
                uasserted(12270900,
                          str::stream() << "Expected container key to be NumberLong or BinData, "
                                           "but got "
                                        << typeName(elem.type()));
        }
    }

    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const {
        std::visit(OverloadedVisitor{
                       [&](int64_t key) { builder->append(fieldName, key); },
                       [&](std::span<const char> key) {
                           builder->appendBinData(
                               fieldName, key.size(), BinDataType::BinDataGeneral, key.data());
                       },
                   },
                   _key);
    }

    bool isIntKey() const {
        return std::holds_alternative<int64_t>(_key);
    }

    int64_t getIntKey() const {
        return std::get<int64_t>(_key);
    }

    std::span<const char> getBytesKey() const {
        return std::get<std::span<const char>>(_key);
    }

    /**
     * Dispatch helper that calls f with the appropriate key type.
     */
    template <typename F>
    auto visit(F&& f) const {
        return std::visit(std::forward<F>(f), _key);
    }


private:
    std::variant<int64_t, std::span<const char>> _key;
};

/**
 * Non-owning view of a container value (bindata) from a BSONObj. The source BSONObj must outlive
 * this object.
 */
class [[MONGO_MOD_PUBLIC]] ContainerVal {
public:
    ContainerVal() : _data() {}
    explicit ContainerVal(std::span<const char> data) : _data(data) {}

    static ContainerVal parse(const BSONElement& elem) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Expected BinData, but got " << typeName(elem.type()),
                elem.type() == BSONType::binData);
        int len = 0;
        const char* data = elem.binData(len);
        return ContainerVal(std::span<const char>{data, static_cast<size_t>(len)});
    }

    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const {
        builder->appendBinData(fieldName, _data.size(), BinDataType::BinDataGeneral, _data.data());
    }

    std::span<const char> data() const {
        return _data;
    }

private:
    std::span<const char> _data;
};

}  // namespace mongo::repl
