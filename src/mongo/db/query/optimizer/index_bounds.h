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
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"


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
    PartialSchemaKey(ABT path);
    PartialSchemaKey(ProjectionName projectionName, ABT path);
    PartialSchemaKey(boost::optional<ProjectionName> projectionName, ABT path);

    bool operator==(const PartialSchemaKey& other) const;

    // Referred, or input projection name.
    boost::optional<ProjectionName> _projectionName;

    // (Partially determined) path.
    ABT _path;
};

using IntervalReqExpr = BoolExpr<IntervalRequirement>;
bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n);

class PartialSchemaRequirement {
public:
    PartialSchemaRequirement(boost::optional<ProjectionName> boundProjectionName,
                             IntervalReqExpr::Node intervals,
                             bool isPerfOnly);

    bool operator==(const PartialSchemaRequirement& other) const;

    const boost::optional<ProjectionName>& getBoundProjectionName() const;

    const IntervalReqExpr::Node& getIntervals() const;

    bool getIsPerfOnly() const;

    bool mayReturnNull(const ConstFoldFn& constFold) const;

private:
    // Bound, or output projection name.
    boost::optional<ProjectionName> _boundProjectionName;

    IntervalReqExpr::Node _intervals;

    // If set, this requirement exists for performance reasons (as opposed to correctness). We will
    // attempt to incorporate it into index bounds, otherwise will not add it to residual
    // predicates.
    bool _isPerfOnly;
};

/**
 * This comparator is used to compare paths with Get, Traverse, and Id.
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


// A sequence of intervals corresponding to a compound bound, with one entry for each index key.
// This is a physical primitive as it is tied to a specific index. It contains a sequence of simple
// bounds which form a single equality prefix. As such the first 0+ entries are inclusive
// (equalities), followed by 0/1 possibly exclusive ranges, followed by 0+ unconstrained entries
// (MinKey to MaxKey). When the IndexNode is lowered we need to compute a global inclusion/exclusion
// for the entire compound interval, and we do so by determining if there are ANY exclusive simple
// low bounds or high bounds. In this case the compound bound is exclusive (on the low or the high
// side respectively).
// TODO: SERVER-72784: Update representation of compound index bounds.
using CompoundIntervalRequirement = std::vector<IntervalRequirement>;

// Unions and conjunctions of individual compound intervals.
using CompoundIntervalReqExpr = BoolExpr<CompoundIntervalRequirement>;

struct EqualityPrefixEntry {
    EqualityPrefixEntry(size_t startPos);

    bool operator==(const EqualityPrefixEntry& other) const;

    // Which one is the first index field in the prefix.
    size_t _startPos;
    // Contains the intervals we compute for each "equality prefix" of query predicates.
    CompoundIntervalReqExpr::Node _interval;
    // Set of positions of predicates (in the requirements map) which we encode with this prefix.
    opt::unordered_set<size_t> _predPosSet;
};

/**
 * Used to pre-compute candidate indexes for SargableNodes.
 * We keep track of the following:
 *  1. Name of index this entry applies to. There may be multiple entries for the same index if we
 * have more than one equality prefix (see below).
 *  2. A map from index field to projection we deliver the field under. We can select which index
 * fields we want to bind to which projections.
 *  3. A vector of equality prefixes. The equality prefix refers to the predicates applied to each
 * of the index fields. It consists of 0+ equalities followed by at most one inequality, and
 * followed by 0+ unconstrained fields. For example, suppose we have an index consisting of five
 * fields with the following intervals applied to each: _field1: [0, 0], _field2: [1, 1], _field3:
 * [2, MaxKey], _field4: (unconstrained), _field5: [MinKey, 10], _field6: [100, MaxKey]. In this
 * example we have 3 equality prefixes: {_field1, field_2, field3, _field4}, {_field5}, {_field6}.
 * If we have more than one prefixes, we may choose to perform recursive index navigation. If in the
 * simplest case with one equality prefix we perform an index scan with some residual predicates
 * applied.
 *  4. Residual predicates. We may not satisfy all intervals directly by converting into index
 * bounds, and instead may satisfy some as residual predicates. Consider the example: _field1: [0,
 * MaxKey], _field2: [1, MaxKey]. If we were to constrain the candidate indexes to just one equality
 * prefix, we would create compound index bound [{0, MinKey}, {MaxKey, MaxKey}] effectively encoding
 * the interval over the first field into the bound, the binding _field2 to a temporary variable,
 * and applying a filter encoding [1, MaxKey] over the index scan.
 *  5. Fields to collate. We keep track of which fields need collation. This is needed during the
 * physical rewrite phase where we need to match the collation requirements with the collation of
 * the index. Essentially, we may ignore collation requirements for fields which are equalities.
 */
struct CandidateIndexEntry {
    CandidateIndexEntry(std::string indexDefName);

    bool operator==(const CandidateIndexEntry& other) const;

    std::string _indexDefName;

    // Indicates which fields we are retrieving and how we assign them to projections.
    FieldProjectionMap _fieldProjectionMap;

    // Contains equality prefixes for this index.
    std::vector<EqualityPrefixEntry> _eqPrefixes;
    // If we have more than one equality prefix, contains the list of the correlated projections.
    ProjectionNameOrderPreservingSet _correlatedProjNames;

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

}  // namespace mongo::optimizer
