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


#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/crypto/fle_stats.h"
#include "mongo/db/auth/authorization_session.h"

#include "mongo/db/commands/fle2_cleanup_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

CleanupStats cleanupEncryptedCollection(OperationContext* opCtx,
                                        const CleanupStructuredEncryptionData& request) {
    uasserted(7293700, "Function not implemented.");
}

class CleanupStructuredEncryptionDataCmd final
    : public TypedCommand<CleanupStructuredEncryptionDataCmd> {
public:
    using Request = CleanupStructuredEncryptionData;
    using Reply = CleanupStructuredEncryptionData::Reply;
    using TC = TypedCommand<CleanupStructuredEncryptionDataCmd>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            return Reply(cleanupEncryptedCollection(opCtx, request()));
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to cleanup structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::cleanupStructuredEncryptionData));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {CleanupStructuredEncryptionData::kCleanupTokensFieldName};
    }
} cleanupStructuredEncryptionDataCmd;


}  // namespace

}  // namespace mongo
