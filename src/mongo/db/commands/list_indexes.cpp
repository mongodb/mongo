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


#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/list_indexes_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

// The allowed fields have to be in sync with those defined in 'src/mongo/db/list_indexes.idl'.
static std::map<StringData, std::set<IndexType>> allowedFieldNames = {
    {ListIndexesReplyItem::k2dsphereIndexVersionFieldName,
     {IndexType::INDEX_2DSPHERE, IndexType::INDEX_2DSPHERE_BUCKET}},
    {ListIndexesReplyItem::kBackgroundFieldName, {}},
    {ListIndexesReplyItem::kBitsFieldName, {IndexType::INDEX_2D}},
    {ListIndexesReplyItem::kBucketSizeFieldName, {}},
    {ListIndexesReplyItem::kBuildUUIDFieldName, {}},
    {ListIndexesReplyItem::kClusteredFieldName, {}},
    {ListIndexesReplyItem::kCoarsestIndexedLevelFieldName, {IndexType::INDEX_2DSPHERE}},
    {ListIndexesReplyItem::kCollationFieldName, {}},
    {ListIndexesReplyItem::kDefault_languageFieldName, {}},
    {ListIndexesReplyItem::kDropDupsFieldName, {}},
    {ListIndexesReplyItem::kExpireAfterSecondsFieldName, {}},
    {ListIndexesReplyItem::kFinestIndexedLevelFieldName, {IndexType::INDEX_2DSPHERE}},
    {ListIndexesReplyItem::kHiddenFieldName, {}},
    {ListIndexesReplyItem::kIndexBuildInfoFieldName, {}},
    {ListIndexesReplyItem::kKeyFieldName, {}},
    {ListIndexesReplyItem::kLanguage_overrideFieldName, {}},
    {ListIndexesReplyItem::kMaxFieldName, {IndexType::INDEX_2D}},
    {ListIndexesReplyItem::kMinFieldName, {IndexType::INDEX_2D}},
    {ListIndexesReplyItem::kNameFieldName, {}},
    {ListIndexesReplyItem::kNsFieldName, {}},
    {ListIndexesReplyItem::kOriginalSpecFieldName, {}},
    {ListIndexesReplyItem::kPartialFilterExpressionFieldName, {}},
    {ListIndexesReplyItem::kPrepareUniqueFieldName, {}},
    {ListIndexesReplyItem::kSparseFieldName, {}},
    {ListIndexesReplyItem::kSpecFieldName, {}},
    {ListIndexesReplyItem::kStorageEngineFieldName, {}},
    {ListIndexesReplyItem::kTextIndexVersionFieldName, {IndexType::INDEX_TEXT}},
    {ListIndexesReplyItem::kUniqueFieldName, {}},
    {ListIndexesReplyItem::kVFieldName, {}},
    {ListIndexesReplyItem::kWeightsFieldName, {IndexType::INDEX_TEXT}},
    {ListIndexesReplyItem::kWildcardProjectionFieldName, {IndexType::INDEX_WILDCARD}},
    {ListIndexesReplyItem::kColumnstoreProjectionFieldName, {IndexType::INDEX_COLUMN}},
    {ListIndexesReplyItem::kColumnstoreCompressorFieldName, {IndexType::INDEX_COLUMN}},
};

/**
 * Returns index specs, with resolved namespace, from the catalog for this listIndexes request.
 */
using IndexSpecsWithNamespaceString = std::pair<std::list<BSONObj>, NamespaceString>;
IndexSpecsWithNamespaceString getIndexSpecsWithNamespaceString(OperationContext* opCtx,
                                                               const ListIndexes& cmd) {
    const auto& origNssOrUUID = cmd.getNamespaceOrUUID();

    bool buildUUID = cmd.getIncludeBuildUUIDs().value_or(false);
    bool indexBuildInfo = cmd.getIncludeIndexBuildInfo().value_or(false);
    invariant(!(buildUUID && indexBuildInfo));
    ListIndexesInclude additionalInclude = buildUUID ? ListIndexesInclude::BuildUUID
        : indexBuildInfo                             ? ListIndexesInclude::IndexBuildInfo
                                                     : ListIndexesInclude::Nothing;

    // Since time-series collections don't have UUIDs, we skip the time-series lookup
    // if the target collection is specified as a UUID.
    if (origNssOrUUID.isNamespaceString()) {
        auto isCommandOnTimeseriesBucketNamespace =
            cmd.getIsTimeseriesNamespace() && *cmd.getIsTimeseriesNamespace();
        if (auto timeseriesOptions = timeseries::getTimeseriesOptions(
                opCtx, origNssOrUUID.nss(), !isCommandOnTimeseriesBucketNamespace)) {
            auto bucketsNss = isCommandOnTimeseriesBucketNamespace
                ? origNssOrUUID.nss()
                : origNssOrUUID.nss().makeTimeseriesBucketsNamespace();
            AutoGetCollectionForReadCommandMaybeLockFree autoColl(opCtx, bucketsNss);

            const CollectionPtr& coll = autoColl.getCollection();
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "ns does not exist: " << bucketsNss.toStringForErrorMsg(),
                    coll);

            return std::make_pair(
                timeseries::createTimeseriesIndexesFromBucketsIndexes(
                    *timeseriesOptions,
                    listIndexesInLock(opCtx, coll, bucketsNss, additionalInclude)),
                bucketsNss.getTimeseriesViewNamespace());
        }
    }

    AutoGetCollectionForReadCommandMaybeLockFree autoColl(opCtx, origNssOrUUID);

    const auto& nss = autoColl.getNss();
    const CollectionPtr& coll = autoColl.getCollection();
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "ns does not exist: " << nss.toStringForErrorMsg(),
            coll);

    return std::make_pair(listIndexesInLock(opCtx, coll, nss, additionalInclude), nss);
}

/**
 * Lists the indexes for a given collection.
 * If 'includeBuildUUIDs' is true, then the index build uuid is also returned alongside the index
 * spec for in-progress index builds only.
 * If 'includeIndexBuildInfo' is true, then the index spec is returned in the spec subdocument, and
 * index build info is returned alongside the index spec for in-progress index builds only.
 * includeBuildUUIDs and includeIndexBuildInfo cannot both be set to true.
 *
 * Format:
 * {
 *   listIndexes: <collection name>,
 *   includeBuildUUIDs: <boolean>,
 *   includeIndexBuildInfo: <boolean>
 * }
 *
 * Return format:
 * {
 *   indexes: [
 *     <index>,
 *     ...
 *   ]
 * }
 *
 * Where '<index>' is the index spec if either the index is ready or 'includeBuildUUIDs' is false.
 * If the index is in-progress and 'includeBuildUUIDs' is true then '<index>' has the following
 * format:
 * {
 *   spec: <index spec>,
 *   buildUUID: <index build uuid>
 * }
 *
 * If 'includeIndexBuildInfo' is true, then for in-progress indexes, <index> has the following
 * format:
 * {
 *   spec: <index spec>,
 *   indexBuildInfo: {
 *     buildUUID: <index build uuid>
 *   }
 * }
 * And for complete (not in-progress) indexes:
 * {
 *   spec: <index spec>
 * }
 */

class CmdListIndexes final : public ListIndexesCmdVersion1Gen<CmdListIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }
    bool maintenanceOk() const final {
        return false;
    }
    bool adminOnly() const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return "list indexes for a collection";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            auto nssOrUUID = request().getNamespaceOrUUID();
            if (nssOrUUID.isUUID()) {
                // UUID requires opCtx to resolve, settle on just the dbname.
                return NamespaceString(request().getDbName());
            }
            return nssOrUUID.nss();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

            auto& cmd = request();
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                authzSession->isAuthorizedToParseNamespaceElement(request().getNamespaceOrUUID()));

            const auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(
                opCtx, cmd.getNamespaceOrUUID());

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to list indexes on collection:"
                                  << nss.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(nss), ActionType::listIndexes));
        }

        ListIndexesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            const auto& cmd = request();
            bool buildUUID = cmd.getIncludeBuildUUIDs().value_or(false);
            bool indexBuildInfo = cmd.getIncludeIndexBuildInfo().value_or(false);
            uassert(ErrorCodes::InvalidOptions,
                    "The includeBuildUUIDs flag and includeBuildIndexInfo flag cannot both be set "
                    "to true",
                    !(buildUUID && indexBuildInfo));
            auto indexSpecsWithNss = getIndexSpecsWithNamespaceString(opCtx, cmd);
            const auto& indexList = indexSpecsWithNss.first;
            const auto& nss = indexSpecsWithNss.second;
            return ListIndexesReply(_makeCursor(opCtx, indexList, nss));
        }

    private:
        /**
         * Constructs a cursor that iterates the index specs found in 'indexSpecsWithNss'.
         * This function does not hold any locks because it does not access in-memory
         * or on-disk data.
         */
        ListIndexesReplyCursor _makeCursor(OperationContext* opCtx,
                                           const std::list<BSONObj>& indexList,
                                           const NamespaceString& nss) {
            auto& cmd = request();

            // We need to copy the serialization context from the request to the reply object
            const auto serializationContext = cmd.getSerializationContext();

            long long batchSize = std::numeric_limits<long long>::max();
            if (cmd.getCursor() && cmd.getCursor()->getBatchSize()) {
                batchSize = *cmd.getCursor()->getBatchSize();
            }

            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);

            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto&& indexSpec : indexList) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), indexSpec.getOwned());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            auto exec = uassertStatusOK(
                plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(root),
                                            &CollectionPtr::null,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            false, /* whether returned BSON must be owned */
                                            nss));

            std::vector<mongo::ListIndexesReplyItem> firstBatch;
            FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);
                nextDoc = index_key_validate::repairIndexSpec(nss, nextDoc, allowedFieldNames);

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
                    exec->stashResult(nextDoc);
                    break;
                }

                try {
                    firstBatch.push_back(ListIndexesReplyItem::parse(
                        IDLParserContext(
                            "ListIndexesReplyItem",
                            false /* apiStrict */,
                            auth::ValidatedTenancyScope::get(opCtx),
                            nss.tenantId(),
                            SerializationContext::stateCommandReply(serializationContext)),
                        nextDoc));
                } catch (const DBException& exc) {
                    LOGV2_ERROR(5254500,
                                "Could not parse catalog entry while replying to listIndexes",
                                "entry"_attr = nextDoc,
                                "error"_attr = exc);
                    uasserted(5254501,
                              fmt::format("Could not parse catalog entry while replying to "
                                          "listIndexes. Entry: '{}'. Error: '{}'.",
                                          nextDoc.toString(),
                                          exc.toString()));
                }
                responseSizeTracker.add(nextDoc);
            }

            if (exec->isEOF()) {
                return ListIndexesReplyCursor(
                    0 /* cursorId */,
                    nss,
                    std::move(firstBatch),
                    SerializationContext::stateCommandReply(serializationContext));
            }

            exec->saveState();
            exec->detachFromOperationContext();

            // Global cursor registration must be done without holding any locks.
            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 ReadPreferenceSetting::get(opCtx),
                 cmd.toBSON({}),
                 {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::listIndexes)}});

            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(firstBatch.size());

            return ListIndexesReplyCursor(
                pinnedCursor.getCursor()->cursorid(),
                nss,
                std::move(firstBatch),
                SerializationContext::stateCommandReply(serializationContext));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListIndexes).forShard();

}  // namespace
}  // namespace mongo
