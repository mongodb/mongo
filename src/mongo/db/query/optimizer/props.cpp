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

#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer::properties {

CollationRequirement::CollationRequirement(ProjectionCollationSpec spec) : _spec(std::move(spec)) {
    uassert(6624302, "Empty collation spec", !_spec.empty());

    ProjectionNameSet projections;
    for (const auto& entry : _spec) {
        uassert(6624021, "Repeated projection name", projections.insert(entry.first).second);
    }
}

bool CollationRequirement::operator==(const CollationRequirement& other) const {
    return _spec == other._spec;
}

const ProjectionCollationSpec& CollationRequirement::getCollationSpec() const {
    return _spec;
}

ProjectionCollationSpec& CollationRequirement::getCollationSpec() {
    return _spec;
}

bool CollationRequirement::hasClusteredOp() const {
    for (const auto& [projName, op] : _spec) {
        if (op == CollationOp::Clustered) {
            return true;
        }
    }
    return false;
}

ProjectionNameSet CollationRequirement::getAffectedProjectionNames() const {
    ProjectionNameSet result;
    for (const auto& entry : _spec) {
        result.insert(entry.first);
    }
    return result;
}

LimitSkipRequirement::LimitSkipRequirement(const int64_t limit, const int64_t skip)
    : _limit((limit < 0) ? kMaxVal : limit), _skip(skip) {}

bool LimitSkipRequirement::operator==(const LimitSkipRequirement& other) const {
    return _skip == other._skip && _limit == other._limit;
}

int64_t LimitSkipRequirement::getLimit() const {
    return _limit;
}

int64_t LimitSkipRequirement::getSkip() const {
    return _skip;
}

int64_t LimitSkipRequirement::getAbsoluteLimit() const {
    return hasLimit() ? (_skip + _limit) : kMaxVal;
}

ProjectionNameSet LimitSkipRequirement::getAffectedProjectionNames() const {
    return {};
}

bool LimitSkipRequirement::hasLimit() const {
    return _limit != kMaxVal;
}

ProjectionRequirement::ProjectionRequirement(ProjectionNameOrderPreservingSet projections)
    : _projections(std::move(projections)) {}

bool ProjectionRequirement::operator==(const ProjectionRequirement& other) const {
    return _projections.isEqualIgnoreOrder(other.getProjections());
}

ProjectionNameSet ProjectionRequirement::getAffectedProjectionNames() const {
    ProjectionNameSet result;
    for (const ProjectionName& projection : _projections.getVector()) {
        result.insert(projection);
    }
    return result;
}

const ProjectionNameOrderPreservingSet& ProjectionRequirement::getProjections() const {
    return _projections;
}

ProjectionNameOrderPreservingSet& ProjectionRequirement::getProjections() {
    return _projections;
}

DistributionAndProjections::DistributionAndProjections(DistributionType type)
    : DistributionAndProjections(type, {}) {}

DistributionAndProjections::DistributionAndProjections(DistributionType type,
                                                       ProjectionNameVector projectionNames)
    : _type(type), _projectionNames(std::move(projectionNames)) {
    uassert(6624096,
            "Must have projection names when distributed under hash or range partitioning",
            (_type != DistributionType::HashPartitioning &&
             _type != DistributionType::RangePartitioning) ||
                !_projectionNames.empty());
}

bool DistributionAndProjections::operator==(const DistributionAndProjections& other) const {
    return _type == other._type && _projectionNames == other._projectionNames;
}

DistributionRequirement::DistributionRequirement(
    DistributionAndProjections distributionAndProjections)
    : _distributionAndProjections(std::move(distributionAndProjections)),
      _disableExchanges(false) {}

bool DistributionRequirement::operator==(const DistributionRequirement& other) const {
    return _distributionAndProjections == other._distributionAndProjections &&
        _disableExchanges == other._disableExchanges;
}

const DistributionAndProjections& DistributionRequirement::getDistributionAndProjections() const {
    return _distributionAndProjections;
}

DistributionAndProjections& DistributionRequirement::getDistributionAndProjections() {
    return _distributionAndProjections;
}

ProjectionNameSet DistributionRequirement::getAffectedProjectionNames() const {
    ProjectionNameSet result;
    for (const ProjectionName& projectionName : _distributionAndProjections._projectionNames) {
        result.insert(projectionName);
    }
    return result;
}

bool DistributionRequirement::getDisableExchanges() const {
    return _disableExchanges;
}

void DistributionRequirement::setDisableExchanges(const bool disableExchanges) {
    _disableExchanges = disableExchanges;
}

IndexingRequirement::IndexingRequirement()
    : IndexingRequirement(IndexReqTarget::Complete, true /*dedupRID*/, {}) {}

IndexingRequirement::IndexingRequirement(IndexReqTarget indexReqTarget,
                                         bool dedupRID,
                                         GroupIdType satisfiedPartialIndexesGroupId)
    : _indexReqTarget(indexReqTarget),
      _dedupRID(dedupRID),
      _satisfiedPartialIndexesGroupId(std::move(satisfiedPartialIndexesGroupId)) {
    uassert(6624097,
            "Avoiding dedup is only allowed for Index target",
            _dedupRID || _indexReqTarget == IndexReqTarget::Index);
}

bool IndexingRequirement::operator==(const IndexingRequirement& other) const {
    return _indexReqTarget == other._indexReqTarget && _dedupRID == other._dedupRID &&
        _satisfiedPartialIndexesGroupId == other._satisfiedPartialIndexesGroupId;
}

ProjectionNameSet IndexingRequirement::getAffectedProjectionNames() const {
    // Specifically not returning ridProjectionName (even if present).
    return {};
}

IndexReqTarget IndexingRequirement::getIndexReqTarget() const {
    return _indexReqTarget;
}

bool IndexingRequirement::getDedupRID() const {
    return _dedupRID;
}

void IndexingRequirement::setDedupRID(const bool value) {
    _dedupRID = value;
}

GroupIdType IndexingRequirement::getSatisfiedPartialIndexesGroupId() const {
    return _satisfiedPartialIndexesGroupId;
}

RepetitionEstimate::RepetitionEstimate(const CEType estimate) : _estimate(estimate) {}

bool RepetitionEstimate::operator==(const RepetitionEstimate& other) const {
    return _estimate == other._estimate;
}

ProjectionNameSet RepetitionEstimate::getAffectedProjectionNames() const {
    return {};
}

CEType RepetitionEstimate::getEstimate() const {
    return _estimate;
}

LimitEstimate::LimitEstimate(const CEType estimate) : _estimate(estimate) {}

bool LimitEstimate::operator==(const LimitEstimate& other) const {
    return _estimate == other._estimate;
}

ProjectionNameSet LimitEstimate::getAffectedProjectionNames() const {
    return {};
}

bool LimitEstimate::hasLimit() const {
    return _estimate >= 0.0;
}

CEType LimitEstimate::getEstimate() const {
    return _estimate;
}

ProjectionAvailability::ProjectionAvailability(ProjectionNameSet projections)
    : _projections(std::move(projections)) {}

bool ProjectionAvailability::operator==(const ProjectionAvailability& other) const {
    return _projections == other._projections;
}

const ProjectionNameSet& ProjectionAvailability::getProjections() const {
    return _projections;
}

CardinalityEstimate::CardinalityEstimate(const CEType estimate)
    : _estimate(estimate), _partialSchemaKeyCE() {}

bool CardinalityEstimate::operator==(const CardinalityEstimate& other) const {
    return _estimate == other._estimate && _partialSchemaKeyCE == other._partialSchemaKeyCE;
}

CEType CardinalityEstimate::getEstimate() const {
    return _estimate;
}

CEType& CardinalityEstimate::getEstimate() {
    return _estimate;
}

const PartialSchemaKeyCE& CardinalityEstimate::getPartialSchemaKeyCE() const {
    return _partialSchemaKeyCE;
}

PartialSchemaKeyCE& CardinalityEstimate::getPartialSchemaKeyCE() {
    return _partialSchemaKeyCE;
}

IndexingAvailability::IndexingAvailability(GroupIdType scanGroupId,
                                           ProjectionName scanProjection,
                                           std::string scanDefName,
                                           const bool eqPredsOnly,
                                           opt::unordered_set<std::string> satisfiedPartialIndexes)
    : _scanGroupId(scanGroupId),
      _scanProjection(std::move(scanProjection)),
      _scanDefName(std::move(scanDefName)),
      _eqPredsOnly(eqPredsOnly),
      _satisfiedPartialIndexes(std::move(satisfiedPartialIndexes)) {}

bool IndexingAvailability::operator==(const IndexingAvailability& other) const {
    return _scanGroupId == other._scanGroupId && _scanProjection == other._scanProjection &&
        _scanDefName == other._scanDefName && _eqPredsOnly == other._eqPredsOnly &&
        _satisfiedPartialIndexes == other._satisfiedPartialIndexes;
}

GroupIdType IndexingAvailability::getScanGroupId() const {
    return _scanGroupId;
}

void IndexingAvailability::setScanGroupId(const GroupIdType scanGroupId) {
    _scanGroupId = scanGroupId;
}

const ProjectionName& IndexingAvailability::getScanProjection() const {
    return _scanProjection;
}

const std::string& IndexingAvailability::getScanDefName() const {
    return _scanDefName;
}

const opt::unordered_set<std::string>& IndexingAvailability::getSatisfiedPartialIndexes() const {
    return _satisfiedPartialIndexes;
}

opt::unordered_set<std::string>& IndexingAvailability::getSatisfiedPartialIndexes() {
    return _satisfiedPartialIndexes;
}

bool IndexingAvailability::getEqPredsOnly() const {
    return _eqPredsOnly;
}

void IndexingAvailability::setEqPredsOnly(const bool value) {
    _eqPredsOnly = value;
}

CollectionAvailability::CollectionAvailability(opt::unordered_set<std::string> scanDefSet)
    : _scanDefSet(std::move(scanDefSet)) {}

bool CollectionAvailability::operator==(const CollectionAvailability& other) const {
    return _scanDefSet == other._scanDefSet;
}

const opt::unordered_set<std::string>& CollectionAvailability::getScanDefSet() const {
    return _scanDefSet;
}

opt::unordered_set<std::string>& CollectionAvailability::getScanDefSet() {
    return _scanDefSet;
}

size_t DistributionHash::operator()(
    const DistributionAndProjections& distributionAndProjections) const {
    size_t result = 0;
    updateHash(result, std::hash<DistributionType>()(distributionAndProjections._type));
    for (const ProjectionName& projectionName : distributionAndProjections._projectionNames) {
        updateHash(result, std::hash<ProjectionName>()(projectionName));
    }
    return result;
}

DistributionAvailability::DistributionAvailability(DistributionSet distributionSet)
    : _distributionSet(std::move(distributionSet)) {}

bool DistributionAvailability::operator==(const DistributionAvailability& other) const {
    return _distributionSet == other._distributionSet;
}

const DistributionSet& DistributionAvailability::getDistributionSet() const {
    return _distributionSet;
}

DistributionSet& DistributionAvailability::getDistributionSet() {
    return _distributionSet;
}

}  // namespace mongo::optimizer::properties
