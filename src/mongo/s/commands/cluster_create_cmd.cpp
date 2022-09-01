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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

void checkCollectionOptions(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const CollectionOptions& options) {
    auto dbName = ns.db();
    auto dbInfo = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
    BSONObjBuilder listCollCmd;
    listCollCmd.append("listCollections", 1);
    listCollCmd.append("filter", BSON("name" << ns.coll()));

    auto response = executeCommandAgainstDatabasePrimary(
        opCtx,
        dbName,
        dbInfo,
        CommandHelpers::filterCommandRequestForPassthrough(listCollCmd.obj()),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent);
    uassertStatusOK(response.swResponse);

    auto responseData = response.swResponse.getValue().data;
    auto listCollectionsStatus = mongo::getStatusFromCommandResult(responseData);
    uassertStatusOK(listCollectionsStatus);

    auto cursorObj = responseData["cursor"].Obj();
    auto collections = cursorObj["firstBatch"].Obj();

    BSONObjIterator collIter(collections);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "cannot find ns: " << ns.ns(),
            collIter.more());

    auto collectionDetails = collIter.next();
    CollectionOptions actualOptions =
        uassertStatusOK(CollectionOptions::parse(collectionDetails["options"].Obj()));
    // TODO: SERVER-33048 check idIndex field

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "ns: " << ns.ns()
                          << " already exists with different options: " << actualOptions.toBSON(),
            options.matchesStorageOptions(
                actualOptions, CollatorFactoryInterface::get(opCtx->getServiceContext())));
}

class CreateCmd final : public CreateCmdVersion1Gen<CreateCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(auth::checkAuthForCreate(
                opCtx, AuthorizationSession::get(opCtx->getClient()), request(), true));
        }

        CreateCommandReply typedRun(OperationContext* opCtx) final {
            auto cmd = request();
            auto dbName = cmd.getDbName();
            cluster::createDatabase(opCtx, dbName.toStringWithTenantId());

            uassert(ErrorCodes::InvalidOptions,
                    "specify size:<n> when capped is true",
                    !cmd.getCapped() || cmd.getSize());
            uassert(ErrorCodes::InvalidOptions,
                    "the 'temp' field is an invalid option",
                    !cmd.getTemp());

            // Manually forward the create collection command to the primary shard.
            const auto dbInfo = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(
                opCtx, dbName.toStringWithTenantId()));
            auto response = [&] {
                auto cmdToSend = cmd.toBSON({});
                cmdToSend = CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON({}));
                cmdToSend = applyReadWriteConcern(opCtx, this, cmdToSend);
                // If the DB primary resides on a shard, forward the create command applying the
                // metadata that characterise an unsharded CRUD request according to the versioning
                // protocol (the received DB version + an "UNSHARDED" shard version): this will
                // guarantee that sharded DDL ops targeting the same namespace won't be executed in
                // parallel.
                // If the DB primary is hosted by the config server, apply the original metadata.
                if (dbInfo->getPrimary() != ShardId::kConfigServerId) {
                    cmdToSend = appendShardVersion(cmdToSend, ShardVersion::UNSHARDED());
                }
                cmdToSend = appendDbVersionIfPresent(cmdToSend, dbInfo);

                std::vector<AsyncRequestsSender::Request> arsRequests{
                    AsyncRequestsSender::Request(dbInfo->getPrimary(), cmdToSend)};

                auto arsResponses =
                    gatherResponses(opCtx,
                                    dbName.db(),
                                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                    Shard::RetryPolicy::kIdempotent,
                                    arsRequests);
                return uassertStatusOK(arsResponses.front().swResponse);
            }();

            const auto createStatus = mongo::getStatusFromCommandResult(response.data);
            if (createStatus == ErrorCodes::NamespaceExists &&
                !opCtx->inMultiDocumentTransaction()) {
                // NamespaceExists will cause multi-document transactions to implicitly abort, so
                // mongos should surface this error to the client.
                checkCollectionOptions(
                    opCtx, cmd.getNamespace(), CollectionOptions::fromCreateCommand(cmd));
            } else {
                uassertStatusOK(createStatus);
            }

            uassertStatusOK(getWriteConcernStatusFromCommandResult(response.data));
            return CreateCommandReply();
        }
    };

} createCmd;

}  // namespace
}  // namespace mongo
