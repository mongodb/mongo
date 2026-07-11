// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/split_vector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <iosfwd>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        // The range needs to be entirely owned by one shard. splitVector is only supported for
        // internal use. The common pattern is to take the min/max boundaries from a chunk,
        // where the max represent a non-included boundary.
        uassert(
            ErrorCodes::InvalidOptions,
            fmt::format("The range {} for the namespace {} is required to be owned by one shard",
                        rangeString(min, max),
                        nss.toStringForErrorMsg()),
            !collection.getShardingDescription().isSharded() ||
                collection.getShardingFilter()->isRangeEntirelyOwned(
                    min, max, false /*includeMaxBound*/));

        auto splitKeys = splitVector(opCtx,
                                     collection,
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
