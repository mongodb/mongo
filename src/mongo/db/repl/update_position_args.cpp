/**
 *    Copyright 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/update_position_args.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"

namespace mongo {
namespace repl {

const char UpdatePositionArgs::kCommandFieldName[] = "replSetUpdatePosition";
const char UpdatePositionArgs::kUpdateArrayFieldName[] = "optimes";
const char UpdatePositionArgs::kAppliedOpTimeFieldName[] = "appliedOpTime";
const char UpdatePositionArgs::kDurableOpTimeFieldName[] = "durableOpTime";
const char UpdatePositionArgs::kMemberIdFieldName[] = "memberId";
const char UpdatePositionArgs::kConfigVersionFieldName[] = "cfgver";

UpdatePositionArgs::UpdateInfo::UpdateInfo(const OpTime& applied,
                                           const OpTime& durable,
                                           long long aCfgver,
                                           long long aMemberId)
    : appliedOpTime(applied), durableOpTime(durable), cfgver(aCfgver), memberId(aMemberId) {}

Status UpdatePositionArgs::initialize(const BSONObj& argsObj) {
    // grab the array of changes
    BSONElement updateArray;
    Status status = bsonExtractTypedField(argsObj, kUpdateArrayFieldName, Array, &updateArray);
    if (!status.isOK())
        return status;

    // now parse each array entry into an update
    BSONObjIterator i(updateArray.Obj());
    while (i.more()) {
        BSONObj entry = i.next().Obj();

        OpTime appliedOpTime;
        status = bsonExtractOpTimeField(entry, kAppliedOpTimeFieldName, &appliedOpTime);
        if (!status.isOK())
            return status;

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

        _updates.push_back(UpdateInfo(appliedOpTime, durableOpTime, cfgver, memberID));
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
            update->durableOpTime.append(&updateEntry, kDurableOpTimeFieldName);
            update->appliedOpTime.append(&updateEntry, kAppliedOpTimeFieldName);
        }
        updateArray.doneFast();
    }
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo
