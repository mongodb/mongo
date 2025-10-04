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

#include "mongo/db/repl/update_position_args.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/bson_extract_optime.h"

namespace mongo {
namespace repl {

const char UpdatePositionArgs::kCommandFieldName[] = "replSetUpdatePosition";
const char UpdatePositionArgs::kUpdateArrayFieldName[] = "optimes";
const char UpdatePositionArgs::kAppliedOpTimeFieldName[] = "appliedOpTime";
const char UpdatePositionArgs::kAppliedWallTimeFieldName[] = "appliedWallTime";
const char UpdatePositionArgs::kWrittenOpTimeFieldName[] = "writtenOpTime";
const char UpdatePositionArgs::kWrittenWallTimeFieldName[] = "writtenWallTime";
const char UpdatePositionArgs::kDurableOpTimeFieldName[] = "durableOpTime";
const char UpdatePositionArgs::kDurableWallTimeFieldName[] = "durableWallTime";
const char UpdatePositionArgs::kMemberIdFieldName[] = "memberId";
const char UpdatePositionArgs::kConfigVersionFieldName[] = "cfgver";

UpdatePositionArgs::UpdateInfo::UpdateInfo(const OpTime& applied,
                                           const Date_t& appliedWall,
                                           const OpTime& written,
                                           const Date_t& writtenWall,
                                           const OpTime& durable,
                                           const Date_t& durableWall,
                                           long long aCfgver,
                                           long long aMemberId)
    : appliedOpTime(applied),
      appliedWallTime(appliedWall),
      writtenOpTime(written),
      writtenWallTime(writtenWall),
      durableOpTime(durable),
      durableWallTime(durableWall),
      cfgver(aCfgver),
      memberId(aMemberId) {}

Status UpdatePositionArgs::initialize(const BSONObj& argsObj) {
    // grab the array of changes
    BSONElement updateArray;
    Status status =
        bsonExtractTypedField(argsObj, kUpdateArrayFieldName, BSONType::array, &updateArray);
    if (!status.isOK())
        return status;

    // now parse each array entry into an update
    BSONObjIterator i(updateArray.Obj());
    while (i.more()) {
        BSONObj entry = i.next().Obj();

        OpTime appliedOpTime;
        status = bsonExtractOpTimeField(entry, kAppliedOpTimeFieldName, &appliedOpTime);
        if (!status.isOK()) {
            return status;
        }
        Date_t appliedWallTime = Date_t();
        BSONElement appliedWallTimeElement;
        status = bsonExtractTypedField(
            entry, kAppliedWallTimeFieldName, BSONType::date, &appliedWallTimeElement);
        if (!status.isOK()) {
            return status;
        }
        appliedWallTime = appliedWallTimeElement.Date();

        OpTime writtenOpTime;
        status = bsonExtractOpTimeField(entry, kWrittenOpTimeFieldName, &writtenOpTime);
        if (status.code() == ErrorCodes::NoSuchKey) {
            writtenOpTime = appliedOpTime;
        } else if (!status.isOK()) {
            return status;
        }

        Date_t writtenWallTime = Date_t();
        BSONElement writtenWallTimeElement;
        status = bsonExtractTypedField(
            entry, kWrittenWallTimeFieldName, BSONType::date, &writtenWallTimeElement);
        if (status.code() == ErrorCodes::NoSuchKey) {
            writtenWallTime = appliedWallTime;
        } else if (!status.isOK()) {
            return status;
        } else {
            writtenWallTime = writtenWallTimeElement.Date();
        }

        Date_t durableWallTime = Date_t();
        BSONElement durableWallTimeElement;
        status = bsonExtractTypedField(
            entry, kDurableWallTimeFieldName, BSONType::date, &durableWallTimeElement);
        if (!status.isOK()) {
            return status;
        }
        durableWallTime = durableWallTimeElement.Date();

        OpTime durableOpTime;
        status = bsonExtractOpTimeField(entry, kDurableOpTimeFieldName, &durableOpTime);
        if (!status.isOK())
            return status;

        // TODO(spencer): The following three fields are optional in 3.0, but should be made
        // required or ignored in 3.0
        long long cfgver;
        status = bsonExtractIntegerFieldWithDefault(entry, kConfigVersionFieldName, -1, &cfgver);
        if (!status.isOK())
            return status;

        long long memberID;
        status = bsonExtractIntegerFieldWithDefault(entry, kMemberIdFieldName, -1, &memberID);
        if (!status.isOK())
            return status;

        _updates.push_back(UpdateInfo(appliedOpTime,
                                      appliedWallTime,
                                      writtenOpTime,
                                      writtenWallTime,
                                      durableOpTime,
                                      durableWallTime,
                                      cfgver,
                                      memberID));
    }

    return Status::OK();
}

BSONObj UpdatePositionArgs::toBSON() const {
    BSONObjBuilder builder;
    // add command name
    builder.append(kCommandFieldName, 1);

    // build array of updates
    if (!_updates.empty()) {
        BSONArrayBuilder updateArray(builder.subarrayStart(kUpdateArrayFieldName));
        for (UpdatePositionArgs::UpdateIterator update = updatesBegin(); update != updatesEnd();
             ++update) {
            BSONObjBuilder updateEntry(updateArray.subobjStart());
            updateEntry.append(kConfigVersionFieldName, update->cfgver);
            updateEntry.append(kMemberIdFieldName, update->memberId);
            update->appliedOpTime.append(kAppliedOpTimeFieldName, &updateEntry);
            update->writtenOpTime.append(kWrittenOpTimeFieldName, &updateEntry);
            update->durableOpTime.append(kDurableOpTimeFieldName, &updateEntry);
        }
        updateArray.doneFast();
    }
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo
