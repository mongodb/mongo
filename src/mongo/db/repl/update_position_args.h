// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <vector>

namespace mongo {

class Status;

namespace repl {

/**
 * Arguments to the update position command.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] UpdatePositionArgs {
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

}  // namespace repl
}  // namespace mongo
