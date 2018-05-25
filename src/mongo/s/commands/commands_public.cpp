/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

bool cursorCommandPassthrough(OperationContext* opCtx,
                              StringData dbName,
                              const CachedDatabaseInfo& dbInfo,
                              const BSONObj& cmdObj,
                              const NamespaceString& nss,
                              BSONObjBuilder* out) {
    auto response = executeCommandAgainstDatabasePrimary(
        opCtx,
        dbName,
        dbInfo,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            dbInfo.primaryId(),
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager()));

    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, out);
    return true;
}

bool nonShardedCollectionCommandPassthrough(OperationContext* opCtx,
                                            StringData dbName,
                                            const NamespaceString& nss,
                                            const CachedCollectionRoutingInfo& routingInfo,
                                            const BSONObj& cmdObj,
                                            Shard::RetryPolicy retryPolicy,
                                            BSONObjBuilder* out) {
    const StringData cmdName(cmdObj.firstElementFieldName());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !routingInfo.cm());

    auto responses = scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                                dbName,
                                                                nss,
                                                                routingInfo,
                                                                cmdObj,
                                                                ReadPreferenceSetting::get(opCtx),
                                                                retryPolicy,
                                                                {},
                                                                {});
    invariant(responses.size() == 1);

    const auto cmdResponse = uassertStatusOK(std::move(responses.front().swResponse));
    const auto status = getStatusFromCommandResult(cmdResponse.data);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !status.isA<ErrorCategory::StaleShardVersionError>());

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.data));
    return status.isOK();
}

class NotAllowedOnShardedCollectionCmd : public BasicCommand {
protected:
    NotAllowedOnShardedCollectionCmd(const char* n) : BasicCommand(n) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on sharded collection",
                !routingInfo.cm());

        const auto primaryShard = routingInfo.db().primary();

        // Here, we first filter the command before appending an UNSHARDED shardVersion, because
        // "shardVersion" is one of the fields that gets filtered out.
        BSONObj filteredCmdObj(CommandHelpers::filterCommandRequestForPassthrough(cmdObj));
        BSONObj filteredCmdObjWithVersion(
            appendShardVersion(filteredCmdObj, ChunkVersion::UNSHARDED()));

        auto commandResponse = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting::get(opCtx),
            dbName,
            primaryShard->isConfig() ? filteredCmdObj : filteredCmdObjWithVersion,
            Shard::RetryPolicy::kIdempotent));

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on a sharded collection",
                !ErrorCodes::isStaleShardVersionError(commandResponse.commandStatus.code()));

        uassertStatusOK(commandResponse.commandStatus);

        if (!commandResponse.writeConcernStatus.isOK()) {
            appendWriteConcernErrorToCmdResponse(
                primaryShard->getId(), commandResponse.response["writeConcernError"], result);
        }
        result.appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(std::move(commandResponse.response)));

        return true;
    }
};

class RenameCollectionCmd : public BasicCommand {
public:
    RenameCollectionCmd() : BasicCommand("renameCollection") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString fromNss(parseNs(dbName, cmdObj));
        const NamespaceString toNss([&cmdObj] {
            const auto fullnsToElt = cmdObj["to"];
            uassert(ErrorCodes::InvalidNamespace,
                    "'to' must be of type String",
                    fullnsToElt.type() == BSONType::String);
            return fullnsToElt.valueStringData();
        }());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << toNss.ns(),
                toNss.isValid());

        const auto fromRoutingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, fromNss));
        uassert(13138, "You can't rename a sharded collection", !fromRoutingInfo.cm());

        const auto toRoutingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, toNss));
        uassert(13139, "You can't rename to a sharded collection", !toRoutingInfo.cm());

        uassert(13137,
                "Source and destination collections must be on same shard",
                fromRoutingInfo.db().primaryId() == toRoutingInfo.db().primaryId());

        return nonShardedCollectionCommandPassthrough(
            opCtx,
            NamespaceString::kAdminDb,
            fromNss,
            fromRoutingInfo,
            appendAllowImplicitCreate(CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                      true),
            Shard::RetryPolicy::kNoRetry,
            &result);
    }

} renameCollectionCmd;

class CopyDBCmd : public BasicCommand {
public:
    CopyDBCmd() : BasicCommand("copydb") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return copydb::checkAuthForCopydbCommand(client, dbname, cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto todbElt = cmdObj["todb"];
        uassert(ErrorCodes::InvalidNamespace,
                "'todb' must be of type String",
                todbElt.type() == BSONType::String);
        const std::string todb = todbElt.str();
        uassert(ErrorCodes::InvalidNamespace,
                "Invalid todb argument",
                NamespaceString::validDBName(todb, NamespaceString::DollarInDbNameBehavior::Allow));

        auto toDbInfo = uassertStatusOK(createShardDatabase(opCtx, todb));
        uassert(ErrorCodes::IllegalOperation,
                "Cannot copy to a sharded database",
                !toDbInfo.shardingEnabled());

        const auto copyDbCmdObj = cmdObj["fromhost"] ? cmdObj : [&] {
            const auto fromDbElt = cmdObj["fromdb"];
            uassert(ErrorCodes::InvalidNamespace,
                    "'fromdb' must be of type String",
                    fromDbElt.type() == BSONType::String);
            const std::string fromdb = fromDbElt.str();
            uassert(ErrorCodes::InvalidNamespace,
                    "invalid fromdb argument",
                    NamespaceString::validDBName(fromdb,
                                                 NamespaceString::DollarInDbNameBehavior::Allow));

            auto fromDbInfo = uassertStatusOK(createShardDatabase(opCtx, fromdb));
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot copy from a sharded database",
                    !fromDbInfo.shardingEnabled());

            // If the source and destination are the same, there is no need to attach the 'fromhost'
            // field since the copy is entirely local to the node
            if (fromDbInfo.primaryId() == toDbInfo.primaryId()) {
                return cmdObj;
            }

            BSONObjBuilder copyDbCmdObjBuilder;

            // Copy everything but the "fromhost" parameter
            BSONForEach(e, cmdObj) {
                if (strcmp(e.fieldName(), "fromhost")) {
                    copyDbCmdObjBuilder.append(e);
                }
            }

            // Append "fromhost" as the database's primary
            {
                const auto shard = uassertStatusOK(
                    Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromDbInfo.primaryId()));
                copyDbCmdObjBuilder.append("fromhost", shard->getConnString().toString());
            }

            return copyDbCmdObjBuilder.obj();
        }();

        // copyDb creates multiple collections and should handle collection creation differently
        auto response = executeCommandAgainstDatabasePrimary(
            opCtx,
            NamespaceString::kAdminDb,
            toDbInfo,
            appendAllowImplicitCreate(
                CommandHelpers::filterCommandRequestForPassthrough(copyDbCmdObj), true),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kNoRetry);

        const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));
        const auto status = getStatusFromCommandResult(cmdResponse.data);

        result.appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.data));
        return status.isOK();
    }

} clusterCopyDBCmd;

class ConvertToCappedCmd : public BasicCommand {
public:
    ConvertToCappedCmd() : BasicCommand("convertToCapped") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::convertToCapped);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        // convertToCapped creates a temp collection and renames it at the end. It will require
        // special handling for create collection.
        return nonShardedCollectionCommandPassthrough(
            opCtx,
            dbName,
            nss,
            routingInfo,
            appendAllowImplicitCreate(CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                      true),
            Shard::RetryPolicy::kIdempotent,
            &result);
    }

} convertToCappedCmd;

class SplitVectorCmd : public NotAllowedOnShardedCollectionCmd {
public:
    SplitVectorCmd() : NotAllowedOnShardedCollectionCmd("splitVector") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        uassert(ErrorCodes::IllegalOperation,
                "Performing splitVector across dbs isn't supported via mongos",
                nss.db() == dbName);

        return NotAllowedOnShardedCollectionCmd::run(opCtx, dbName, cmdObj, result);
    }

} splitVectorCmd;

class CmdListCollections : public BasicCommand {
public:
    CmdListCollections() : BasicCommand("listCollections") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (authzSession->isAuthorizedToListCollections(dbname, cmdObj)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    BSONObj rewriteCommandForListingOwnCollections(OperationContext* opCtx,
                                                   const std::string& dbName,
                                                   const BSONObj& cmdObj) {
        mutablebson::Document rewrittenCmdObj(cmdObj);
        mutablebson::Element ownCollections =
            mutablebson::findFirstChildNamed(rewrittenCmdObj.root(), "authorizedCollections");

        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

        // We must strip $ownCollections from the delegated command.
        uassertStatusOK(ownCollections.remove());

        BSONObj collectionFilter;

        // Extract and retain any previous filter
        mutablebson::Element oldFilter =
            mutablebson::findFirstChildNamed(rewrittenCmdObj.root(), "filter");

        // Make a new filter, containing a $and array.
        mutablebson::Element newFilter = rewrittenCmdObj.makeElementObject("filter");
        mutablebson::Element newFilterAnd = rewrittenCmdObj.makeElementArray("$and");
        uassertStatusOK(newFilter.pushBack(newFilterAnd));

        // Append a rule to the $and, which rejects system collections.
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^(?!system\\.)"))));
        uassertStatusOK(newFilterAnd.pushBack(systemCollectionsFilter));

        if (!authzSession->isAuthorizedForAnyActionOnResource(
                ResourcePattern::forDatabaseName(dbName))) {
            // We passed an auth check which said we might be able to render some collections,
            // but it doesn't seem like we should render all of them. We must filter.

            // Compute the set of collection names which would be permissible to return.
            std::set<std::string> collectionNames;
            for (UserNameIterator nameIter = authzSession->getAuthenticatedUserNames();
                 nameIter.more();
                 nameIter.next()) {
                User* authUser = authzSession->lookupUser(*nameIter);
                const User::ResourcePrivilegeMap& resourcePrivilegeMap = authUser->getPrivileges();
                for (const std::pair<ResourcePattern, Privilege>& resourcePrivilege :
                     resourcePrivilegeMap) {
                    const auto& resource = resourcePrivilege.first;
                    if (resource.isCollectionPattern() || (resource.isExactNamespacePattern() &&
                                                           resource.databaseToMatch() == dbName)) {
                        collectionNames.emplace(resource.collectionToMatch().toString());
                    }
                }
            }

            // Construct a new filter predicate which returns only collections we were found to
            // have privileges for.
            BSONObjBuilder predicateBuilder;
            BSONObjBuilder nameBuilder(predicateBuilder.subobjStart("name"));
            BSONArrayBuilder setBuilder(nameBuilder.subarrayStart("$in"));

            // Load the de-duplicated set into a BSON array
            for (StringData collectionName : collectionNames) {
                setBuilder << collectionName;
            }
            setBuilder.done();
            nameBuilder.done();

            collectionFilter = predicateBuilder.obj();

            // Filter the results by our collection names.
            mutablebson::Element newFilterAndIn =
                rewrittenCmdObj.makeElementObject("", collectionFilter);
            uassertStatusOK(newFilterAnd.pushBack(newFilterAndIn));
        }

        // If there was a pre-existing filter, compose it with our new one.
        if (oldFilter.ok()) {
            uassertStatusOK(oldFilter.remove());
            uassertStatusOK(newFilterAnd.pushBack(oldFilter));
        }

        // Attach our new composite filter back onto the listCollections command object.
        uassertStatusOK(rewrittenCmdObj.root().pushBack(newFilter));

        return rewrittenCmdObj.getObject();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto nss(NamespaceString::makeListCollectionsNSS(dbName));

        BSONObj newCmd = cmdObj;

        if (newCmd["authorizedCollections"].trueValue()) {
            newCmd = rewriteCommandForListingOwnCollections(opCtx, dbName, cmdObj);
        }

        auto dbInfoStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
        if (!dbInfoStatus.isOK()) {
            return appendEmptyResultSet(opCtx, result, dbInfoStatus.getStatus(), nss.ns());
        }

        return cursorCommandPassthrough(
            opCtx, dbName, dbInfoStatus.getValue(), newCmd, nss, &result);
    }

} cmdListCollections;

class CmdListIndexes : public BasicCommand {
public:
    CmdListIndexes() : BasicCommand("listIndexes") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listIndexes ActionType on the database, or find on system.indexes for pre
        // 3.0 systems.
        const NamespaceString ns(parseNs(dbname, cmdObj));

        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns),
                                                           ActionType::listIndexes) ||
            authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.indexes")),
                ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list indexes on collection: "
                                    << ns.coll());
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        return cursorCommandPassthrough(opCtx,
                                        nss.db(),
                                        routingInfo.db(),
                                        cmdObj,
                                        NamespaceString::makeListIndexesNSS(nss.db(), nss.coll()),
                                        &result);
    }

} cmdListIndexes;

}  // namespace
}  // namespace mongo
