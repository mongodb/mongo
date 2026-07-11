// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo::join_ordering {

std::string_view toStringData(MackertLohmanCase c) {
    switch (c) {
        case MackertLohmanCase::kCollectionFitsCache:
            return "collection-fits-cache";
        case MackertLohmanCase::kReturnedDocsFitCache:
            return "returned-docs-fit-cache";
        case MackertLohmanCase::kPartialEviction:
            return "partial-eviction";
    }
    MONGO_UNREACHABLE;
}

void JoinExtraEstimateInfo::serialize(BSONObjBuilder& bob) const {
    QSNEstimate::serialize(bob);
    BSONObjBuilder subBob(bob.subobjStart("joinCostComponents"));
    subBob.append("docsProcessed", docsProcessed);
    subBob.append("docsOutput", docsOutput);
    subBob.append("sequentialIOPages", sequentialIOPages);
    subBob.append("randomIOPages", randomIOPages);
    subBob.append("localOpCost", localOpCost);
    if (mackertLohmanCase) {
        subBob.append("mackertLohmanCase", toStringData(*mackertLohmanCase));
    }
}

// These coefficients were calibrated in /buildscripts/cost_model/join_start.py,
// which indicated that processing a single document takes 612 nanoseconds.
const CostCoefficient docProcessCpuIncremental = makeCostCoefficient(nsec(612.0));
const CostCoefficient ioSeqIncremental{
    CostCoefficientType{docProcessCpuIncremental.toDouble() * 261.1}};
const CostCoefficient ioRandIncremental{
    CostCoefficientType{docProcessCpuIncremental.toDouble() * 1411.4}};

CardinalityEstimate numDocsProcessedFromCpuCost(CostEstimate cpuCost) {
    return CardinalityEstimate{
        CardinalityType{cpuCost.toDouble() / docProcessCpuIncremental.toDouble()},
        cpuCost.source()};
}

JoinCostEstimate::JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                                   CardinalityEstimate numDocsOutput,
                                   CardinalityEstimate numSeqIOs,
                                   CardinalityEstimate numRandIOs)
    : _numDocsProcessed(numDocsProcessed),
      _numDocsOutput(numDocsOutput),
      _ioSeqNumPages(numSeqIOs),
      _ioRandNumPages(numRandIOs),
      _localOpCost(zeroCost),
      _totalCost(zeroCost) {
    _localOpCost =
        docProcessCpuIncremental * (_numDocsProcessed + _numDocsOutput + _numDocsTransmitted) +
        _ioSeqNumPages * ioSeqIncremental + _ioRandNumPages * ioRandIncremental;
    _totalCost = _localOpCost;
}

JoinCostEstimate::JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                                   CardinalityEstimate numDocsOutput,
                                   CardinalityEstimate numSeqIOs,
                                   CardinalityEstimate numRandIOs,
                                   JoinCostEstimate leftCost,
                                   JoinCostEstimate rightCost)
    : JoinCostEstimate(numDocsProcessed, numDocsOutput, numSeqIOs, numRandIOs) {
    _totalCost = _localOpCost + leftCost.getTotalCost() + rightCost.getTotalCost();
}

JoinCostEstimate::JoinCostEstimate(CardinalityEstimate numDocsProcessed,
                                   CardinalityEstimate numDocsOutput,
                                   CardinalityEstimate numSeqIOs,
                                   CardinalityEstimate numRandIOs,
                                   JoinCostEstimate leftCost,
                                   JoinCostEstimate rightCost,
                                   MackertLohmanCase mackertLohmanCase)
    : JoinCostEstimate(
          numDocsProcessed, numDocsOutput, numSeqIOs, numRandIOs, leftCost, rightCost) {
    _mackertLohmanCase = mackertLohmanCase;
}

JoinCostEstimate::JoinCostEstimate(CostEstimate totalCost)
    : _numDocsProcessed(zeroCE),
      _numDocsOutput(zeroCE),
      _ioSeqNumPages(zeroCE),
      _ioRandNumPages(zeroCE),
      _localOpCost(zeroCost),
      _totalCost(totalCost) {}

std::string JoinCostEstimate::toString() const {
    return str::stream() << _totalCost.toDouble();
}

BSONObj JoinCostEstimate::toBSON() const {
    BSONObjBuilder bob;
    bob << "totalCost" << _totalCost.toBSON() << "numDocsProcessed" << _numDocsProcessed.toBSON()
        << "numDocsOutput" << _numDocsOutput.toBSON() << "ioSeqNumPages" << _ioSeqNumPages.toBSON()
        << "ioRandNumPages" << _ioRandNumPages.toBSON();
    if (_mackertLohmanCase) {
        bob << "mackertLohmanCase" << toStringData(*_mackertLohmanCase);
    }
    return bob.obj();
}

}  // namespace mongo::join_ordering
