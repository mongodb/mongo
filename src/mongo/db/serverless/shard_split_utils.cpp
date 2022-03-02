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

#include "mongo/db/serverless/shard_split_utils.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/repl_set_config.h"

namespace mongo {

namespace serverless {
std::vector<repl::MemberConfig> getRecipientMembers(const repl::ReplSetConfig& config,
                                                    const StringData& recipientTagName) {
    std::vector<repl::MemberConfig> result;
    const auto& tagConfig = config.getTagConfig();
    for (auto member : config.members()) {
        auto matchesTag =
            std::any_of(member.tagsBegin(), member.tagsEnd(), [&](const repl::ReplSetTag& tag) {
                return tagConfig.getTagKey(tag) == recipientTagName;
            });

        if (matchesTag) {
            result.emplace_back(member);
        }
    }

    return result;
}


ConnectionString makeRecipientConnectionString(const repl::ReplSetConfig& config,
                                               const StringData& recipientTagName,
                                               const StringData& recipientSetName) {
    auto recipientMembers = getRecipientMembers(config, recipientTagName);
    std::vector<HostAndPort> recipientNodes;
    std::transform(recipientMembers.cbegin(),
                   recipientMembers.cend(),
                   std::back_inserter(recipientNodes),
                   [](const repl::MemberConfig& member) { return member.getHostAndPort(); });

    return ConnectionString::forReplicaSet(recipientSetName.toString(), recipientNodes);
}

repl::ReplSetConfig makeSplitConfig(const repl::ReplSetConfig& config,
                                    const std::string& recipientSetName,
                                    const std::string& recipientTagName) {
    dassert(!recipientSetName.empty() && recipientSetName != config.getReplSetName());
    uassert(6201800,
            "We can not make a split config of an existing split config.",
            !config.isSplitConfig());

    const auto& tagConfig = config.getTagConfig();
    std::vector<BSONObj> recipientMembers, donorMembers;
    int donorIndex = 0, recipientIndex = 0;
    for (const auto& member : config.members()) {
        bool isRecipient =
            std::any_of(member.tagsBegin(), member.tagsEnd(), [&](const repl::ReplSetTag& tag) {
                return tagConfig.getTagKey(tag) == recipientTagName;
            });
        if (isRecipient) {
            BSONObjBuilder bob(
                member.toBSON().removeField("votes").removeField("priority").removeField("_id"));
            bob.appendNumber("_id", recipientIndex);
            recipientMembers.push_back(bob.obj());
            recipientIndex++;
        } else {
            BSONObjBuilder bob(member.toBSON().removeField("_id"));
            bob.appendNumber("_id", donorIndex);
            donorMembers.push_back(bob.obj());
            donorIndex++;
        }
    }

    uassert(6201801, "No recipient members found for split config.", !recipientMembers.empty());
    uassert(6201802, "No donor members found for split config.", !donorMembers.empty());

    const auto updatedVersion = config.getConfigVersion() + 1;
    const auto configNoMembersBson = config.toBSON().removeField("members").removeField("version");

    BSONObjBuilder recipientConfigBob(
        configNoMembersBson.removeField("_id").removeField("settings"));
    recipientConfigBob.append("_id", recipientSetName)
        .append("members", recipientMembers)
        .append("version", updatedVersion);
    if (configNoMembersBson.hasField("settings") &&
        configNoMembersBson.getField("settings").isABSONObj()) {
        BSONObj settings = configNoMembersBson.getField("settings").Obj();
        if (settings.hasField("replicaSetId")) {
            recipientConfigBob.append("settings", settings.removeField("replicaSetId"));
        }
    }

    BSONObjBuilder splitConfigBob(configNoMembersBson);
    splitConfigBob.append("version", updatedVersion);
    splitConfigBob.append("members", donorMembers);
    splitConfigBob.append("recipientConfig", recipientConfigBob.obj());

    return repl::ReplSetConfig::parse(splitConfigBob.obj());
}

Status insertStateDoc(OperationContext* opCtx, const ShardSplitDonorDocument& stateDoc) {
    const auto nss = NamespaceString::kTenantSplitDonorsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "No longer primary while attempting to insert shard split"
                             " state document",
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    return writeConflictRetry(opCtx, "insertShardSplitStateDoc", nss.ns(), [&]() -> Status {
        const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName
                                 << stateDoc.getId() << ShardSplitDonorDocument::kExpireAtFieldName
                                 << BSON("$exists" << false));
        const auto updateMod = BSON("$setOnInsert" << stateDoc.toBSON());
        auto updateResult =
            Helpers::upsert(opCtx, nss.ns(), filter, updateMod, /*fromMigrate=*/false);

        invariant(!updateResult.numDocsModified);
        if (updateResult.upsertedId.isEmpty()) {
            return {ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Failed to insert the shard split state doc: "
                                  << stateDoc.toBSON()};
        }
        return Status::OK();
    });
}

Status updateStateDoc(OperationContext* opCtx, const ShardSplitDonorDocument& stateDoc) {
    const auto nss = NamespaceString::kTenantSplitDonorsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << nss.ns() << " does not exist");
    }

    return writeConflictRetry(opCtx, "updateShardSplitStateDoc", nss.ns(), [&]() -> Status {
        auto updateResult =
            Helpers::upsert(opCtx, nss.ns(), stateDoc.toBSON(), /*fromMigrate=*/false);
        if (updateResult.numMatched == 0) {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "Existing shard split state document not found for id: "
                                  << stateDoc.getId()};
        }

        return Status::OK();
    });
}

StatusWith<ShardSplitDonorDocument> getStateDocument(OperationContext* opCtx,
                                                     const UUID& shardSplitId) {
    // Read the most up to date data.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);
    AutoGetCollectionForRead collection(opCtx, NamespaceString::kTenantSplitDonorsNamespace);
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection not found looking for state document: "
                                    << NamespaceString::kTenantSplitDonorsNamespace.ns());
    }

    BSONObj result;
    auto foundDoc = Helpers::findOne(
        opCtx, collection.getCollection(), BSON("_id" << shardSplitId), result, true);

    if (!foundDoc) {
        return Status(ErrorCodes::NoMatchingDocument,
                      str::stream()
                          << "No matching state doc found with shard split id: " << shardSplitId);
    }

    try {
        return ShardSplitDonorDocument::parse(IDLParserErrorContext("shardSplitStateDocument"),
                                              result);
    } catch (DBException& ex) {
        return ex.toStatus(str::stream()
                           << "Invalid BSON found for matching document with shard split id: "
                           << shardSplitId << " , res: " << result);
    }
}


}  // namespace serverless
}  // namespace mongo
