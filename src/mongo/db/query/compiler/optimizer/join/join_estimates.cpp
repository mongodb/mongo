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

namespace mongo::join_ordering {

// TODO SERVER-117084: Calibrate coefficients
// As a starting point, assume the following:
// * Processing a document takes 100 nanoseconds
// * Sequential IO is 100x slower that processing a document
// * Random IO is 10x slower than sequential IO.
const CostCoefficient docProcessCpuIncremental{CostCoefficientType{100.0_ms}};
const CostCoefficient ioSeqIncremental{
    CostCoefficientType{docProcessCpuIncremental.toDouble() * 10e2}};
const CostCoefficient ioRandIncremental{CostCoefficientType{ioSeqIncremental.toDouble() * 10}};

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

std::string JoinCostEstimate::toString() const {
    return str::stream() << _totalCost.cost().v();
}

BSONObj JoinCostEstimate::toBSON() const {
    return BSON("totalCost" << _totalCost.toBSON() << "numDocsProcessed"
                            << _numDocsProcessed.toBSON() << "numDocsOutput"
                            << _numDocsOutput.toBSON() << "ioSeqNumPages" << _ioSeqNumPages.toBSON()
                            << "ioRandNumPages" << _ioRandNumPages.toBSON());
}

}  // namespace mongo::join_ordering
