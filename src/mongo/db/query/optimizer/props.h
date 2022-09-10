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

#include <map>
#include <string>
#include <vector>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer::properties {

/**
 * Tag for logical property types.
 */
class LogicalPropertyTag {};

/**
 * Tag for physical property types.
 */
class PhysPropertyTag {};

/**
 * Logical properties.
 */
class CardinalityEstimate;

class ProjectionAvailability;
class IndexingAvailability;
class CollectionAvailability;
class DistributionAvailability;

/**
 * Physical properties.
 */
class CollationRequirement;
class LimitSkipRequirement;
class ProjectionRequirement;
class DistributionRequirement;
class IndexingRequirement;
class RepetitionEstimate;
class LimitEstimate;

using LogicalProperty = algebra::PolyValue<CardinalityEstimate,
                                           ProjectionAvailability,
                                           IndexingAvailability,
                                           CollectionAvailability,
                                           DistributionAvailability>;

using PhysProperty = algebra::PolyValue<CollationRequirement,
                                        LimitSkipRequirement,
                                        ProjectionRequirement,
                                        DistributionRequirement,
                                        IndexingRequirement,
                                        RepetitionEstimate,
                                        LimitEstimate>;

using LogicalProps = opt::unordered_map<LogicalProperty::key_type, LogicalProperty>;
using PhysProps = opt::unordered_map<PhysProperty::key_type, PhysProperty>;

template <typename T,
          std::enable_if_t<std::is_base_of_v<LogicalPropertyTag, T>, bool> = true,
          typename... Args>
inline auto makeProperty(Args&&... args) {
    return LogicalProperty::make<T>(std::forward<Args>(args)...);
}

template <typename T,
          std::enable_if_t<std::is_base_of_v<PhysPropertyTag, T>, bool> = true,
          typename... Args>
inline auto makeProperty(Args&&... args) {
    return PhysProperty::make<T>(std::forward<Args>(args)...);
}

template <class P, std::enable_if_t<std::is_base_of_v<LogicalPropertyTag, P>, bool> = true>
static constexpr auto getPropertyKey() {
    return LogicalProperty::template tagOf<P>();
}

template <class P, std::enable_if_t<std::is_base_of_v<PhysPropertyTag, P>, bool> = true>
static constexpr auto getPropertyKey() {
    return PhysProperty::template tagOf<P>();
}

template <class P, class C>
bool hasProperty(const C& props) {
    return props.find(getPropertyKey<P>()) != props.cend();
}

template <class P, class C>
P& getProperty(C& props) {
    if (!hasProperty<P>(props)) {
        uasserted(6624022, "Property type does not exist.");
    }
    return *props.at(getPropertyKey<P>()).template cast<P>();
}

template <class P, class C>
const P& getPropertyConst(const C& props) {
    if (!hasProperty<P>(props)) {
        uasserted(6624023, "Property type does not exist.");
    }
    return *props.at(getPropertyKey<P>()).template cast<P>();
}

template <class P, class C>
void removeProperty(C& props) {
    props.erase(getPropertyKey<P>());
}

template <class P, class C>
bool setProperty(C& props, P property) {
    return props.emplace(getPropertyKey<P>(), makeProperty<P>(std::move(property))).second;
}

template <class P, class C>
void setPropertyOverwrite(C& props, P property) {
    props.insert_or_assign(getPropertyKey<P>(), makeProperty<P>(std::move(property)));
}

template <class C, typename... Args>
inline auto makeProps(Args&&... args) {
    C props;
    (setProperty(props, args), ...);
    return props;
}

template <typename... Args>
inline auto makeLogicalProps(Args&&... args) {
    return makeProps<LogicalProps>(std::forward<Args>(args)...);
}

template <typename... Args>
inline auto makePhysProps(Args&&... args) {
    return makeProps<PhysProps>(std::forward<Args>(args)...);
}

/**
 * A physical property which specifies how the collection (or intermediate result) is required to be
 * collated (sorted).
 */
class CollationRequirement final : public PhysPropertyTag {
public:
    CollationRequirement(ProjectionCollationSpec spec);

    bool operator==(const CollationRequirement& other) const;

    const ProjectionCollationSpec& getCollationSpec() const;
    ProjectionCollationSpec& getCollationSpec();

    bool hasClusteredOp() const;

    ProjectionNameSet getAffectedProjectionNames() const;

private:
    ProjectionCollationSpec _spec;
};

/**
 * A physical property which specifies what portion of the result in terms of window defined by the
 * limit and skip is to be returned.
 */
class LimitSkipRequirement final : public PhysPropertyTag {
public:
    static constexpr int64_t kMaxVal = std::numeric_limits<int64_t>::max();

    LimitSkipRequirement(int64_t limit, int64_t skip);

    bool operator==(const LimitSkipRequirement& other) const;

    bool hasLimit() const;

    int64_t getLimit() const;
    int64_t getSkip() const;
    int64_t getAbsoluteLimit() const;

    ProjectionNameSet getAffectedProjectionNames() const;

private:
    // Max number of documents to return. Maximum integer value means unlimited.
    int64_t _limit;
    // Documents to skip before start returning in result.
    int64_t _skip;
};

/**
 * A physical property which specifies required projections to be returned as part of the result.
 */
class ProjectionRequirement final : public PhysPropertyTag {
public:
    ProjectionRequirement(ProjectionNameOrderPreservingSet projections);

    bool operator==(const ProjectionRequirement& other) const;

    const ProjectionNameOrderPreservingSet& getProjections() const;
    ProjectionNameOrderPreservingSet& getProjections();

    ProjectionNameSet getAffectedProjectionNames() const;

private:
    ProjectionNameOrderPreservingSet _projections;
};

struct DistributionAndProjections {
    DistributionAndProjections(DistributionType type);
    DistributionAndProjections(DistributionType type, ProjectionNameVector projectionNames);

    bool operator==(const DistributionAndProjections& other) const;

    const DistributionType _type;

    /**
     * Defined for hash and range-based partitioning.
     */
    ProjectionNameVector _projectionNames;
};

/**
 * A physical property which specifies how the result is to be distributed (or partitioned) amongst
 * the computing partitions/nodes.
 */
class DistributionRequirement final : public PhysPropertyTag {
public:
    DistributionRequirement(DistributionAndProjections distributionAndProjections);

    bool operator==(const DistributionRequirement& other) const;

    const DistributionAndProjections& getDistributionAndProjections() const;
    DistributionAndProjections& getDistributionAndProjections();

    ProjectionNameSet getAffectedProjectionNames() const;

    bool getDisableExchanges() const;
    void setDisableExchanges(bool disableExchanges);

private:
    DistributionAndProjections _distributionAndProjections;

    // Heuristic used to disable exchanges right after Filter, Eval, and local GroupBy nodes.
    bool _disableExchanges;
};

/**
 * A physical property which describes if we intend to satisfy sargable predicates using an index.
 * With indexing requirement "Complete", we are requiring a regular physical
 * scan (both rid and row). With "Seek" (where we must have a non-empty RID projection name), we are
 * targeting a physical Seek. With "Index" (with or without RID projection name), we
 * are targeting a physical IndexScan. If in this case we have set RID projection, then we have
 * either gone for a Seek, or we have performed intersection. With empty RID we are targeting a
 * covered index scan.
 */
class IndexingRequirement final : public PhysPropertyTag {
public:
    IndexingRequirement();
    IndexingRequirement(IndexReqTarget indexReqTarget,
                        bool dedupRIDs,
                        GroupIdType satisfiedPartialIndexesGroupId);

    bool operator==(const IndexingRequirement& other) const;

    ProjectionNameSet getAffectedProjectionNames() const;

    IndexReqTarget getIndexReqTarget() const;

    bool getDedupRID() const;
    void setDedupRID(bool value);

    GroupIdType getSatisfiedPartialIndexesGroupId() const;

private:
    const IndexReqTarget _indexReqTarget;

    // If target == Index, specifies if we need to dedup RIDs.
    // Prior RID intersection removes the need to dedup.
    bool _dedupRID;

    // Set of indexes with partial indexes whose partial filters are satisfied considering the whole
    // query. Points to a group where can interrogate IndexingAvailability to find the satisfied
    // indexes.
    const GroupIdType _satisfiedPartialIndexesGroupId;
};

/**
 * A physical property that specifies how many times do we expect to execute the current subtree.
 * Typically generated via a NLJ where it is set on the inner side to reflect the outer side's
 * cardinality. This property affects costing of stateful physical operators such as sort and hash
 * groupby.
 */
class RepetitionEstimate final : public PhysPropertyTag {
public:
    RepetitionEstimate(CEType estimate);

    bool operator==(const RepetitionEstimate& other) const;

    ProjectionNameSet getAffectedProjectionNames() const;

    CEType getEstimate() const;

private:
    CEType _estimate;
};

/**
 * A physical property that specifies that the we will consider only some approximate number of
 * documents. Typically generated after enforcing a LimitSkipRequirement. This property affects
 * costing of stateful physical operators such as sort and hash groupby.
 */
class LimitEstimate final : public PhysPropertyTag {
public:
    LimitEstimate(CEType estimate);

    bool operator==(const LimitEstimate& other) const;

    ProjectionNameSet getAffectedProjectionNames() const;

    bool hasLimit() const;
    CEType getEstimate() const;

private:
    CEType _estimate;
};

/**
 * A logical property which specifies available projections for a given ABT tree.
 */
class ProjectionAvailability final : public LogicalPropertyTag {
public:
    ProjectionAvailability(ProjectionNameSet projections);

    bool operator==(const ProjectionAvailability& other) const;

    const ProjectionNameSet& getProjections() const;

private:
    ProjectionNameSet _projections;
};

/**
 * A logical property which provides an estimated row count for a given ABT tree.
 */
class CardinalityEstimate final : public LogicalPropertyTag {
public:
    CardinalityEstimate(CEType estimate);

    bool operator==(const CardinalityEstimate& other) const;

    CEType getEstimate() const;
    CEType& getEstimate();

    const PartialSchemaKeyCE& getPartialSchemaKeyCE() const;
    PartialSchemaKeyCE& getPartialSchemaKeyCE();

private:
    CEType _estimate;

    // Used for SargableNodes. Provide additional per partial schema key CE.
    PartialSchemaKeyCE _partialSchemaKeyCE;
};

/**
 * A logical property which specifies availability to index predicates in the ABT subtree and
 * contains the scan projection. The projection and definition name are here for convenience: it can
 * be retrieved using the scan group from the memo.
 */
class IndexingAvailability final : public LogicalPropertyTag {
public:
    IndexingAvailability(GroupIdType scanGroupId,
                         ProjectionName scanProjection,
                         std::string scanDefName,
                         bool eqPredsOnly,
                         opt::unordered_set<std::string> satisfiedPartialIndexes);

    bool operator==(const IndexingAvailability& other) const;

    GroupIdType getScanGroupId() const;
    void setScanGroupId(GroupIdType scanGroupId);

    const ProjectionName& getScanProjection() const;
    const std::string& getScanDefName() const;

    const opt::unordered_set<std::string>& getSatisfiedPartialIndexes() const;
    opt::unordered_set<std::string>& getSatisfiedPartialIndexes();

    bool getEqPredsOnly() const;
    void setEqPredsOnly(bool value);

private:
    GroupIdType _scanGroupId;
    const ProjectionName _scanProjection;
    const std::string _scanDefName;

    // Specifies if all predicates in the current group and child group are equalities.
    // This is determined based on SargableNode exclusively containing equality intervals.
    bool _eqPredsOnly;

    // Set of indexes with partial indexes whose partial filters are satisfied for the current
    // group.
    opt::unordered_set<std::string> _satisfiedPartialIndexes;
};


/**
 * Logical property which specifies which collections (scanDefs) are available for a particular
 * group. For example if the group contains a join of two tables, we would have (at least) two
 * collections in the set.
 */
class CollectionAvailability final : public LogicalPropertyTag {
public:
    CollectionAvailability(opt::unordered_set<std::string> scanDefSet);

    bool operator==(const CollectionAvailability& other) const;

    const opt::unordered_set<std::string>& getScanDefSet() const;
    opt::unordered_set<std::string>& getScanDefSet();

private:
    opt::unordered_set<std::string> _scanDefSet;
};

struct DistributionHash {
    size_t operator()(const DistributionAndProjections& distributionAndProjections) const;
};

using DistributionSet = opt::unordered_set<DistributionAndProjections, DistributionHash>;

/**
 * Logical property which specifies promising projections and distributions to attempt to enforce
 * during physical optimization. For example, a group containing a GroupByNode would add hash
 * partitioning on the group-by projections.
 */
class DistributionAvailability final : public LogicalPropertyTag {
public:
    DistributionAvailability(DistributionSet distributionSet);

    bool operator==(const DistributionAvailability& other) const;

    const DistributionSet& getDistributionSet() const;
    DistributionSet& getDistributionSet();

private:
    DistributionSet _distributionSet;
};

}  // namespace mongo::optimizer::properties
