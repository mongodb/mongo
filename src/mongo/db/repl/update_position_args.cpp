// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
