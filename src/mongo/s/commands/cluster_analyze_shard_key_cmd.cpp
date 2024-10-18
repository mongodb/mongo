/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <iterator>
#include <memory>
#include <set>
#include <string>


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

class AnalyzeShardKeyCmd : public TypedCommand<AnalyzeShardKeyCmd> {
public:
    using Request = AnalyzeShardKey;
    using Response = AnalyzeShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            uassertStatusOK(validateNamespace(nss));

            const auto& catalogCache = Grid::get(opCtx)->catalogCache();
            const auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
            auto primaryShardId = cri.cm.dbPrimary();

            std::set<ShardId> candidateShardIds;
            if (cri.cm.hasRoutingTable()) {
                cri.cm.getAllShardIds(&candidateShardIds);
            } else {
                candidateShardIds.insert(primaryShardId);
            }

            PseudoRandom random{SecureRandom{}.nextInt64()};

            // On secondaries, the database and shard version check is only performed for commands
            // that specify a readConcern (that is not "available"). Therefore, to opt into the
            // check, explicitly attach the readConcern.
            auto newRequest = request();
            if (!newRequest.getReadConcern()) {
                newRequest.setReadConcern(extractReadConcern(opCtx));
            }
            const auto unversionedCmdObj =
                CommandHelpers::filterCommandRequestForPassthrough(newRequest.toBSON());

            while (true) {
                // Select a random shard.
                invariant(!candidateShardIds.empty());
                auto shardId = [&] {
                    // The monotonicity check can return an incorrect result if the collection has
                    // gone through chunk migrations since chunk migration deletes documents from
                    // the donor shard and re-inserts them on the recipient shard so there is no
                    // guarantee that the insertion order from the client is preserved. Therefore,
                    // the likelihood of an incorrect result is correlated to the ratio between
                    // the number of documents inserted by the client and the number of documents
                    // inserted by chunk migrations. Prioritizing the primary shard helps lower the
                    // risk of incorrect results since if the collection did not start out as being
                    // sharded (which applies to most cases), the primary shard is likely to be the
                    // shard with the least number of documents inserted by chunk migrations since
                    // all the data starts out there.
                    if (candidateShardIds.find(primaryShardId) != candidateShardIds.end()) {
                        return primaryShardId;
                    }

                    auto it = candidateShardIds.begin();
                    std::advance(it, random.nextInt64(candidateShardIds.size()));
                    return *it;
                }();
                candidateShardIds.erase(shardId);

                uassert(ErrorCodes::IllegalOperation,
                        "Cannot analyze a shard key for a collection in a fixed database",
                        !cri.cm.dbVersion().isFixed());

                auto expCtx = makeExpressionContextWithDefaultsForTargeter(
                    opCtx, nss, cri, BSONObj(), boost::none, boost::none, boost::none);
                // Execute the command against the shard.
                auto requests =
                    buildVersionedRequests(expCtx, nss, cri, {shardId}, unversionedCmdObj);
                invariant(requests.size() == 1);

                ReadPreferenceSetting readPref = request().getReadPreference().value_or(
                    ReadPreferenceSetting(ReadPreference::SecondaryPreferred));

                try {
                    auto response = gatherResponses(opCtx,
                                                    DatabaseName::kAdmin,
                                                    std::move(readPref),
                                                    Shard::RetryPolicy::kIdempotent,
                                                    requests)
                                        .front();
                    uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
                    return AnalyzeShardKeyResponse::parse(
                        IDLParserContext("clusterAnalyzeShardKey"),
                        response.swResponse.getValue().data);
                } catch (const ExceptionFor<ErrorCodes::CollectionIsEmptyLocally>& ex) {
                    uassert(ErrorCodes::IllegalOperation,
                            str::stream() << "Cannot analyze a shard key for an empty collection: "
                                          << redact(ex),
                            !candidateShardIds.empty());

                    LOGV2(6875300,
                          "Failed to analyze shard key on the selected shard since it did not "
                          "have any documents for the collection locally. Retrying on a different "
                          "shard.",
                          logAttrs(nss),
                          "shardKey"_attr = request().getKey(),
                          "shardId"_attr = shardId,
                          "error"_attr = ex.toString());
                } catch (
                    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&) {
                    // Don't propagate CommandOnShardedViewNotSupportedOnMongod errors for clarity,
                    // even for the cases where this is thrown as an exception.
                    uasserted(ErrorCodes::CommandNotSupportedOnView,
                              "Operation not supported for a view");
                }
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::analyzeShardKey));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    std::string help() const override {
        return "Returns metrics for evaluating a shard key for a collection.";
    }
};
MONGO_REGISTER_COMMAND(AnalyzeShardKeyCmd).forRouter();

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo
