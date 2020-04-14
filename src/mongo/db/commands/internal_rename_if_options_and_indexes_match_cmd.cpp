/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/commands/internal_rename_if_options_and_indexes_match_gen.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"

namespace mongo {

/**
 * Rename a collection while checking collection option and indexes.
 */
class InternalRenameIfOptionsAndIndexesMatchCmd final
    : public TypedCommand<InternalRenameIfOptionsAndIndexesMatchCmd> {
public:
    using Request = InternalRenameIfOptionsAndIndexesMatch;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto thisRequest = request();
            auto originalIndexes = thisRequest.getIndexes();
            auto indexList = std::list<BSONObj>(originalIndexes.begin(), originalIndexes.end());
            RenameCollectionOptions options;
            options.dropTarget = true;
            options.stayTemp = false;
            options.skipSourceCollectionShardedCheck = true;
            doLocalRenameIfOptionsAndIndexesHaveNotChanged(opCtx,
                                                           thisRequest.getFrom(),
                                                           thisRequest.getTo(),
                                                           options,
                                                           std::move(indexList),
                                                           thisRequest.getCollectionOptions());
        }

    private:
        NamespaceString ns() const override {
            return request().getFrom();
        }
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto thisRequest = request();
            auto from = thisRequest.getFrom();
            auto to = thisRequest.getTo();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to rename " << from,
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(from),
                                                           ActionType::renameCollection));
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to drop " << to,
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(to),
                                                           ActionType::dropCollection));
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to insert into " << to,
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(to),
                                                           ActionType::insert));
        }
    };

    std::string help() const override {
        return "Internal command to rename and check collection options";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

} internalRenameIfOptionsAndIndexesMatchCmd;
}  // namespace mongo
