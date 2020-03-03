/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <iostream>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

/**
 * This class contains an integer used as a member's _id and _memberid.
 **/
class MemberId {
public:
    MemberId() : _id(kUninitializedMemberId) {}

    explicit MemberId(int id) {
        if (id < 0 || id > 255) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "_id field value of " << id << " is out of range.");
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

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
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
