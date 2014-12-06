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

#include "mongo/db/repl/update_position_args.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {


    UpdatePositionArgs::UpdateInfo::UpdateInfo(
            const OID& anRid, const OpTime& aTs, long long aCfgver, long long aMemberId)
        : rid(anRid), ts(aTs), cfgver(aCfgver), memberId(aMemberId) {}

namespace {

    const std::string kCommandFieldName = "replSetUpdatePosition";
    const std::string kUpdateArrayFieldName = "optimes";

    const std::string kLegalUpdatePositionFieldNames[] = {
        kCommandFieldName,
        kUpdateArrayFieldName,
    };

    const std::string kMemberRIDFieldName = "_id";
    const std::string kMemberConfigFieldName = "config";
    const std::string kOpTimeFieldName = "optime";
    const std::string kMemberIdFieldName = "memberId";
    const std::string kConfigVersionFieldName = "cfgver";

    const std::string kLegalUpdateInfoFieldNames[] = {
        kMemberConfigFieldName,
        kMemberRIDFieldName,
        kOpTimeFieldName,
        kMemberIdFieldName,
        kConfigVersionFieldName,
    };

} // namespace

    Status UpdatePositionArgs::initialize(const BSONObj& argsObj) {
        Status status = bsonCheckOnlyHasFields("UpdatePositionArgs",
                                               argsObj,
                                               kLegalUpdatePositionFieldNames);

        if (!status.isOK())
            return status;

        // grab the array of changes
        BSONElement updateArray;
        status = bsonExtractTypedField(argsObj, kUpdateArrayFieldName, Array, &updateArray);
        if (!status.isOK())
            return status;

        // now parse each array entry into an update
        BSONObjIterator i(updateArray.Obj());
        while(i.more()) {
            BSONObj entry = i.next().Obj();
            status = bsonCheckOnlyHasFields("UpdateInfoArgs",
                                            entry,
                                            kLegalUpdateInfoFieldNames);
            if (!status.isOK())
                return status;

            OpTime ts;
            status = bsonExtractOpTimeField(entry, kOpTimeFieldName, &ts);
            if (!status.isOK())
                return status;

            // TODO(spencer): The following three fields are optional in 2.8, but should be made
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

            _updates.push_back(UpdateInfo(rid, ts, cfgver, memberID));
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
            for (UpdatePositionArgs::UpdateIterator update = updatesBegin();
                    update != updatesEnd();
                    ++update) {
                updateArray.append(BSON(kMemberRIDFieldName << update->rid <<
                                        kOpTimeFieldName << update->ts <<
                                        kConfigVersionFieldName << update->cfgver <<
                                        kMemberIdFieldName << update->memberId));
            }
            updateArray.doneFast();
        }
        return builder.obj();
    }

}  // namespace repl
}  // namespace mongo
