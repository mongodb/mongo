// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <iosfwd>
#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * Basic structure that contains a value and an opTime.
 */
template <typename T>
struct OpTimeWith {
public:
    OpTimeWith() = default;
    explicit OpTimeWith(T val) : value(std::move(val)) {}
    OpTimeWith(T val, repl::OpTime ts) : value(std::move(val)), opTime(std::move(ts)) {}

    std::string toString() const;

    bool operator==(const OpTimeWith<T>& rhs) const {
        return opTime == rhs.opTime && value == rhs.value;
    }

    T value;
    repl::OpTime opTime;
};

template <typename T>
std::ostream& operator<<(std::ostream& out, const OpTimeWith<T>& opTimeWith) {
    return out << opTimeWith.toString();
}

template <typename T>
std::string OpTimeWith<T>::toString() const {
    return str::stream() << opTime.toString() << "[" << value << "]";
}

}  // namespace repl
}  // namespace mongo
