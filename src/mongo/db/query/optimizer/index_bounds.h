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

/**
 * Generic bound.
 */
template <class T>
class Bound {
public:
    Bound(bool inclusive, T bound) : _inclusive(inclusive), _bound(std::move(bound)) {}

    bool operator==(const Bound& other) const {
        return _inclusive == other._inclusive && _bound == other._bound;
    }

    bool isInclusive() const {
        return _inclusive;
    }

    const T& getBound() const {
        return _bound;
    }
    T& getBound() {
        return _bound;
    }

protected:
    bool _inclusive;
    T _bound;
};

/**
 * Generic interval.
 */
template <class T>
class Interval {
public:
    Interval(T lowBound, T highBound)
        : _lowBound(std::move(lowBound)), _highBound(std::move(highBound)) {}

    bool operator==(const Interval& other) const {
        return _lowBound == other._lowBound && _highBound == other._highBound;
    }

    bool isEquality() const {
        return _lowBound.isInclusive() && _highBound.isInclusive() && _lowBound == _highBound;
    }

    void reverse() {
        std::swap(_lowBound, _highBound);
    }

    const T& getLowBound() const {
        return _lowBound;
    }
    T& getLowBound() {
        return _lowBound;
    }

    const T& getHighBound() const {
        return _highBound;
    }
    T& getHighBound() {
        return _highBound;
    }

protected:
    T _lowBound;
    T _highBound;
};

/**
 * Represents a bound in an simple interval (interval over one projection). The bound can be a
 * constant or an expression (e.g. a formula). This is a logical abstraction.
 */
class BoundRequirement : public Bound<ABT> {
    using Base = Bound<ABT>;

public:
    static BoundRequirement makeMinusInf();
    static BoundRequirement makePlusInf();

    BoundRequirement(bool inclusive, ABT bound);

    bool isMinusInf() const;
    bool isPlusInf() const;
};

/**
 * Represents a simple interval (interval over one projection). This is a logical abstraction. It
 * counts low and high bounds which may be inclusive or exclusive.
 */
class IntervalRequirement : public Interval<BoundRequirement> {
    using Base = Interval<BoundRequirement>;

public:
    IntervalRequirement();
    IntervalRequirement(BoundRequirement lowBound, BoundRequirement highBound);

    bool isFullyOpen() const;
    bool isConstant() const;
};

/**
 * Represents an expression (consisting of possibly nested unions and intersections) over an
 * interval.
 */
using IntervalReqExpr = BoolExpr<IntervalRequirement>;
bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n);

/**
 * Represents a bound in a compound interval, which encodes an equality prefix. It consists of a
 * vector of expressions, which represents an index bound. This is a physical abstraction.
 */
class CompoundBoundRequirement : public Bound<ABTVector> {
    using Base = Bound<ABTVector>;

public:
    CompoundBoundRequirement(bool inclusive, ABTVector bound);

    bool isMinusInf() const;
    bool isPlusInf() const;
    bool isConstant() const;

    size_t size() const;

    // Extend the current compound bound with a simple bound. It is the caller's responsibility to
    // ensure we confirm to an equality prefix.
    void push_back(BoundRequirement bound);
};

/**
 * An interval of compound keys: each endpoint is a compound key, with one expression per index key.
 * This is a physical primitive tied to a specific index.
 */
class CompoundIntervalRequirement : public Interval<CompoundBoundRequirement> {
    using Base = Interval<CompoundBoundRequirement>;

public:
    CompoundIntervalRequirement();
    CompoundIntervalRequirement(CompoundBoundRequirement lowBound,
                                CompoundBoundRequirement highBound);

    bool isFullyOpen() const;

    size_t size() const;
    void push_back(IntervalRequirement interval);
};

// Unions and conjunctions of individual compound intervals.
using CompoundIntervalReqExpr = BoolExpr<CompoundIntervalRequirement>;

struct PartialSchemaKey {
    // The default construct sets the path to PathIdentity and the projectionName to boost::none.
    PartialSchemaKey();

    PartialSchemaKey(ABT path);
    PartialSchemaKey(ProjectionName projectionName, ABT path);
    PartialSchemaKey(boost::optional<ProjectionName> projectionName, ABT path);

    bool operator==(const PartialSchemaKey& other) const;
    bool operator!=(const PartialSchemaKey& other) const {
        return !(*this == other);
    }

    // Referred, or input projection name.
    boost::optional<ProjectionName> _projectionName;

    // (Partially determined) path.
    ABT _path;
};

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
struct IndexPathLessComparator {
    bool operator()(const ABT& path1, const ABT& path2) const;
};

using IndexPathSet = std::set<ABT, IndexPathLessComparator>;

struct PartialSchemaKeyComparator {
    struct Less {
        bool operator()(const PartialSchemaKey& k1, const PartialSchemaKey& k2) const;
    };

    struct Cmp3W {
        int operator()(const PartialSchemaKey& k1, const PartialSchemaKey& k2) const;
    };
};
struct PartialSchemaRequirementComparator {
    struct Less {
        bool operator()(const PartialSchemaRequirement& req1,
                        const PartialSchemaRequirement& req2) const;
    };

    struct Cmp3W {
        int operator()(const PartialSchemaRequirement& req1,
                       const PartialSchemaRequirement& req2) const;
    };
};


/**
 * Used to track cardinality estimates per predicate inside a PartialSchemaRequirement. The order of
 * estimates is the same as the leaf order in the primary PartialSchemaRequirements.
 */
using PartialSchemaKeyCE = std::vector<std::pair<PartialSchemaKey, CEType>>;

using PartialSchemaKeySet = std::set<PartialSchemaKey, PartialSchemaKeyComparator::Less>;

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
using ResidualRequirements = BoolExpr<ResidualRequirement>;

struct ResidualRequirementWithOptionalCE {
    ResidualRequirementWithOptionalCE(PartialSchemaKey key,
                                      PartialSchemaRequirement req,
                                      boost::optional<CEType> ce);

    bool operator==(const ResidualRequirementWithOptionalCE& other) const;

    PartialSchemaKey _key;
    PartialSchemaRequirement _req;
    boost::optional<CEType> _ce;
};
using ResidualRequirementsWithOptionalCE = BoolExpr<ResidualRequirementWithOptionalCE>;

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
 * Specifies type of query predicate which is being answered using a particular field on a candidate
 * index. This is used to determine if we can (or should) attempt to satisfy collation requirements
 * during the implementation phase. For example if we have a query (a = 1) and (b > 2) and (c = 3 or
 * c > 10) the entries will be SimpleEquality, SimpleInequality, and Compound.
 */
#define INDEXFIELD_PREDTYPE_OPNAMES(F) \
    F(SimpleEquality)                  \
    F(SimpleInequality)                \
    F(Compound)                        \
    F(Unbound)

MAKE_PRINTABLE_ENUM(IndexFieldPredType, INDEXFIELD_PREDTYPE_OPNAMES);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(IndexFieldPredTypeEnum,
                                 IndexFieldPredType,
                                 INDEXFIELD_PREDTYPE_OPNAMES);
#undef INDEXFIELD_PREDTYPE_OPNAMES

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
 *  5. List of the predicate types applied to each field in the candidate index. This is needed
 * during the physical rewrite phase where we need to match the collation requirements with the
 * collation of the index. One use is to ignore collation requirements for fields which are
 * equalities.
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

    // Requirements which are not satisfied directly by the IndexScan. Each Conjunction is intended
    // to have its children sorted from most to least selective.
    // boost::none indicates that all requirements are satisfied by the IndexScan.
    boost::optional<ResidualRequirements::Node> _residualRequirements;

    // Types of the predicates which we will answer using a field at a given position in the
    // candidate index.
    std::vector<IndexFieldPredType> _predTypes;

    // Length of prefix of fields with applied intervals.
    size_t _intervalPrefixSize;
};

/**
 * ScanParams describes a set of predicates and projections to use for a collection scan or fetch.
 *
 * The semantics are:
 * 1. Apply the FieldProjectionMap to introduce some bindings.
 * 2. Apply the ResidualRequirements (a filter), which can read any of those bindings.
 *
 * We represent projections specially because SBE 'ScanStage' is more efficient at handling multiple
 * fields, compared to doing N separate getField calls.
 */
struct ScanParams {
    bool operator==(const ScanParams& other) const;

    FieldProjectionMap _fieldProjectionMap;

    // Requirements which are not satisfied directly by a PhysicalScan or Seek. Each Conjunction is
    // intended to have its children sorted from most to least selective. boost::none indicates that
    // all requirements are satisfied by the PhysicalScan or Seek.
    boost::optional<ResidualRequirements::Node> _residualRequirements;
};

using CandidateIndexes = std::vector<CandidateIndexEntry>;

}  // namespace mongo::optimizer
