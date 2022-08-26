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
    void reverse();

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

/**
 * This comparator can only compare paths with Get, Traverse, and Id.
 */
struct IndexPath3WComparator {
    bool operator()(const ABT& path1, const ABT& path2) const;
};

using IndexPathSet = std::set<ABT, IndexPath3WComparator>;

struct PartialSchemaKeyLessComparator {
    bool operator()(const PartialSchemaKey& k1, const PartialSchemaKey& k2) const;
};

/**
 * Map from referred (or input) projection name to list of requirements for that projection.
 * Only one instance of a path without Traverse elements (non-multikey) is allowed. By contrast
 * several instances of paths with Traverse elements (multikey) are allowed. For example: Get "a"
 * Get "b" Id is allowed just once while Get "a" Traverse Get "b" Id is allowed multiple times.
 */
using PartialSchemaRequirements =
    std::multimap<PartialSchemaKey, PartialSchemaRequirement, PartialSchemaKeyLessComparator>;

/**
 * Used to track cardinality estimates per predicate inside a PartialSchemaRequirement. The order of
 * estimates is the same as the order in the primary PartialSchemaRequirements map.
 */
using PartialSchemaKeyCE = std::vector<std::pair<PartialSchemaKey, CEType>>;

using PartialSchemaKeySet = std::set<PartialSchemaKey, PartialSchemaKeyLessComparator>;

// Requirements which are not satisfied directly by an IndexScan, PhysicalScan or Seek (e.g. using
// an index field, or scan field). The index refers to the underlying entry in the
// PartialSchemaRequirement map.
struct ResidualRequirement {
    ResidualRequirement(PartialSchemaKey key, PartialSchemaRequirement req, size_t entryIndex);

    bool operator==(const ResidualRequirement& other) const;

    PartialSchemaKey _key;
    PartialSchemaRequirement _req;
    size_t _entryIndex;
};
using ResidualRequirements = std::vector<ResidualRequirement>;

struct ResidualRequirementWithCE {
    ResidualRequirementWithCE(PartialSchemaKey key, PartialSchemaRequirement req, CEType ce);

    PartialSchemaKey _key;
    PartialSchemaRequirement _req;
    CEType _ce;
};
using ResidualRequirementsWithCE = std::vector<ResidualRequirementWithCE>;


// A sequence of intervals corresponding, one for each index key.
using CompoundIntervalRequirement = std::vector<IntervalRequirement>;

// Unions and conjunctions of individual compound intervals.
using CompoundIntervalReqExpr = BoolExpr<CompoundIntervalRequirement>;

// Used to pre-compute candidate indexes for SargableNodes.
struct CandidateIndexEntry {
    CandidateIndexEntry(std::string indexDefName);

    bool operator==(const CandidateIndexEntry& other) const;

    std::string _indexDefName;

    FieldProjectionMap _fieldProjectionMap;
    CompoundIntervalReqExpr::Node _intervals;

    // Requirements which are not satisfied directly by the IndexScan. They are intended to be
    // sorted in their containing vector from most to least selective.
    ResidualRequirements _residualRequirements;

    // We have equalities on those index fields, and thus do not consider for collation
    // requirements.
    opt::unordered_set<size_t> _fieldsToCollate;

    // Length of prefix of fields with applied intervals.
    size_t _intervalPrefixSize;
};

struct ScanParams {
    bool operator==(const ScanParams& other) const;

    FieldProjectionMap _fieldProjectionMap;

    // Requirements which are not satisfied directly by a PhysicalScan or Seek. They are intended to
    // be sorted in their containing vector from most to least selective.
    ResidualRequirements _residualRequirements;
};

using CandidateIndexes = std::vector<CandidateIndexEntry>;

class IndexSpecification {
public:
    IndexSpecification(std::string scanDefName,
                       std::string indexDefName,
                       CompoundIntervalRequirement interval,
                       bool reverseOrder);

    bool operator==(const IndexSpecification& other) const;

    const std::string& getScanDefName() const;
    const std::string& getIndexDefName() const;

    const CompoundIntervalRequirement& getInterval() const;

    bool isReverseOrder() const;

private:
    // Name of the collection.
    const std::string _scanDefName;

    // The name of the index.
    const std::string _indexDefName;

    // The index interval.
    CompoundIntervalRequirement _interval;

    // Do we reverse the index order.
    const bool _reverseOrder;
};

}  // namespace mongo::optimizer
