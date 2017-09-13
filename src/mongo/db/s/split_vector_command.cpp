/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/split_vector.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {

class SplitVector : public ErrmsgCommandDeprecated {
public:
    SplitVector() : ErrmsgCommandDeprecated("splitVector") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    void help(stringstream& help) const override {
        help << "Internal command.\n"
                "examples:\n"
                "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, "
                "maxChunkSize:200 }\n"
                "  maxChunkSize unit in MBs\n"
                "  May optionally specify 'maxSplitPoints' and 'maxChunkObjects' to avoid "
                "traversing the whole chunk\n"
                "  \n"
                "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, "
                "force: true }\n"
                "  'force' will produce one split point even if data is small; defaults to false\n"
                "NOTE: This command may take a while to run";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) override {

        const NamespaceString nss = NamespaceString(parseNs(dbname, jsobj));
        BSONObj keyPattern = jsobj.getObjectField("keyPattern");

        if (keyPattern.isEmpty()) {
            errmsg = "no key pattern found in splitVector";
            return false;
        }

        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        if (min.isEmpty() != max.isEmpty()) {
            errmsg = "either provide both min and max or leave both empty";
            return false;
        }

        bool force = false;
        BSONElement forceElem = jsobj["force"];
        if (forceElem.trueValue()) {
            force = true;
        }

        boost::optional<long long> maxSplitPoints;
        BSONElement maxSplitPointsElem = jsobj["maxSplitPoints"];
        if (maxSplitPointsElem.isNumber()) {
            maxSplitPoints = maxSplitPointsElem.numberLong();
        }

        boost::optional<long long> maxChunkObjects;
        BSONElement maxChunkObjectsElem = jsobj["maxChunkObjects"];
        if (maxChunkObjectsElem.isNumber()) {
            maxChunkObjects = maxChunkObjectsElem.numberLong();
        }

        boost::optional<long long> maxChunkSize;
        BSONElement maxSizeElem = jsobj["maxChunkSize"];
        if (maxSizeElem.isNumber()) {
            maxChunkSize = maxSizeElem.numberLong();
        }

        boost::optional<long long> maxChunkSizeBytes;
        maxSizeElem = jsobj["maxChunkSizeBytes"];
        if (maxSizeElem.isNumber()) {
            maxChunkSizeBytes = maxSizeElem.numberLong();
        }

        auto statusWithSplitKeys = splitVector(opCtx,
                                               nss,
                                               keyPattern,
                                               min,
                                               max,
                                               force,
                                               maxSplitPoints,
                                               maxChunkObjects,
                                               maxChunkSize,
                                               maxChunkSizeBytes);
        if (!statusWithSplitKeys.isOK()) {
            return appendCommandStatus(result, statusWithSplitKeys.getStatus());
        }

        result.append("splitKeys", statusWithSplitKeys.getValue());
        return true;
    }

} cmdSplitVector;

}  // namespace
}  // namespace mongo
