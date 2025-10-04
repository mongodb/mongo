/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/index_builds/index_builds_common.h"

#include "mongo/db/database_name.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo {
namespace {
bool isPrimaryDrivenIndexBuildEnabled(const VersionContext& vCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(vCtx, fcvSnapshot)) {
        return true;
    }
    return false;
}
}  // namespace

static constexpr StringData kIndexNameFieldName = "name"_sd;

IndexBuildInfo::IndexBuildInfo(BSONObj specObj, boost::optional<std::string> idxIdent)
    : spec(std::move(specObj)), indexIdent(idxIdent.value_or("")) {}

IndexBuildInfo::IndexBuildInfo(BSONObj specObj,
                               StorageEngine& storageEngine,
                               const DatabaseName& dbName,
                               const VersionContext& vCtx)
    : spec(std::move(specObj)), indexIdent(storageEngine.generateNewIndexIdent(dbName)) {
    setInternalIdents(storageEngine, vCtx);
}

StringData IndexBuildInfo::getIndexName() const {
    return spec.getStringField(kIndexNameFieldName);
}

void IndexBuildInfo::setInternalIdents(StorageEngine& storageEngine, const VersionContext& vCtx) {
    // TODO SERVER-110289: Check whether index build method is primary driven rather just the
    // featureFlag
    if (isPrimaryDrivenIndexBuildEnabled(vCtx)) {
        setInternalIdents(
            storageEngine.generateNewInternalIndexBuildIdent("sorter", indexIdent),
            storageEngine.generateNewInternalIndexBuildIdent("sideWrites", indexIdent),
            storageEngine.generateNewInternalIndexBuildIdent("skippedRecordsTracker", indexIdent),
            storageEngine.generateNewInternalIndexBuildIdent("constraintViolations", indexIdent));
        return;
    }
    setInternalIdents(storageEngine.generateNewInternalIdent(),
                      storageEngine.generateNewInternalIdent(),
                      storageEngine.generateNewInternalIdent(),
                      storageEngine.generateNewInternalIdent());
}

void IndexBuildInfo::setInternalIdents(
    boost::optional<std::string> sorterIdent,
    boost::optional<std::string> sideWritesIdent,
    boost::optional<std::string> skippedRecordsTrackerIdent,
    boost::optional<std::string> constraintViolationsTrackerIdent) {
    this->sorterIdent = std::move(sorterIdent);
    this->sideWritesIdent = std::move(sideWritesIdent);
    this->skippedRecordsTrackerIdent = std::move(skippedRecordsTrackerIdent);
    this->constraintViolationsTrackerIdent = std::move(constraintViolationsTrackerIdent);
}

BSONObj IndexBuildInfo::toBSON() const {
    BSONObjBuilder bsonObjBuilder;
    bsonObjBuilder.append("name", getIndexName());
    if (!indexIdent.empty()) {
        bsonObjBuilder.append("indexIdent", indexIdent);
    }
    if (sorterIdent) {
        bsonObjBuilder.append("sorterIdent", *sorterIdent);
    }
    if (sideWritesIdent) {
        bsonObjBuilder.append("sideWritesIdent", *sideWritesIdent);
    }
    if (skippedRecordsTrackerIdent) {
        bsonObjBuilder.append("skippedRecordsTrackerIdent", *skippedRecordsTrackerIdent);
    }
    if (constraintViolationsTrackerIdent) {
        bsonObjBuilder.append("constraintViolationsTrackerIdent",
                              *constraintViolationsTrackerIdent);
    }
    return bsonObjBuilder.obj();
}

std::vector<IndexBuildInfo> toIndexBuildInfoVec(const std::vector<BSONObj>& specs) {
    std::vector<IndexBuildInfo> indexes;
    indexes.reserve(specs.size());
    for (const auto& spec : specs) {
        indexes.emplace_back(spec, boost::none);
    }
    return indexes;
}

std::vector<IndexBuildInfo> toIndexBuildInfoVec(const std::vector<BSONObj>& specs,
                                                StorageEngine& storageEngine,
                                                const DatabaseName& dbName,
                                                const VersionContext& vCtx) {
    std::vector<IndexBuildInfo> indexes;
    indexes.reserve(specs.size());
    for (const auto& spec : specs) {
        indexes.emplace_back(spec, storageEngine, dbName, vCtx);
    }
    return indexes;
}

std::vector<std::string> toIndexNames(const std::vector<IndexBuildInfo>& indexes) {
    std::vector<std::string> indexNames;
    indexNames.reserve(indexes.size());
    for (const auto& indexBuildInfo : indexes) {
        indexNames.push_back(std::string{indexBuildInfo.getIndexName()});
    }
    return indexNames;
}

std::vector<BSONObj> toIndexSpecs(const std::vector<IndexBuildInfo>& indexes) {
    std::vector<BSONObj> indexSpecs;
    indexSpecs.reserve(indexes.size());
    for (const auto& indexBuildInfo : indexes) {
        indexSpecs.push_back(indexBuildInfo.spec);
    }
    return indexSpecs;
}

}  // namespace mongo
