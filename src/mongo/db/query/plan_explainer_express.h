/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <fmt/format.h>

namespace mongo {
namespace express {
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

    const std::string& indexKeyPattern() const {
        return _indexKeyPattern;
    }

    bool projectionCovered() const {
        return _projectionCovered;
    }

    void setStageName(StringData stageName) {
        _stageName = std::string{stageName};
    }

    void incNumKeysExamined(size_t amount) {
        _numKeysExamined += amount;
    }

    void incNumDocumentsFetched(size_t amount) {
        _numDocumentsFetched += amount;
    }

    void setIndexName(StringData indexName) {
        _indexName = std::string{indexName};
    }

    void setIndexKeyPattern(StringData indexKeyPattern) {
        _indexKeyPattern = std::string{indexKeyPattern};
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
        if (!_indexKeyPattern.empty()) {
            builder.append("keyPattern"_sd, _indexKeyPattern);
        }
        if (!_indexName.empty()) {
            builder.append("indexName"_sd, _indexName);
        }
    }

private:
    std::string _stageName;
    size_t _numKeysExamined{0};
    size_t _numDocumentsFetched{0};
    std::string _indexName;
    std::string _indexKeyPattern;
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
                         BSONObj projection)
        : _planStats(planStats),
          _iteratorStats(iteratorStats),
          _writeOperationStats(writeOperationStats),
          _projection(std::move(projection)) {}

    const ExplainVersion& getVersion() const override {
        static const ExplainVersion kExplainVersion = "1";
        return kExplainVersion;
    }

    bool isMultiPlan() const override {
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
    BSONObj _projection;
};
}  // namespace mongo
