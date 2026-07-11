// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/resize_oplog_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

class CmdReplSetResizeOplog : public BasicCommand {
public:
    CmdReplSetResizeOplog() : BasicCommand("replSetResizeOplog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Resize oplog using size (in MBs) and optionally, retention (in terms of hours)";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
        if (authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::replSetResizeOplog)) {
            return Status::OK();
        }
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& jsobj,
             BSONObjBuilder& result) override {
        // Use LocalWrite intent so the IntentRegistry does not enforce primary-only
        // write access, allowing replSetResizeOplog to run on secondaries as intended.
        AutoGetCollection coll(
            opCtx,
            NamespaceString::kRsOplogNamespace,
            MODE_X,
            auto_get_collection::Options{}.globalLockOptions(Lock::GlobalLockOptions{
                .explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite}));
        uassert(ErrorCodes::NamespaceNotFound, "oplog does not exist", coll);
        uassert(ErrorCodes::IllegalOperation, "oplog isn't capped", coll->isCapped());

        auto params =
            ReplSetResizeOplogRequest::parse(jsobj, IDLParserContext("replSetResizeOplog"));

        return writeConflictRetry(opCtx, "replSetResizeOplog", coll->ns(), [&] {
            WriteUnitOfWork wunit(opCtx);

            CollectionWriter writer{opCtx, coll};

            if (auto sizeMB = params.getSize()) {
                const long long sizeBytes = *sizeMB * 1024 * 1024;
                uassertStatusOK(writer.getWritableCollection(opCtx)->updateCappedSize(
                    opCtx, sizeBytes, /*newCappedMax=*/boost::none));
            }

            if (auto minRetentionHoursOpt = params.getMinRetentionHours()) {
                storageGlobalParams.oplogMinRetentionHours.store(*minRetentionHoursOpt);
                storageGlobalParams.oplogMinRetentionInitializedUsingDefault = false;
            }
            wunit.commit();

            LOGV2(20497,
                  "replSetResizeOplog success",
                  "size"_attr = coll->getCollectionOptions().cappedSize,
                  "minRetentionHours"_attr = storageGlobalParams.oplogMinRetentionHours.load());
            return true;
        });
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetResizeOplog).forShard();

}  // namespace
}  // namespace mongo
