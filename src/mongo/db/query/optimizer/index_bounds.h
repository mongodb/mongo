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

#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"


namespace mongo::optimizer {

class BoundRequirement {
public:
    static BoundRequirement makeMinusInf();
    static BoundRequirement makePlusInf();

    BoundRequirement(bool inclusive, ABT bound);

    bool operator==(const BoundRequirement& other) const;

    bool isInclusive() const;

    bool isMinusInf() const;
    bool isPlusInf() const;

    const ABT& getBound() const;

private:
    bool _inclusive;
    ABT _bound;
};

class IntervalRequirement {
public:
    IntervalRequirement();
    IntervalRequirement(BoundRequirement lowBound, BoundRequirement highBound);

    bool operator==(const IntervalRequirement& other) const;

    bool isFullyOpen() const;
    bool isEquality() const;

    const BoundRequirement& getLowBound() const;
    BoundRequirement& getLowBound();
    const BoundRequirement& getHighBound() const;
    BoundRequirement& getHighBound();

private:
    BoundRequirement _lowBound;
    BoundRequirement _highBound;
};

struct PartialSchemaKey {
    PartialSchemaKey();
    PartialSchemaKey(ProjectionName projectionName);
    PartialSchemaKey(ProjectionName projectionName, ABT path);

    bool operator==(const PartialSchemaKey& other) const;

    bool emptyPath() const;

    // Referred, or input projection name.
    ProjectionName _projectionName;

    // (Partially determined) path.
    ABT _path;
};

using IntervalReqExpr = BoolExpr<IntervalRequirement>;
bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n);

class PartialSchemaRequirement {
public:
    PartialSchemaRequirement();
    PartialSchemaRequirement(ProjectionName boundProjectionName, IntervalReqExpr::Node intervals);

    bool operator==(const PartialSchemaRequirement& other) const;

    bool hasBoundProjectionName() const;
    const ProjectionName& getBoundProjectionName() const;
    void setBoundProjectionName(ProjectionName boundProjectionName);

    const IntervalReqExpr::Node& getIntervals() const;
    IntervalReqExpr::Node& getIntervals();

private:
    // Bound, or output projection name.
    ProjectionName _boundProjectionName;

    IntervalReqExpr::Node _intervals;
};

struct PartialSchemaKeyLessComparator {
    bool operator()(const PartialSchemaKey& k1, const PartialSchemaKey& k2) const;
};

// Map from referred (or input) projection name to list of requirements for that projection.
using PartialSchemaRequirements =
    std::map<PartialSchemaKey, PartialSchemaRequirement, PartialSchemaKeyLessComparator>;

using PartialSchemaKeyCE = std::map<PartialSchemaKey, CEType, PartialSchemaKeyLessComparator>;
using ResidualKeyMap = std::map<PartialSchemaKey, PartialSchemaKey, PartialSchemaKeyLessComparator>;

using PartialSchemaKeySet = std::set<PartialSchemaKey, PartialSchemaKeyLessComparator>;

// Requirements which are not satisfied directly by an IndexScan, PhysicalScan or Seek (e.g. using
// an index field, or scan field). They are intended to be sorted in their containing vector from
// most to least selective.
struct ResidualRequirement {
    ResidualRequirement(PartialSchemaKey key, PartialSchemaRequirement req, CEType ce);

    PartialSchemaKey _key;
    PartialSchemaRequirement _req;
    CEType _ce;
};
using ResidualRequirements = std::vector<ResidualRequirement>;

// A sequence of intervals corresponding, one for each index key.
using MultiKeyIntervalRequirement = std::vector<IntervalRequirement>;

// Multi-key intervals represent unions and conjunctions of individual multi-key intervals.
using MultiKeyIntervalReqExpr = BoolExpr<MultiKeyIntervalRequirement>;

// Used to pre-compute candidate indexes for SargableNodes.
struct CandidateIndexEntry {
    bool operator==(const CandidateIndexEntry& other) const;

    FieldProjectionMap _fieldProjectionMap;
    MultiKeyIntervalReqExpr::Node _intervals;

    PartialSchemaRequirements _residualRequirements;
    // Projections needed to evaluate residual requirements.
    ProjectionNameSet _residualRequirementsTempProjections;

    // Used for CE. Mapping for residual requirement key to query key.
    ResidualKeyMap _residualKeyMap;

    // We have equalities on those index fields, and thus do not consider for collation
    // requirements.
    // TODO: consider a bitset.
    opt::unordered_set<size_t> _fieldsToCollate;

    // Length of prefix of fields with applied intervals.
    size_t _intervalPrefixSize;
};

using CandidateIndexMap = std::map<std::string /*index name*/, CandidateIndexEntry>;

class IndexSpecification {
public:
    IndexSpecification(std::string scanDefName,
                       std::string indexDefName,
                       MultiKeyIntervalRequirement interval,
                       bool reverseOrder);

    bool operator==(const IndexSpecification& other) const;

    const std::string& getScanDefName() const;
    const std::string& getIndexDefName() const;

    const MultiKeyIntervalRequirement& getInterval() const;

    bool isReverseOrder() const;

private:
    // Name of the collection.
    const std::string _scanDefName;

    // The name of the index.
    const std::string _indexDefName;

    // The index interval.
    MultiKeyIntervalRequirement _interval;

    // Do we reverse the index order.
    const bool _reverseOrder;
};

}  // namespace mongo::optimizer
