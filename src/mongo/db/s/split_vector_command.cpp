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

#include <iosfwd>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/split_vector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

using namespace fmt::literals;
namespace mongo {

using std::string;
using std::stringstream;

namespace {

std::string rangeString(const BSONObj& min, const BSONObj& max) {
    std::ostringstream os;
    os << "{min: " << min.toString() << " , max" << max.toString() << " }";
    return os.str();
}

class SplitVector : public ErrmsgCommandDeprecated {
public:
    SplitVector() : ErrmsgCommandDeprecated("splitVector") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command.\n"
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

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) override {

        const NamespaceString nss(parseNs(dbName, jsobj));
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
            maxSplitPoints = maxSplitPointsElem.safeNumberLong();
        }

        boost::optional<long long> maxChunkObjects;
        BSONElement maxChunkObjectsElem = jsobj["maxChunkObjects"];
        if (maxChunkObjectsElem.isNumber()) {
            maxChunkObjects = maxChunkObjectsElem.safeNumberLong();
            uassert(ErrorCodes::InvalidOptions,
                    "maxChunkObjects must be greater than 0",
                    *maxChunkObjects > 0);
        }

        const auto maxChunkSizeBytes = [&]() {
            BSONElement maxSizeElem = jsobj["maxChunkSize"];
            BSONElement maxSizeBytesElem = jsobj["maxChunkSizeBytes"];

            boost::optional<long long> ret = boost::none;
            // Use maxChunkSize if present otherwise maxChunkSizeBytes
            if (maxSizeElem.isNumber()) {
                long long maxChunkSizeMB = maxSizeElem.safeNumberLong();
                // Prevent maxChunkSizeBytes overflow. Check aimed to avoid fuzzer failures
                // since users are definitely not expected to specify maxChunkSize in exabytes.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "maxChunkSize must lie within the range [1MB, 1024MB]",
                        maxChunkSizeMB >= 1 && maxChunkSizeMB <= 1024);
                ret = maxChunkSizeMB << 20;
            } else if (maxSizeBytesElem.isNumber()) {
                ret = maxSizeBytesElem.safeNumberLong();
                uassert(ErrorCodes::InvalidOptions,
                        "The specified max chunk size must lie within the range [1MB, 1024MB]",
                        *ret >= 1024 * 1024 && *ret <= 1024 * 1024 * 1024);
            }

            return ret;
        }();

        {
            const auto collection =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      opCtx, nss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            // The range needs to be entirely owned by one shard. splitVector is only supported for
            // internal use. The common pattern is to take the min/max boundaries from a chunk,
            // where the max represent a non-included boundary.
            uassert(ErrorCodes::InvalidOptions,
                    "The range {} for the namespace {} is required to be owned by one shard"_format(
                        rangeString(min, max), nss.toStringForErrorMsg()),
                    !collection.getShardingDescription().isSharded() ||
                        collection.getShardingFilter()->isRangeEntirelyOwned(
                            min, max, false /*includeMaxBound*/));
        }

        auto splitKeys = splitVector(opCtx,
                                     nss,
                                     keyPattern,
                                     min,
                                     max,
                                     force,
                                     maxSplitPoints,
                                     maxChunkObjects,
                                     maxChunkSizeBytes);

        result.append("splitKeys", splitKeys);
        return true;
    }
};
MONGO_REGISTER_COMMAND(SplitVector).forShard();

}  // namespace
}  // namespace mongo
