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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_index.h"
#include "mongo/db/s/global_index_crud_commands_gen.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo {
namespace {

class ShardsvrWriteGlobalIndexKeysCmd final : public TypedCommand<ShardsvrWriteGlobalIndexKeysCmd> {
public:
    using Request = WriteGlobalIndexKeys;

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx);

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return {request().getDbName(), ""};
        }
    };

    std::string help() const override {
        return "Internal command to perform multiple global index key writes in bulk.";
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};

void ShardsvrWriteGlobalIndexKeysCmd::Invocation::typedRun(OperationContext* opCtx) {
    uassert(ErrorCodes::CommandNotSupported,
            "Global indexes are not enabled.",
            gFeatureFlagGlobalIndexes.isEnabled(serverGlobalParams.featureCompatibility));

    uassert(6789500,
            "_shardsvrWriteGlobalIndexKeys must run inside a multi-doc transaction.",
            opCtx->inMultiDocumentTransaction());

    const auto& ops = request().getOps();

    uassert(6789502, "_shardsvrWriteGlobalIndexKeys 'ops' field cannot be empty", ops.size());

    for (const auto& op : ops) {
        const auto cmd = op.firstElementFieldNameStringData();
        uassert(6789501,
                str::stream() << "_shardsvrWriteGlobalIndexKeys ops entries must be of type "
                              << InsertGlobalIndexKey::kCommandParameterFieldName << " or "
                              << DeleteGlobalIndexKey::kCommandParameterFieldName,
                cmd == InsertGlobalIndexKey::kCommandParameterFieldName ||
                    cmd == DeleteGlobalIndexKey::kCommandParameterFieldName);

        const auto uuid = uassertStatusOK(UUID::parse(op[cmd]));
        const auto key = GlobalIndexKeyEntry::parse(IDLParserContext{"GlobalIndexKeyEntry"}, op);
        if (cmd == InsertGlobalIndexKey::kCommandParameterFieldName) {
            global_index::insertKey(opCtx, uuid, key.getKey(), key.getDocKey());
        } else {
            global_index::deleteKey(opCtx, uuid, key.getKey(), key.getDocKey());
        }
    }
}

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ShardsvrWriteGlobalIndexKeysCmd,
                                       mongo::gFeatureFlagGlobalIndexes);

}  // namespace
}  // namespace mongo
