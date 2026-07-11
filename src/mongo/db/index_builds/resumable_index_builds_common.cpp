// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/resumable_index_builds_common.h"

#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::index_builds {

MONGO_FAIL_POINT_DEFINE(failToParseResumeIndexInfo);

boost::optional<ResumeIndexInfo> readAndParseResumeIndexInfo(StorageEngine* engine,
                                                             OperationContext* opCtx,
                                                             const std::string& ident) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    if (!engine->getEngine()->hasIdent(ru, ident)) {
        return boost::none;
    }
    auto rs = engine->getEngine()->getRecordStore(
        opCtx, NamespaceString::kEmpty, ident, RecordStore::Options{}, boost::none /* uuid */);

    auto cursor = rs->getCursor(opCtx, ru);
    auto record = cursor->next();
    if (!record) {
        return boost::none;
    }

    ResumeIndexInfo resumeInfo;
    try {
        if (MONGO_unlikely(failToParseResumeIndexInfo.shouldFail())) {
            uasserted(ErrorCodes::FailPointEnabled,
                      "failToParseResumeIndexInfo fail point is enabled");
        }

        // Peek at the last record. If first.id == last.id, the table has exactly one record that is
        // a complete ResumeIndexInfo. Otherwise, the record is an IndexBuildMetadata and each
        // subsequent record is an IndexStateInfo.
        auto isSingleRecord = [&] {
            auto reverseCursor = rs->getCursor(opCtx, ru, /*forward=*/false);
            auto lastRecord = reverseCursor->next();
            return lastRecord && lastRecord->id == record->id;
        }();

        if (isSingleRecord) {
            resumeInfo =
                ResumeIndexInfo::parse(record->data.toBson(), IDLParserContext("ResumeIndexInfo"));
        } else {
            resumeInfo.setMetadata(IndexBuildMetadata::parse(
                record->data.toBson(), IDLParserContext("IndexBuildMetadata")));
            std::vector<IndexStateInfo> indexes;
            while (auto indexStateInfo = cursor->next()) {
                indexes.emplace_back(IndexStateInfo::parse(indexStateInfo->data.toBson(),
                                                           IDLParserContext("IndexStateInfo")));
            }
            resumeInfo.setIndexes(std::move(indexes));
        }
    } catch (const DBException& e) {
        LOGV2(4916300, "Failed to parse resumable index info", "error"_attr = e.toStatus());

        // Ignore the error so that we can restart the index build instead of resume it. We
        // will drop the internal ident if we failed to parse, either immediately on catalog
        // repair or via abort for primary-driven index builds.
        return boost::none;
    }

    LOGV2(4916301,
          "Found unfinished index build to resume",
          "buildUUID"_attr = resumeInfo.getBuildUUID(),
          "collectionUUID"_attr = resumeInfo.getCollectionUUID(),
          "phase"_attr = idl::serialize(resumeInfo.getPhase()));

    return resumeInfo;
}

ResumeIndexInfo synthesizeResumeIndexInfo(const UUID& buildUUID,
                                          IndexBuildPhaseEnum phase,
                                          const UUID& collectionUUID,
                                          const std::vector<IndexBuildInfo>& indexes) {
    ResumeIndexInfo resumeInfo;
    resumeInfo.setBuildUUID(buildUUID);
    resumeInfo.setPhase(phase);
    resumeInfo.setCollectionUUID(collectionUUID);

    std::vector<IndexStateInfo> indexResumeInfos;
    for (auto&& indexBuildInfo : indexes) {
        uassert(ErrorCodes::BadValue,
                "Failed to synthesize the index build resume state: unknown sideWritesIdent",
                indexBuildInfo.sideWritesIdent);

        IndexStateInfo indexResumeInfo;
        indexResumeInfo.setSpec(indexBuildInfo.spec);
        indexResumeInfo.setIsMultikey(false);
        indexResumeInfo.setMultikeyPaths({});
        indexResumeInfo.setSideWritesTable(*indexBuildInfo.sideWritesIdent);
        indexResumeInfo.setDuplicateKeyTrackerTable(indexBuildInfo.constraintViolationsIdent);
        indexResumeInfo.setSkippedRecordTrackerTable(indexBuildInfo.skippedRecordsIdent);
        indexResumeInfo.setStorageIdentifier(indexBuildInfo.sorterIdent);
        indexResumeInfos.push_back(std::move(indexResumeInfo));
    }
    resumeInfo.setIndexes(std::move(indexResumeInfos));

    LOGV2(12500501,
          "Found unfinished index build to resume (synthesized resume state)",
          "buildUUID"_attr = resumeInfo.getBuildUUID(),
          "collectionUUID"_attr = resumeInfo.getCollectionUUID(),
          "phase"_attr = idl::serialize(resumeInfo.getPhase()));

    return resumeInfo;
}

boost::optional<RecordId> minLastSpilledRecordId(const std::vector<IndexStateInfo>& indexes) {
    boost::optional<RecordId> result;
    for (auto&& index : indexes) {
        auto& lastSpilled = index.getLastSpilledRecordId();
        if (!lastSpilled) {
            return boost::none;
        }
        if (!result || *lastSpilled < *result) {
            result = *lastSpilled;
        }
    }
    return result;
}

}  // namespace mongo::index_builds
