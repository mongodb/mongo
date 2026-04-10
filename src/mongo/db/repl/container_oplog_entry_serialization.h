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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

#include <cstdint>
#include <span>
#include <variant>

namespace mongo::repl {

/**
 * Non-owning view of a container key. Either an int64_t or a span into the source BSONObj's
 * binData. The source BSONObj must outlive this object.
 */
class ContainerKey {
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

    void serialize(StringData fieldName, BSONObjBuilder* builder) const {
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
class ContainerVal {
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

    void serialize(StringData fieldName, BSONObjBuilder* builder) const {
        builder->appendBinData(fieldName, _data.size(), BinDataType::BinDataGeneral, _data.data());
    }

    std::span<const char> data() const {
        return _data;
    }

private:
    std::span<const char> _data;
};

}  // namespace mongo::repl
