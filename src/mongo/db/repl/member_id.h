// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <iostream>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * This class contains an integer used as a member's _id and _memberid.
 **/
class MemberId {
public:
    MemberId() : _id(kUninitializedMemberId) {}

    explicit MemberId(int id) {
        if (id < 0) {
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << "_id field value of " << id << " can't be a negative number");
        }
        _id = id;
    }

    static MemberId parseFromBSON(const BSONElement& element) {
        if (!element.isNumber()) {
            uasserted(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "Element for MemberId was not a number: " << element.toString());
        }
        return MemberId(element.numberInt());
    }

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
        builder->appendNumber(fieldName, _id);
    }

    int getData() const {
        invariant(*this);
        return _id;
    }

    explicit operator bool() const {
        return _id != kUninitializedMemberId;
    }

    std::string toString() const {
        return str::stream() << "MemberId(" << (*this ? std::to_string(_id) : "") << ")";
    }

    friend bool operator==(MemberId lhs, MemberId rhs) {
        return lhs._id == rhs._id;
    }
    friend bool operator!=(MemberId lhs, MemberId rhs) {
        return lhs._id != rhs._id;
    }
    friend bool operator<(MemberId lhs, MemberId rhs) {
        return lhs._id < rhs._id;
    }
    friend bool operator<=(MemberId lhs, MemberId rhs) {
        return lhs._id <= rhs._id;
    }
    friend bool operator>(MemberId lhs, MemberId rhs) {
        return lhs._id > rhs._id;
    }
    friend bool operator>=(MemberId lhs, MemberId rhs) {
        return lhs._id >= rhs._id;
    }

    friend StringBuilder& operator<<(StringBuilder& stream, const MemberId& id) {
        return stream << id.toString();
    }

    friend std::ostream& operator<<(std::ostream& stream, const MemberId& id) {
        return stream << id.toString();
    }

private:
    // The default value for an unset memberId.
    static const int kUninitializedMemberId = -1;
    int _id;
};

}  // namespace repl
}  // namespace mongo
