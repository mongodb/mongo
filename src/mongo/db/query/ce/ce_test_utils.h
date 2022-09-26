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

#pragma once

#include <cstddef>
#include <sys/types.h>

#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/ce/scalar_histogram.h"
#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"

namespace mongo {

namespace optimizer {
namespace cascades {

// Forward declaration.
class CEInterface;

}  // namespace cascades
}  // namespace optimizer

namespace ce {

using namespace optimizer;
using namespace sbe;

// Enable this flag to log all estimates, and let all tests pass.
constexpr bool kCETestLogOnly = false;

const double kMaxCEError = 0.01;
const CEType kInvalidCardinality = -1.0;

const OptPhaseManager::PhaseSet kDefaultCETestPhaseSet{OptPhase::MemoSubstitutionPhase,
                                                       OptPhase::MemoExplorationPhase,
                                                       OptPhase::MemoImplementationPhase};

const OptPhaseManager::PhaseSet kOnlySubPhaseSet{OptPhase::MemoSubstitutionPhase};

const OptPhaseManager::PhaseSet kNoOptPhaseSet{};

/**
 * Helpful macros for asserting that the CE of a $match predicate is approximately what we were
 * expecting.
 */

#define _ASSERT_CE(estimatedCE, expectedCE)                             \
    if constexpr (kCETestLogOnly) {                                     \
        if (std::abs(estimatedCE - expectedCE) > kMaxCEError) {         \
            std::cout << "ERROR: expected " << expectedCE << std::endl; \
        }                                                               \
        ASSERT_APPROX_EQUAL(1.0, 1.0, kMaxCEError);                     \
    } else {                                                            \
        ASSERT_APPROX_EQUAL(estimatedCE, expectedCE, kMaxCEError);      \
    }
#define _PREDICATE(field, predicate) (str::stream() << "{" << field << ": " << predicate "}")
#define _ELEMMATCH_PREDICATE(field, predicate) \
    (str::stream() << "{" << field << ": {$elemMatch: " << predicate << "}}")

// This macro verifies the cardinality of a pipeline or an input ABT.
#define ASSERT_CE(ce, pipeline, expectedCE)                                                   \
    _ASSERT_CE(                                                                               \
        ce.getCE(pipeline, [](const ABT& n) -> bool { return n.is<optimizer::RootNode>(); }), \
        (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_CE_CARD(ce, pipeline, expectedCE, collCard) \
    ce.setCollCard(collCard);                              \
    ASSERT_CE(ce, pipeline, expectedCE)

// This macro verifies the cardinality of a pipeline with a single $match predicate.
#define ASSERT_MATCH_CE(ce, predicate, expectedCE)                                              \
    _ASSERT_CE(ce.getMatchCE(predicate,                                                         \
                             [](const ABT& n) -> bool { return n.is<optimizer::RootNode>(); }), \
               (expectedCE))

#define ASSERT_MATCH_CE_NODE(ce, queryPredicate, expectedCE, nodePredicate) \
    _ASSERT_CE(ce.getMatchCE(queryPredicate, nodePredicate), (expectedCE))

// This macro does the same as above but also sets the collection cardinality.
#define ASSERT_MATCH_CE_CARD(ce, predicate, expectedCE, collCard) \
    ce.setCollCard(collCard);                                     \
    ASSERT_MATCH_CE(ce, predicate, expectedCE)

// This macro tests cardinality of two versions of the predicate; with and without $elemMatch.
#define ASSERT_EQ_ELEMMATCH_CE(tester, expectedCE, elemMatchExpectedCE, field, predicate) \
    ASSERT_MATCH_CE(tester, _PREDICATE(field, predicate), expectedCE);                    \
    ASSERT_MATCH_CE(tester, _ELEMMATCH_PREDICATE(field, predicate), elemMatchExpectedCE)

/**
 * A test utility class for helping verify the cardinality of CE transports on a given $match
 * predicate.
 */
class CETester {
public:
    /**
     * The tester initializes at least one collection with the name 'collName' and the cardinality
     * 'numRecords' in the metadata.
     */
    CETester(std::string collName,
             double numRecords,
             const OptPhaseManager::PhaseSet& optPhases = kDefaultCETestPhaseSet);

    /**
     * Returns the estimated cardinality of a given 'matchPredicate'.
     *
     * 'nodePredicate' identifies the node in the memo we want to estimate.
     */
    CEType getMatchCE(const std::string& matchPredicate,
                      std::function<bool(const ABT&)> nodePredicate) const;

    /**
     * Returns the estimated cardinality of a given 'pipeline'.
     *
     * 'nodePredicate' identifies the node in the memo we want to estimate.
     */
    CEType getCE(const std::string& pipeline, std::function<bool(const ABT&)> nodePredicate) const;

    /**
     * Returns the estimated cardinality of a given 'abt'.
     *
     * 'nodePredicate' identifies the node in the memo we want to estimate.
     */
    CEType getCE(ABT& abt, std::function<bool(const ABT&)> nodePredicate) const;

    /**
     * Updates the cardinality of the collection '_collName'.
     */
    void setCollCard(double card);

    /**
     * Updates the indexes used by the collection '_collName'.
     */
    void setIndexes(opt::unordered_map<std::string, IndexDefinition> indexes);

    /**
     * Adds a ScanDefinition for an additional collection for the test.
     */
    void addCollection(std::string collName,
                       double numRecords,
                       opt::unordered_map<std::string, IndexDefinition> indexes = {});

    /**
     * Prevents the optimizer from generating collection scan plans.
     */
    void setDisableScan(bool disableScan) {
        _hints._disableScan = disableScan;
    }

protected:
    /**
     * Subclasses need to override this method to initialize the transports they are testing.
     */
    virtual std::unique_ptr<cascades::CEInterface> getCETransport() const = 0;

private:
    /**
     * Helper to find the ScanDefinition of '_collName' in _metadata.
     */
    ScanDefinition& getCollScanDefinition();

    // Phases to use when optimizing an input query.
    const OptPhaseManager::PhaseSet& _optPhases;

    // Used to initialize the OptPhaseManager.
    mutable PrefixId _prefixId;

    // Allows us to pass hints to the optimizer.
    QueryHints _hints;

    // Stores the ScanDefinitions for all collections defined in the test.
    Metadata _metadata;

    // Name of the collection tests will be executed against.
    std::string _collName;
};

/**
 * Test utility for helping with creation of manual histograms in the unit tests.
 */
struct BucketData {
    Value _v;
    double _equalFreq;
    double _rangeFreq;
    double _ndv;

    BucketData(Value v, double equalFreq, double rangeFreq, double ndv)
        : _v(v), _equalFreq(equalFreq), _rangeFreq(rangeFreq), _ndv(ndv) {}
    BucketData(const std::string& v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
    BucketData(int v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
};

ScalarHistogram createHistogram(const std::vector<BucketData>& data);

double estimateIntValCard(const ScalarHistogram& hist, int v, EstimationType type);

}  // namespace ce
}  // namespace mongo
