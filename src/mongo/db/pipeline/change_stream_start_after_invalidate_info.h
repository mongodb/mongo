/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"

#include <memory>

namespace mongo {

class BSONObjBuilder;

/**
 * Contains information to augment the 'ChangeStreamStartAfterInvalidation' error code. In
 * particular, this class captures the 'invalidate' event that contains the client-provided resume
 * token.
 */
class ChangeStreamStartAfterInvalidateInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ChangeStreamStartAfterInvalidate;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    explicit ChangeStreamStartAfterInvalidateInfo(BSONObj startAfterInvalidateEvent)
        : _startAfterInvalidateEvent{startAfterInvalidateEvent.getOwned()} {}

    BSONObj getStartAfterInvalidateEvent() const {
        return _startAfterInvalidateEvent;
    }

    void serialize(BSONObjBuilder* bob) const final;

private:
    BSONObj _startAfterInvalidateEvent;
};

}  // namespace mongo
