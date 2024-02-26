/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/search_index_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_process_interface.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * Passthrough command to the search index management endpoint on which the manageSearchIndex
 * command is called. Accepts requests of the IDL form createSearchIndexes.
 *
 * {
 *     createSearchIndexes: "<collection-name>",
 *     $db: "<database-name>",
 *     indexes: [
 *         {
 *             name: "<index-name>",
 *             definition: {
 *                 // search index definition fields
 *             }
 *         }
 *     ]
 * }
 *
 */
class CmdCreateSearchIndexesCommand final : public TypedCommand<CmdCreateSearchIndexesCommand> {
public:
    using Request = CreateSearchIndexesCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to create a search index. Only supported with Atlas.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        CreateSearchIndexesReply typedRun(OperationContext* opCtx) {
            throwIfNotRunningWithRemoteSearchIndexManagement();

            const auto& cmd = request();
            const auto& nss = cmd.getNamespace();

            auto collectionUUID =
                SearchIndexProcessInterface::get(opCtx)->fetchCollectionUUIDOrThrow(opCtx, nss);

            // Run the search index command against the remote search index management server.
            BSONObj manageSearchIndexResponse = getSearchIndexManagerResponse(
                opCtx, nss, collectionUUID, cmd.toBSON(BSONObj() /* commandPassthroughFields */));

            IDLParserContext ctx("CreateSearchIndexesReply Parser");
            return CreateSearchIndexesReply::parseOwned(ctx, std::move(manageSearchIndexResponse));
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            const NamespaceString& nss = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call createSearchIndexes on collection "
                                  << nss.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(nss, ActionType::createSearchIndexes));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdCreateSearchIndexesCommand).forShard().forRouter();

/**
 * Passthrough command to the search index management endpoint on which the manageSearchIndex
 * command is called. Accepts requests of the form:
 *
 * {
 *     dropSearchIndex: "<collection-name>",
 *     $db: "<database-name>",
 *     id: "<index-Id>"         // Only id or name may be specified, both is not accepted.
 *     name: "<index-name>"
 * }
 *
 */
class CmdDropSearchIndexCommand final : public TypedCommand<CmdDropSearchIndexCommand> {
public:
    using Request = DropSearchIndexCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to drop a search index. Only supported with Atlas.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        DropSearchIndexReply typedRun(OperationContext* opCtx) {
            throwIfNotRunningWithRemoteSearchIndexManagement();

            const auto& cmd = request();

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot set both 'name' and 'id'.",
                    !(cmd.getName() && cmd.getId()));

            const auto& nss = cmd.getNamespace();

            auto collectionUUID =
                SearchIndexProcessInterface::get(opCtx)->fetchCollectionUUIDOrThrow(opCtx, nss);

            BSONObj manageSearchIndexResponse = getSearchIndexManagerResponse(
                opCtx, nss, collectionUUID, cmd.toBSON(BSONObj() /* commandPassthroughFields */));

            IDLParserContext ctx("DropSearchIndexReply Parser");
            return DropSearchIndexReply::parseOwned(ctx, std::move(manageSearchIndexResponse));
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            const NamespaceString& nss = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call dropSearchIndex on collection "
                                  << nss.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(nss, ActionType::dropSearchIndex));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDropSearchIndexCommand).forShard().forRouter();

/**
 * Passthrough command to the search index management endpoint on which the manageSearchIndex
 * command is called. Accepts requests of the form:
 *
 * {
 *     updateSearchIndex: "<collection name>",
 *     $db: "<database name>",
 *     id: "<index Id>",  // Only id or name may be specified, both is not accepted.
 *     name: "<index name>",
 *     definition: {
 *         // search index definition fields
 *     }
 * }
 *
 */
class CmdUpdateSearchIndexCommand final : public TypedCommand<CmdUpdateSearchIndexCommand> {
public:
    using Request = UpdateSearchIndexCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to update a search index. Only supported with Atlas.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        UpdateSearchIndexReply typedRun(OperationContext* opCtx) {
            throwIfNotRunningWithRemoteSearchIndexManagement();

            const auto& cmd = request();
            cmd.getName();

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot set both 'name' and 'id'.",
                    !(cmd.getName() && cmd.getId()));

            uassert(ErrorCodes::InvalidOptions,
                    "Must set either 'name' or 'id'.",
                    cmd.getName() || cmd.getId());

            const auto& nss = cmd.getNamespace();

            auto collectionUUID =
                SearchIndexProcessInterface::get(opCtx)->fetchCollectionUUIDOrThrow(opCtx, nss);

            BSONObj manageSearchIndexResponse = getSearchIndexManagerResponse(
                opCtx, nss, collectionUUID, cmd.toBSON(BSONObj() /* commandPassthroughFields */));

            IDLParserContext ctx("UpdateSearchIndexReply Parser");
            return UpdateSearchIndexReply::parseOwned(ctx, std::move(manageSearchIndexResponse));
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            const NamespaceString& nss = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call updateSearchIndex on collection "
                                  << nss.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(nss, ActionType::updateSearchIndex));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdUpdateSearchIndexCommand).forShard().forRouter();

/**
 * Passthrough command to the search index management endpoint on which the manageSearchIndex
 * command is called. Accepts requests of the form:
 *
 * {
 *     listSearchIndexes: "<collection-name>",
 *     $db: "<database-name>",
 *     id: "<index-Id>",
 *     name: "<index-name>"
 * }
 *
 * id and name are optional. Both cannot be specified at the same time. If neither of them are
 * specified, then all indexes are returned for the collection.
 *
 * The command returns a 'cursor' field like listIndexes, but the cursorId will always be 0,
 * indicating there is no more data to fetch than that which is returned in the first batch.
 * The response created by the remote search index management host should look something like this:
 *
 * {
 *   ok: 1,
 *   cursor: {
 *     id: Long("0"),
 *     ns: "<database name>.<collection name>",
 *     firstBatch: [
 *       {
 *         id: "<index Id>",
 *         name: "<index name>",
 *         status: "INITIAL SYNC",
 *         definition: {
 *           mappings: {
 *             dynamic: true,
 *           }
 *         }
 *       },
 *       {
 *         id: "<index Id>",
 *         name: "<index name>",
 *         status: "ACTIVE",
 *         definition: {
 *           mappings: {
 *             dynamic: true,
 *           },
 *           synonyms: [<synonym mapping>]
 *         }
 *       }
 *     ]
 *   }
 * }
 *
 */
class CmdListSearchIndexesCommand final : public TypedCommand<CmdListSearchIndexesCommand> {
public:
    using Request = ListSearchIndexesCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to list search indexes. Only supported with Atlas.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        ListSearchIndexesReply typedRun(OperationContext* opCtx) {
            throwIfNotRunningWithRemoteSearchIndexManagement();

            const auto& cmd = request();

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot set both 'name' and 'id'.",
                    !(cmd.getName() && cmd.getId()));

            const auto& nss = cmd.getNamespace();

            auto collectionUUID =
                SearchIndexProcessInterface::get(opCtx)->fetchCollectionUUIDOrThrow(opCtx, nss);

            BSONObj manageSearchIndexResponse = getSearchIndexManagerResponse(
                opCtx, nss, collectionUUID, cmd.toBSON(BSONObj() /* commandPassthroughFields */));

            IDLParserContext ctx("ListSearchIndexesReply Parser");
            return ListSearchIndexesReply::parseOwned(ctx, std::move(manageSearchIndexResponse));
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            const NamespaceString& nss = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call listSearchIndexes on collection "
                                  << nss.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(nss, ActionType::listSearchIndexes));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListSearchIndexesCommand).forShard().forRouter();

}  // namespace
}  // namespace mongo
