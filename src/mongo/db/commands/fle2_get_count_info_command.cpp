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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fle2_get_count_info_command_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

std::vector<std::vector<FLEEdgePrfBlock>> toNestedTokens(
    const std::vector<mongo::QECountInfoRequestTokenSet>& tagSets) {

    std::vector<std::vector<FLEEdgePrfBlock>> nestedBlocks;
    nestedBlocks.reserve(tagSets.size());

    for (const auto& tagset : tagSets) {
        std::vector<FLEEdgePrfBlock> blocks;

        const auto& tags = tagset.getTokens();

        blocks.reserve(tags.size());

        for (auto& tag : tags) {
            blocks.emplace_back();
            auto& block = blocks.back();

            block.esc = PrfBlockfromCDR(tag.getESCDerivedFromDataTokenAndContentionFactorToken());
            block.edc =
                tag.getEDCDerivedFromDataTokenAndContentionFactorToken().map(PrfBlockfromCDR);
        }

        nestedBlocks.emplace_back(std::move(blocks));
    }

    return nestedBlocks;
}

std::vector<QECountInfoReplyTokenSet> toGetTagRequestTupleSet(
    const std::vector<std::vector<FLEEdgeCountInfo>>& countInfoSets) {

    std::vector<QECountInfoReplyTokenSet> nestedBlocks;
    nestedBlocks.reserve(countInfoSets.size());

    for (const auto& countInfos : countInfoSets) {
        std::vector<QECountInfoReplyTokens> tokens;

        tokens.reserve(countInfos.size());

        for (auto& countInfo : countInfos) {
            tokens.emplace_back(FLEUtil::vectorFromCDR(countInfo.tagToken.toCDR()),
                                countInfo.count);

            if (countInfo.edc.has_value()) {
                auto& replyTuple = tokens.back();
                replyTuple.setEDCDerivedFromDataTokenAndContentionFactorToken(
                    countInfo.edc.value().toCDR());
            }
        }

        nestedBlocks.emplace_back(std::move(tokens));
    }

    return nestedBlocks;
}

QECountInfosReply getTagsLocal(OperationContext* opCtx,
                               const GetQueryableEncryptionCountInfo& request) {

    CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

    auto nestedTokens = toNestedTokens(request.getTokens());

    auto countInfoSets =
        getTagsFromStorage(opCtx,
                           request.getNamespace(),
                           nestedTokens,
                           request.getForInsert() ? FLETagQueryInterface::TagQueryType::kInsert
                                                  : FLETagQueryInterface::TagQueryType::kQuery);

    QECountInfosReply reply;
    reply.setCounts(toGetTagRequestTupleSet(countInfoSets));

    return reply;
}

/**
 * Retrieve a set of tags from ESC. Returns a count suitable for either insert or query.
 */
class GetQueryableEncryptionCountInfoCmd final
    : public TypedCommand<GetQueryableEncryptionCountInfoCmd> {
public:
    using Request = GetQueryableEncryptionCountInfo;
    using Reply = GetQueryableEncryptionCountInfo::Reply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            return Reply(getTagsLocal(opCtx, request()));
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to read tags",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                         ActionType::internal));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        // Restrict to primary for now to allow future possibilities of caching on primary
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {GetQueryableEncryptionCountInfo::kTokensFieldName};
    }
} getQueryableEncryptionCountInfoCmd;

}  // namespace
}  // namespace mongo
