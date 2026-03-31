/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo::join_ordering {

StringData toStringData(MackertLohmanCase c) {
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
    return str::stream() << _totalCost.cost().v();
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
