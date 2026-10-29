// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_builds_common.h"

#include "mongo/db/database_name.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/storage_engine.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kIndexNameFieldName = "name"sv;
static constexpr std::string_view kUniqueFieldName = "unique"sv;
static constexpr std::string_view kKeyFieldName = "key"sv;

namespace {

bool usesConstraintViolationsTracker(const BSONObj& spec) {
    return spec[kUniqueFieldName].trueValue() ||
        IndexDescriptor::isIdIndexPattern(spec.getObjectField(kKeyFieldName));
}

}  // namespace

IndexBuildInfo::IndexBuildInfo(BSONObj specObj, boost::optional<std::string> idxIdent)
    : spec(std::move(specObj)), indexIdent(idxIdent.value_or("")) {}

IndexBuildInfo::IndexBuildInfo(BSONObj specObj,
                               std::string_view idxIdent,
                               StorageEngine& storageEngine)
    : spec(std::move(specObj)), indexIdent(idxIdent) {
    setInternalIdents(storageEngine);
}

IndexBuildInfo::IndexBuildInfo(BSONObj specObj,
                               StorageEngine& storageEngine,
                               const DatabaseName& dbName)
    : spec(std::move(specObj)), indexIdent(storageEngine.generateNewIndexIdent(dbName)) {
    setInternalIdents(storageEngine);
}

std::string_view IndexBuildInfo::getIndexName() const {
    return spec.getStringField(kIndexNameFieldName);
}

void IndexBuildInfo::setInternalIdents(StorageEngine& storageEngine) {
    setInternalIdents(
        storageEngine.generateNewInternalIndexBuildIdent("sorter", indexIdent),
        storageEngine.generateNewInternalIndexBuildIdent("sideWrites", indexIdent),
        storageEngine.generateNewInternalIndexBuildIdent("skippedRecords", indexIdent),
        usesConstraintViolationsTracker(spec)
            ? boost::make_optional(storageEngine.generateNewInternalIndexBuildIdent(
                  "constraintViolations", indexIdent))
            : boost::none);
}

void IndexBuildInfo::setInternalIdents(boost::optional<std::string> sorterIdent,
                                       boost::optional<std::string> sideWritesIdent,
                                       boost::optional<std::string> skippedRecordsIdent,
                                       boost::optional<std::string> constraintViolationsIdent) {
    this->sorterIdent = std::move(sorterIdent);
    this->sideWritesIdent = std::move(sideWritesIdent);
    this->skippedRecordsIdent = std::move(skippedRecordsIdent);
    this->constraintViolationsIdent = std::move(constraintViolationsIdent);
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
    if (skippedRecordsIdent) {
        bsonObjBuilder.append("skippedRecordsIdent", *skippedRecordsIdent);
    }
    if (constraintViolationsIdent) {
        bsonObjBuilder.append("constraintViolationsIdent", *constraintViolationsIdent);
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
                                                const DatabaseName& dbName) {
    std::vector<IndexBuildInfo> indexes;
    indexes.reserve(specs.size());
    for (const auto& spec : specs) {
        indexes.emplace_back(spec, storageEngine, dbName);
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
