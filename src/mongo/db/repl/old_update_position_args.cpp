/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/old_update_position_args.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"

namespace mongo {
namespace repl {

const char OldUpdatePositionArgs::kCommandFieldName[] = "replSetUpdatePosition";
const char OldUpdatePositionArgs::kUpdateArrayFieldName[] = "optimes";
const char OldUpdatePositionArgs::kMemberRIDFieldName[] = "_id";
const char OldUpdatePositionArgs::kMemberConfigFieldName[] = "config";
const char OldUpdatePositionArgs::kOpTimeFieldName[] = "optime";
const char OldUpdatePositionArgs::kMemberIdFieldName[] = "memberId";
const char OldUpdatePositionArgs::kConfigVersionFieldName[] = "cfgver";

OldUpdatePositionArgs::UpdateInfo::UpdateInfo(const OID& anRid,
                                              const OpTime& aTs,
                                              long long aCfgver,
                                              long long aMemberId)
    : rid(anRid), ts(aTs), cfgver(aCfgver), memberId(aMemberId) {}

namespace {

const std::string kLegalUpdatePositionFieldNames[] = {
    OldUpdatePositionArgs::kCommandFieldName, OldUpdatePositionArgs::kUpdateArrayFieldName,
};

const std::string kLegalUpdateInfoFieldNames[] = {
    OldUpdatePositionArgs::kMemberConfigFieldName,
    OldUpdatePositionArgs::kMemberRIDFieldName,
    OldUpdatePositionArgs::kOpTimeFieldName,
    OldUpdatePositionArgs::kMemberIdFieldName,
    OldUpdatePositionArgs::kConfigVersionFieldName,
};

}  // namespace

Status OldUpdatePositionArgs::initialize(const BSONObj& argsObj) {
    Status status =
        bsonCheckOnlyHasFields("OldUpdatePositionArgs", argsObj, kLegalUpdatePositionFieldNames);

    if (!status.isOK())
        return status;

    // grab the array of changes
    BSONElement updateArray;
    status = bsonExtractTypedField(argsObj, kUpdateArrayFieldName, Array, &updateArray);
    if (!status.isOK())
        return status;

    // now parse each array entry into an update
    BSONObjIterator i(updateArray.Obj());
    while (i.more()) {
        BSONObj entry = i.next().Obj();
        status = bsonCheckOnlyHasFields("UpdateInfoArgs", entry, kLegalUpdateInfoFieldNames);
        if (!status.isOK())
            return status;

        OpTime opTime;
        if (entry[kOpTimeFieldName].isABSONObj()) {
            // In protocol version 1, { ts: <timestamp>, t: term }
            Status status = bsonExtractOpTimeField(entry, kOpTimeFieldName, &opTime);
            if (!status.isOK())
                return status;
        } else {
            Timestamp ts;
            status = bsonExtractTimestampField(entry, kOpTimeFieldName, &ts);
            if (!status.isOK())
                return status;
            opTime = OpTime(ts, OpTime::kUninitializedTerm);
        }
        if (!status.isOK())
            return status;

        // TODO(spencer): The following three fields are optional in 3.0, but should be made
        // required or ignored in 3.0
        long long cfgver;
        status = bsonExtractIntegerFieldWithDefault(entry, kConfigVersionFieldName, -1, &cfgver);
        if (!status.isOK())
            return status;

        OID rid;
        status = bsonExtractOIDFieldWithDefault(entry, kMemberRIDFieldName, OID(), &rid);
        if (!status.isOK())
            return status;

        long long memberID;
        status = bsonExtractIntegerFieldWithDefault(entry, kMemberIdFieldName, -1, &memberID);
        if (!status.isOK())
            return status;

        _updates.push_back(UpdateInfo(rid, opTime, cfgver, memberID));
    }

    return Status::OK();
}

BSONObj OldUpdatePositionArgs::toBSON() const {
    BSONObjBuilder builder;
    // add command name
    builder.append(kCommandFieldName, 1);

    // build array of updates
    if (!_updates.empty()) {
        BSONArrayBuilder updateArray(builder.subarrayStart(kUpdateArrayFieldName));
        for (OldUpdatePositionArgs::UpdateIterator update = updatesBegin(); update != updatesEnd();
             ++update) {
            updateArray.append(BSON(kMemberRIDFieldName << update->rid << kOpTimeFieldName
                                                        << update->ts.getTimestamp()
                                                        << kConfigVersionFieldName
                                                        << update->cfgver
                                                        << kMemberIdFieldName
                                                        << update->memberId));
        }
        updateArray.doneFast();
    }
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo
