/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <vector>

namespace mongo {

class Status;

namespace MONGO_MOD_PUB repl {

/**
 * Arguments to the update position command.
 */
class UpdatePositionArgs {
public:
    static const char kCommandFieldName[];
    static const char kUpdateArrayFieldName[];
    static const char kAppliedOpTimeFieldName[];
    static const char kAppliedWallTimeFieldName[];
    static const char kWrittenOpTimeFieldName[];
    static const char kWrittenWallTimeFieldName[];
    static const char kDurableOpTimeFieldName[];
    static const char kDurableWallTimeFieldName[];
    static const char kMemberIdFieldName[];
    static const char kConfigVersionFieldName[];

    struct UpdateInfo {
        UpdateInfo(const OpTime& applied,
                   const Date_t& appliedWall,
                   const OpTime& written,
                   const Date_t& writtenWall,
                   const OpTime& durable,
                   const Date_t& durableWall,
                   long long aCfgver,
                   long long aMemberId);

        OpTime appliedOpTime;
        Date_t appliedWallTime;
        OpTime writtenOpTime;
        Date_t writtenWallTime;
        OpTime durableOpTime;
        Date_t durableWallTime;
        long long cfgver;
        long long memberId;
    };

    typedef std::vector<UpdateInfo>::const_iterator UpdateIterator;

    /**
     * Initializes this UpdatePositionArgs from the contents of "argsObj".
     */
    Status initialize(const BSONObj& argsObj);

    /**
     * Gets a begin iterator over the UpdateInfos stored in this UpdatePositionArgs.
     */
    UpdateIterator updatesBegin() const {
        return _updates.begin();
    }

    /**
     * Gets an end iterator over the UpdateInfos stored in this UpdatePositionArgs.
     */
    UpdateIterator updatesEnd() const {
        return _updates.end();
    }

    /**
     * Returns a BSONified version of the object.
     * _updates is only included if it is not empty.
     */
    BSONObj toBSON() const;

private:
    std::vector<UpdateInfo> _updates;
};

}  // namespace MONGO_MOD_PUB repl
}  // namespace mongo
