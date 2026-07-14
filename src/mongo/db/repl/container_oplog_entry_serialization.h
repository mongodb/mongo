// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mongo::repl {

/**
 * Non-owning view of a container key. Either an int64_t or a span into the source BSONObj's
 * binData. The source BSONObj must outlive this object. For Array types, it is a container of
 * non-owning views.
 */
class [[MONGO_MOD_PUBLIC]] ContainerKey {
public:
    ContainerKey() : _key(int64_t{0}) {}
    explicit ContainerKey(std::vector<std::span<const char>> key) : _key(std::move(key)) {}
    explicit ContainerKey(int64_t key) : _key(key) {}
    explicit ContainerKey(std::span<const char> key) : _key(key) {}

    static ContainerKey parse(const BSONElement& elem);

    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const;

    bool isArrayKey() const {
        return std::holds_alternative<std::vector<std::span<const char>>>(_key);
    }

    bool isIntKey() const {
        return std::holds_alternative<int64_t>(_key);
    }

    bool isBytesKey() const {
        return std::holds_alternative<std::span<const char>>(_key);
    }

    std::vector<std::span<const char>> getArrayKey() const;

    int64_t getIntKey() const;

    std::span<const char> getBytesKey() const;

    /**
     * Dispatch helper that calls f with the appropriate key type.
     */
    template <typename F>
    auto visit(F&& f) const {
        return std::visit(std::forward<F>(f), _key);
    }

private:
    std::variant<std::vector<std::span<const char>>, int64_t, std::span<const char>> _key;
};

/**
 * Non-owning view of a container value (bindata) from a BSONObj. The source BSONObj must
 * outlive this object. For Array types, it is a container of non-owning views.
 */
class [[MONGO_MOD_PUBLIC]] ContainerVal {
public:
    ContainerVal() : _data(std::span<const char>{}) {}
    explicit ContainerVal(std::vector<std::span<const char>> values) : _data(std::move(values)) {}
    explicit ContainerVal(std::span<const char> data) : _data(data) {}

    static ContainerVal parse(const BSONElement& elem);

    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const;

    bool isBytesVal() const {
        return std::holds_alternative<std::span<const char>>(_data);
    }

    bool isArrayVal() const {
        return std::holds_alternative<std::vector<std::span<const char>>>(_data);
    }

    std::vector<std::span<const char>> getArrayVal() const;

    std::span<const char> data() const;

    /**
     * Dispatch helper that calls f with the appropriate value type.
     */
    template <typename F>
    auto visit(F&& f) const {
        return std::visit(std::forward<F>(f), _data);
    }

private:
    std::variant<std::vector<std::span<const char>>, std::span<const char>> _data;
};

}  // namespace mongo::repl
