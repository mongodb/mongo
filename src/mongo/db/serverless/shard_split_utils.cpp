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
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/logv2/log_debug.h"

namespace mongo {

namespace serverless {

const size_t kMinimumRequiredRecipientNodes = 3;

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

    uassert(ErrorCodes::BadValue,
            "The recipient connection string must have at least three members.",
            recipientNodes.size() >= kMinimumRequiredRecipientNodes);

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
            auto memberBSON = member.toBSON();
            auto recipientTags = memberBSON.getField("tags").Obj().removeField(recipientTagName);
            BSONObjBuilder bob(memberBSON.removeFields(
                StringDataSet{"votes", "priority", "_id", "tags", "hidden"}));

            bob.appendNumber("_id", recipientIndex);
            bob.append("tags", recipientTags);
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
            recipientConfigBob.append(
                "settings",
                settings.removeField("replicaSetId").addFields(BSON("replicaSetId" << OID::gen())));
        }
    }

    BSONObjBuilder splitConfigBob(configNoMembersBson);
    splitConfigBob.append("version", updatedVersion);
    splitConfigBob.append("members", donorMembers);
    splitConfigBob.append("recipientConfig", recipientConfigBob.obj());

    auto finalConfig = repl::ReplSetConfig::parse(splitConfigBob.obj());

    uassert(ErrorCodes::InvalidReplicaSetConfig,
            "Recipient config and top level config cannot share the same replicaSetId",
            finalConfig.getReplicaSetId() != finalConfig.getRecipientConfig()->getReplicaSetId());

    return finalConfig;
}

Status insertStateDoc(OperationContext* opCtx, const ShardSplitDonorDocument& stateDoc) {
    const auto nss = NamespaceString::kShardSplitDonorsNamespace;
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
        auto updateResult = Helpers::upsert(opCtx, nss, filter, updateMod, /*fromMigrate=*/false);

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
    const auto nss = NamespaceString::kShardSplitDonorsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << nss.ns() << " does not exist");
    }

    return writeConflictRetry(opCtx, "updateShardSplitStateDoc", nss.ns(), [&]() -> Status {
        auto updateResult = Helpers::upsert(opCtx, nss, stateDoc.toBSON(), /*fromMigrate=*/false);
        if (updateResult.numMatched == 0) {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "Existing shard split state document not found for id: "
                                  << stateDoc.getId()};
        }

        return Status::OK();
    });
}

StatusWith<bool> deleteStateDoc(OperationContext* opCtx, const UUID& shardSplitId) {
    const auto nss = NamespaceString::kShardSplitDonorsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << nss.ns() << " does not exist");
    }
    auto query = BSON(ShardSplitDonorDocument::kIdFieldName << shardSplitId);
    return writeConflictRetry(opCtx, "ShardSplitDonorDeleteStateDoc", nss.ns(), [&]() -> bool {
        auto nDeleted =
            deleteObjects(opCtx, collection.getCollection(), nss, query, true /* justOne */);
        return nDeleted > 0;
    });
}

bool shouldRemoveStateDocumentOnRecipient(OperationContext* opCtx,
                                          const ShardSplitDonorDocument& stateDocument) {
    if (!stateDocument.getRecipientSetName()) {
        return false;
    }
    auto recipientSetName = *stateDocument.getRecipientSetName();
    auto config = repl::ReplicationCoordinator::get(cc().getServiceContext())->getConfig();
    return recipientSetName == config.getReplSetName() &&
        stateDocument.getState() >= ShardSplitDonorStateEnum::kBlocking;
}

Status validateRecipientNodesForShardSplit(const ShardSplitDonorDocument& stateDocument,
                                           const repl::ReplSetConfig& localConfig) {
    if (stateDocument.getState() > ShardSplitDonorStateEnum::kUninitialized) {
        return Status::OK();
    }

    auto recipientSetName = stateDocument.getRecipientSetName();
    auto recipientTagName = stateDocument.getRecipientTagName();
    uassert(6395901, "Missing recipientTagName when validating recipient nodes.", recipientTagName);
    uassert(6395902, "Missing recipientSetName when validating recipient nodes.", recipientSetName);

    if (*recipientSetName == localConfig.getReplSetName()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Recipient set name '" << *recipientSetName << "' and local set name '"
                          << localConfig.getReplSetName() << "' must be different.");
    }

    auto recipientNodes = getRecipientMembers(localConfig, *recipientTagName);
    if (recipientNodes.size() < kMinimumRequiredRecipientNodes) {
        return Status(ErrorCodes::InvalidReplicaSetConfig,
                      str::stream() << "Local set config has " << recipientNodes.size()
                                    << " nodes when it requires at least "
                                    << kMinimumRequiredRecipientNodes << " in its config.");
    }

    stdx::unordered_set<std::string> uniqueTagValues;
    const auto& tagConfig = localConfig.getTagConfig();
    for (auto member : recipientNodes) {
        for (repl::MemberConfig::TagIterator it = member.tagsBegin(); it != member.tagsEnd();
             ++it) {
            if (tagConfig.getTagKey(*it) == *recipientTagName) {
                auto tagValue = tagConfig.getTagValue(*it);
                if (!uniqueTagValues.insert(tagValue).second) {
                    return Status(ErrorCodes::InvalidOptions,
                                  str::stream() << "Local member '" << member.getId().toString()
                                                << "' does not have a unique value for the tag '"
                                                << *recipientTagName << ". Current value is '"
                                                << tagValue << "'.");
                }
            }
        }
    }

    const bool allRecipientNodesNonVoting =
        std::none_of(recipientNodes.cbegin(), recipientNodes.cend(), [&](const auto& member) {
            return member.isVoter() || member.getPriority() != 0;
        });

    if (!allRecipientNodesNonVoting) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Local members tagged with '" << *recipientTagName
                                    << "' must be non-voting and with a priority set to 0.");
    }

    const bool allHiddenRecipientNodes =
        std::all_of(recipientNodes.cbegin(), recipientNodes.cend(), [&](const auto& member) {
            return member.isHidden();
        });
    if (!allHiddenRecipientNodes) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Local members tagged with '" << *recipientTagName
                                    << "' must be hidden.");
    }

    return Status::OK();
}

RecipientAcceptSplitListener::RecipientAcceptSplitListener(
    const ConnectionString& recipientConnectionString)
    : _numberOfRecipient(recipientConnectionString.getServers().size()),
      _recipientSetName(recipientConnectionString.getSetName()) {}

const std::string kSetNameFieldName = "setName";
const std::string kLastWriteFieldName = "lastWrite";
const std::string kLastWriteOpTimeFieldName = "opTime";
void RecipientAcceptSplitListener::onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
                                                                   const BSONObj reply) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_fulfilled || !reply.hasField(kSetNameFieldName)) {
        return;
    }

    auto lastWriteOpTime = [&]() {
        if (reply.hasField(kLastWriteFieldName)) {
            auto lastWriteObj = reply[kLastWriteFieldName].Obj();
            auto swLastWriteOpTime =
                repl::OpTime::parseFromOplogEntry(lastWriteObj[kLastWriteOpTimeFieldName].Obj());
            if (swLastWriteOpTime.isOK()) {
                return swLastWriteOpTime.getValue();
            }
        }

        if (_reportedSetNames.contains(hostAndPort)) {
            return _reportedSetNames[hostAndPort].opTime;
        }

        return repl::OpTime();
    }();

    _reportedSetNames[hostAndPort] =
        repl::OpTimeWith<std::string>(reply["setName"].str(), lastWriteOpTime);
    auto allReportCorrectly = std::all_of(_reportedSetNames.begin(),
                                          _reportedSetNames.end(),
                                          [&](const auto& entry) {
                                              return !entry.second.opTime.isNull() &&
                                                  entry.second.value == _recipientSetName;
                                          }) &&
        _reportedSetNames.size() == _numberOfRecipient;

    if (allReportCorrectly) {
        _fulfilled = true;
        auto highestLastApplied = std::max_element(
            _reportedSetNames.begin(), _reportedSetNames.end(), [](const auto& p1, const auto& p2) {
                return p1.second.opTime < p2.second.opTime;
            });

        _promise.emplaceValue(highestLastApplied->first);
    }
}

SharedSemiFuture<HostAndPort> RecipientAcceptSplitListener::getSplitAcceptedFuture() const {
    return _promise.getFuture();
}

}  // namespace serverless
}  // namespace mongo
