// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>
using namespace std::literals::string_view_literals;

namespace mongo {
namespace express {
using namespace std::literals::string_view_literals;
class IteratorStats {
public:
    const std::string& stageName() const {
        return _stageName;
    }

    long long numKeysExamined() const {
        return _numKeysExamined;
    }

    long long numDocumentsFetched() const {
        return _numDocumentsFetched;
    }

    const std::string& indexName() const {
        return _indexName;
    }

    const BSONObj& indexKeyPattern() const {
        return _indexKeyPattern;
    }

    bool projectionCovered() const {
        return _projectionCovered;
    }

    void setStageName(std::string_view stageName) {
        _stageName = std::string{stageName};
    }

    void incNumKeysExamined(size_t amount) {
        _numKeysExamined += amount;
    }

    void incNumDocumentsFetched(size_t amount) {
        _numDocumentsFetched += amount;
    }

    void setIndexName(std::string_view indexName) {
        _indexName = std::string{indexName};
    }

    void setIndexKeyPattern(const BSONObj& indexKeyPattern) {
        _indexKeyPattern = indexKeyPattern.getOwned();
    }

    void setProjectionCovered(bool projectionCovered) {
        _projectionCovered = projectionCovered;
    }

    void populateSummaryStats(PlanSummaryStats* statsOut) const {
        statsOut->totalKeysExamined = _numKeysExamined;
        statsOut->totalDocsExamined = _numDocumentsFetched;
        if (!_indexName.empty()) {
            statsOut->indexesUsed.emplace(_indexName);
        }
    }

    void appendDataAccessStats(BSONObjBuilder& builder) const {
        if (!_indexKeyPattern.isEmpty()) {
            builder.append("keyPattern"sv, _indexKeyPattern);
        }
        if (!_indexName.empty()) {
            builder.append("indexName"sv, _indexName);
        }
    }

private:
    std::string _stageName;
    size_t _numKeysExamined{0};
    size_t _numDocumentsFetched{0};
    std::string _indexName;
    BSONObj _indexKeyPattern;
    bool _projectionCovered{false};
};

class PlanStats {
public:
    size_t numResults() const {
        return _numResults;
    }

    void incNumResults(size_t amount) {
        _numResults += amount;
    }

private:
    size_t _numResults{0};
};

class WriteOperationStats {
public:
    const std::string& stageName() const {
        return _stageName;
    }

    void setStageName(std::string stageName) {
        _stageName = std::move(stageName);
    }

    size_t docsUpdated() const {
        return _docsUpdated;
    }

    size_t docsMatched() const {
        return _docsMatched;
    }

    bool isModUpdate() const {
        return _isModUpdate;
    }

    void incDocsUpdated(size_t numDocs) {
        _docsUpdated += numDocs;
    }

    void incUpdatedStats(size_t numDocsMatched) {
        _docsMatched += numDocsMatched;
    }

    void setIsModUpdate(bool val) {
        _isModUpdate = val;
    }

    void populateExecStats(BSONObjBuilder& bob) const {
        bob.appendNumber("nWouldModify", static_cast<long long>(docsUpdated()));
        bob.appendNumber("nWouldUpsert", 0LL);
        bob.appendNumber("nWouldDelete", static_cast<long long>(docsDeleted()));
    }

    bool containsDotsAndDollarsField() const {
        return _containsDotsAndDollarsField;
    }

    void setContainsDotsAndDollarsField(bool val) {
        _containsDotsAndDollarsField = val;
    }

    size_t docsDeleted() const {
        return _docsDeleted;
    }

    void incDeletedStats(size_t numDocsDeleted) {
        _docsDeleted += numDocsDeleted;
    }

private:
    std::string _stageName;
    size_t _docsMatched{0};
    size_t _docsUpdated{0};
    size_t _docsDeleted{0};
    bool _isModUpdate{false};
    bool _containsDotsAndDollarsField{false};
};

}  // namespace express

/**
 * A PlanExplainer implementation for express execution plans. Since we don't build a plan tree for
 * these queries, PlanExplainerExpress does not include stage information that is typically included
 * by other PlanExplainers, such as whether or not shard filtering was needed and what index bounds
 * were used. However, it does report the chosen index.
 */
class PlanExplainerExpress final : public PlanExplainer {
public:
    PlanExplainerExpress(const express::PlanStats* planStats,
                         const express::IteratorStats* iteratorStats,
                         const express::WriteOperationStats* writeOperationStats,
                         const CommonStats* commonStats,
                         BSONObj projection)
        : _planStats(planStats),
          _iteratorStats(iteratorStats),
          _writeOperationStats(writeOperationStats),
          _commonStats(commonStats),
          _projection(std::move(projection)) {}

    const ExplainVersion& getVersion() const override {
        static const ExplainVersion kExplainVersion = "1";
        return kExplainVersion;
    }

    bool areThereRejectedPlansToExplain() const override {
        return false;
    }

    std::string getPlanSummary() const override;

    void getSummaryStats(PlanSummaryStats* statsOut) const override;

    PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const override;

    PlanStatsDetails getWinningPlanTrialStats() const override {
        return {};
    }

    std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const override {
        return {};
    }

private:
    const express::PlanStats* _planStats;
    const express::IteratorStats* _iteratorStats;
    const express::WriteOperationStats* _writeOperationStats;
    const CommonStats* _commonStats;
    BSONObj _projection;
};
}  // namespace mongo
