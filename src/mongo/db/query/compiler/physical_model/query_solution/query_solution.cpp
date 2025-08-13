/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_helpers.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include <algorithm>
#include <queue>
#include <tuple>
#include <vector>

#include <s2cellid.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/range/adaptor/argument_fwd.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {

namespace {

namespace wcp = ::mongo::wildcard_planning;

// Create an ordred interval list which represents the bounds for all BSON elements of type String,
// Object, or Array.
OrderedIntervalList buildStringBoundsOil(const std::string& keyName) {
    OrderedIntervalList ret;
    ret.name = keyName;

    BSONObjBuilder strBob;
    strBob.appendMinForType("", stdx::to_underlying(BSONType::string));
    strBob.appendMaxForType("", stdx::to_underlying(BSONType::string));
    ret.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(strBob.obj(), BoundInclusion::kIncludeStartKeyOnly));

    BSONObjBuilder objBob;
    objBob.appendMinForType("", stdx::to_underlying(BSONType::object));
    objBob.appendMaxForType("", stdx::to_underlying(BSONType::object));
    ret.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(objBob.obj(), BoundInclusion::kIncludeStartKeyOnly));

    BSONObjBuilder arrBob;
    arrBob.appendMinForType("", stdx::to_underlying(BSONType::array));
    arrBob.appendMaxForType("", stdx::to_underlying(BSONType::array));
    ret.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(arrBob.obj(), BoundInclusion::kIncludeStartKeyOnly));

    return ret;
}

bool rangeCanContainString(const BSONElement& startKey,
                           const BSONElement& endKey,
                           BoundInclusion boundInclusion) {
    OrderedIntervalList stringBoundsOil = buildStringBoundsOil("");
    OrderedIntervalList rangeOil;
    BSONObjBuilder bob;
    bob.appendAs(startKey, "");
    bob.appendAs(endKey, "");
    rangeOil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(bob.obj(), boundInclusion));

    IndexBoundsBuilder::intersectize(rangeOil, &stringBoundsOil);
    return !stringBoundsOil.intervals.empty();
}

// Helper for 'getAllSecondaryNamespaces' that deduplicates namespaces.
void getAllSecondaryNamespacesHelper(const QuerySolutionNode* qsn,
                                     const NamespaceString& mainNss,
                                     std::set<NamespaceString>& secondaryNssSet) {
    if (!qsn) {
        return;
    }

    if (auto eqLookupNode = dynamic_cast<const EqLookupNode*>(qsn)) {
        NamespaceString nss(eqLookupNode->foreignCollection);
        if (nss != mainNss) {
            secondaryNssSet.emplace(std::move(nss));
        }
    }

    for (auto&& child : qsn->children) {
        getAllSecondaryNamespacesHelper(child.get(), mainNss, secondaryNssSet);
    }
}
}  // namespace

bool ProvidedSortSet::contains(BSONObj input) const {
    auto sortPatternItr = BSONObjIterator(_baseSortPattern);
    for (auto&& inputElement : input) {
        // Remove all the fields that are present in '_ignoredFields' from the sort pattern object
        // since they do not contribute to changing the output order.
        if (_ignoredFields.count(inputElement.fieldName())) {
            continue;
        }
        if (SimpleBSONElementComparator::kInstance.evaluate(inputElement == *sortPatternItr)) {
            ++sortPatternItr;
            continue;
        }
        return false;
    }
    return true;
}

string QuerySolutionNode::toString() const {
    str::stream ss;
    appendToString(&ss, 0);
    return ss;
}

// static
void QuerySolutionNode::addIndent(str::stream* ss, int level) {
    for (int i = 0; i < level; ++i) {
        *ss << "---";
    }
}

void QuerySolutionNode::addCommon(str::stream* ss, int indent) const {
    addIndent(ss, indent + 1);
    *ss << "nodeId = " << _nodeId << '\n';
    addIndent(ss, indent + 1);
    *ss << "fetched = " << fetched() << '\n';
    addIndent(ss, indent + 1);
    *ss << "sortedByDiskLoc = " << sortedByDiskLoc() << '\n';
    addIndent(ss, indent + 1);
    *ss << "providedSorts = {" << providedSorts().debugString() << "}" << '\n';
}

bool QuerySolutionNode::hasNode(StageType type) const {
    if (type == getType()) {
        return true;
    }

    for (auto&& child : children) {
        if (child->hasNode(type)) {
            return true;
        }
    }

    return false;
}

bool QuerySolutionNode::isEligibleForPlanCache() const {
    if (!eligibleForPlanCache) {
        return false;
    }

    for (auto&& child : children) {
        if (!child->isEligibleForPlanCache()) {
            return false;
        }
    }

    return true;
}

std::pair<const QuerySolutionNode*, size_t> QuerySolutionNode::getFirstNodeByType(
    StageType type) const {
    const QuerySolutionNode* result = nullptr;
    size_t count = 0;
    if (getType() == type) {
        result = this;
        count++;
    }

    for (auto&& child : children) {
        auto [subTreeResult, subTreeCount] = child->getFirstNodeByType(type);
        if (!result) {
            result = subTreeResult;
        }
        count += subTreeCount;
    }

    return {result, count};
}

std::string QuerySolution::summaryString() const {
    tassert(5968205, "QuerySolutionNode cannot be null in this QuerySolution", _root);

    StringBuilder sb;
    bool seenLeaf = false;
    std::queue<const QuerySolutionNode*> queue;
    queue.push(_root.get());

    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();

        if (node->children.empty()) {
            if (seenLeaf) {
                sb << ", ";
            } else {
                seenLeaf = true;
            }

            sb << nodeStageTypeToString(node);

            switch (node->getType()) {
                case STAGE_COUNT_SCAN: {
                    auto csn = static_cast<const CountScanNode*>(node);
                    const KeyPattern keyPattern{csn->index.keyPattern};
                    sb << " " << keyPattern;
                    break;
                }
                case STAGE_DISTINCT_SCAN: {
                    auto dn = static_cast<const DistinctNode*>(node);
                    const KeyPattern keyPattern{dn->index.keyPattern};
                    sb << " " << keyPattern;
                    break;
                }
                case STAGE_GEO_NEAR_2D: {
                    auto geo2d = static_cast<const GeoNear2DNode*>(node);
                    const KeyPattern keyPattern{geo2d->index.keyPattern};
                    sb << " " << keyPattern;
                    break;
                }
                case STAGE_GEO_NEAR_2DSPHERE: {
                    auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
                    const KeyPattern keyPattern{geo2dsphere->index.keyPattern};
                    sb << " " << keyPattern;
                    break;
                }
                case STAGE_IXSCAN: {
                    auto ixn = static_cast<const IndexScanNode*>(node);
                    const KeyPattern keyPattern{ixn->index.keyPattern};
                    sb << " " << keyPattern;
                    break;
                }
                case STAGE_TEXT_MATCH: {
                    auto tn = static_cast<const TextMatchNode*>(node);
                    const KeyPattern keyPattern{tn->indexPrefix};
                    sb << " " << keyPattern;
                    break;
                }
                default:
                    break;
            }
        }

        for (auto&& child : node->children) {
            queue.push(child.get());
        }
    }

    return sb.str();
}

void QuerySolution::assignNodeIds(QsnIdGenerator& idGenerator, QuerySolutionNode& node) {
    for (auto&& child : node.children) {
        assignNodeIds(idGenerator, *child);
    }
    node._nodeId = idGenerator.generate();
}

void QuerySolution::extendWith(std::unique_ptr<QuerySolutionNode> extensionRoot) {
    auto current = extensionRoot.get();
    if (current == nullptr || current->getType() == StageType::STAGE_SENTINEL) {
        // Nothing to do for a trivial extension.
        return;
    }

    QuerySolutionNode* parentOfSentinel = nullptr;
    while (current->getType() != StageType::STAGE_SENTINEL) {
        parentOfSentinel = current;
        tassert(5842801,
                "Cannot find the sentinel node in the extension tree",
                !parentOfSentinel->children.empty());

        // At the moment, we only extend a solution plan with a tree for $group stage(s), which have
        // exactly one child. We'll replace the left-most branch descent with a full tree traversal,
        // if/when it becomes necessary.
        tassert(5842800,
                "Only chain extension trees are supported",
                parentOfSentinel->children.size() == 1);
        current = parentOfSentinel->children[0].get();
    }
    QuerySolutionNode* oldRoot = _root.get();
    parentOfSentinel->children[0] = std::move(_root);
    setRoot(std::move(extensionRoot));
    // setRoot may re-assign node ids, so we assign this value after calling setRoot.
    _unextendedRootId = oldRoot->nodeId();
}

void QuerySolution::setRoot(std::unique_ptr<QuerySolutionNode> root) {
    uassert(6882300, "QuerySolutionNode must be non null", root);
    _root = std::move(root);
    _enumeratorExplainInfo.hitScanLimit = _root->getScanLimit();

    QsnIdGenerator idGenerator;
    assignNodeIds(idGenerator, *_root);
}

std::vector<NamespaceStringOrUUID> QuerySolution::getAllSecondaryNamespaces(
    const NamespaceString& mainNss) {
    std::set<NamespaceString> secondaryNssSet;
    getAllSecondaryNamespacesHelper(_root.get(), mainNss, secondaryNssSet);
    return {secondaryNssSet.begin(), secondaryNssSet.end()};
}

std::pair<const QuerySolutionNode*, size_t> QuerySolution::getFirstNodeByType(
    StageType type) const {
    return _root->getFirstNodeByType(type);
}

//
// CollectionScanNode
//
CollectionScanNode::CollectionScanNode()
    : clusteredIndex(boost::none), hasCompatibleCollation(false), tailable(false), direction(1) {}

void CollectionScanNode::computeProperties() {
    if (clusteredIndex && hasCompatibleCollation) {
        auto sort = clustered_util::getSortPattern(*clusteredIndex);
        if (direction == -1) {
            // If we are scanning the collection in the descending direction, we provide the reverse
            // sort order.
            sort = QueryPlannerCommon::reverseSortObj(sort);
        }
        sortSet = ProvidedSortSet(sort);
    }
}

void CollectionScanNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    if (doClusteredCollectionScanClassic() || doClusteredCollectionScanSbe()) {
        *ss << "CLUSTERED_IDXSCAN\n";
    } else {
        *ss << "COLLSCAN\n";
    }
    addIndent(ss, indent + 1);
    *ss << "ns = " << toStringForLogging(nss) << '\n';
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << "filter = " << filter->debugString();
    }
    addCommon(ss, indent);
}

std::unique_ptr<QuerySolutionNode> CollectionScanNode::clone() const {
    auto copy = std::make_unique<CollectionScanNode>();
    cloneBaseData(copy.get());

    copy->nss = this->nss;
    copy->tailable = this->tailable;
    copy->direction = this->direction;
    copy->isClustered = this->isClustered;
    copy->minRecord = this->minRecord;
    copy->maxRecord = this->maxRecord;
    copy->clusteredIndex = this->clusteredIndex;
    copy->isOplog = this->isOplog;
    copy->shouldTrackLatestOplogTimestamp = this->shouldTrackLatestOplogTimestamp;
    copy->assertTsHasNotFallenOff = this->assertTsHasNotFallenOff;
    copy->shouldWaitForOplogVisibility = this->shouldWaitForOplogVisibility;
    copy->clusteredIndex = this->clusteredIndex;
    copy->hasCompatibleCollation = this->hasCompatibleCollation;
    return copy;
}

//
// VirtualScanNode
//

VirtualScanNode::VirtualScanNode(std::vector<BSONArray> docs,
                                 ScanType scanType,
                                 bool hasRecordId,
                                 BSONObj indexKeyPattern)
    : docs(std::move(docs)),
      scanType(scanType),
      hasRecordId(hasRecordId),
      indexKeyPattern(std::move(indexKeyPattern)) {}

void VirtualScanNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "VIRTUAL_SCAN\n";
    addIndent(ss, indent + 1);
    *ss << "nDocuments = " << docs.size();
    addIndent(ss, indent + 1);
    *ss << "hasRecordId = " << hasRecordId;
    addCommon(ss, indent);
    *ss << "scanType = " << static_cast<size_t>(scanType);
    addCommon(ss, indent);
    *ss << "indexKeyPattern = " << indexKeyPattern;
    addCommon(ss, indent);
}

std::unique_ptr<QuerySolutionNode> VirtualScanNode::clone() const {
    auto copy = std::make_unique<VirtualScanNode>(docs, scanType, hasRecordId, indexKeyPattern);
    cloneBaseData(copy.get());
    return copy;
}

//
// AndHashNode
//

AndHashNode::AndHashNode() {}

AndHashNode::~AndHashNode() {}

void AndHashNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "AND_HASH\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString() << '\n';
    }
    addCommon(ss, indent);
    for (size_t i = 0; i < children.size(); ++i) {
        addIndent(ss, indent + 1);
        *ss << "Child " << i << ":\n";
        children[i]->appendToString(ss, indent + 1);
    }
}

bool AndHashNode::fetched() const {
    // Any WSM output from this stage came from all children stages.  If any child provides
    // fetched data, we merge that fetched data into the WSM we output.
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i]->fetched()) {
            return true;
        }
    }
    return false;
}

FieldAvailability AndHashNode::getFieldAvailability(const string& field) const {
    // A field can be provided by any of the children.
    auto result = FieldAvailability::kNotProvided;
    for (size_t i = 0; i < children.size(); ++i) {
        result = std::max(result, children[i]->getFieldAvailability(field));
    }
    return result;
}

std::unique_ptr<QuerySolutionNode> AndHashNode::clone() const {
    auto copy = std::make_unique<AndHashNode>();
    cloneBaseData(copy.get());
    return copy;
}

//
// AndSortedNode
//

AndSortedNode::AndSortedNode() {}

AndSortedNode::~AndSortedNode() {}

void AndSortedNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "AND_SORTED\n";
    addCommon(ss, indent);
    for (size_t i = 0; i < children.size(); ++i) {
        addIndent(ss, indent + 1);
        *ss << "Child " << i << ":\n";
        children[i]->appendToString(ss, indent + 1);
    }
}

bool AndSortedNode::fetched() const {
    // Any WSM output from this stage came from all children stages.  If any child provides
    // fetched data, we merge that fetched data into the WSM we output.
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i]->fetched()) {
            return true;
        }
    }
    return false;
}

FieldAvailability AndSortedNode::getFieldAvailability(const string& field) const {
    // A field can be provided by any of the children.
    auto result = FieldAvailability::kNotProvided;
    for (size_t i = 0; i < children.size(); ++i) {
        result = std::max(result, children[i]->getFieldAvailability(field));
    }
    return result;
}

std::unique_ptr<QuerySolutionNode> AndSortedNode::clone() const {
    auto copy = std::make_unique<AndSortedNode>();
    cloneBaseData(copy.get());
    return copy;
}

//
// OrNode
//

OrNode::OrNode() : dedup(true) {}

OrNode::~OrNode() {}

void OrNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "OR\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString() << '\n';
    }
    addCommon(ss, indent);
    for (size_t i = 0; i < children.size(); ++i) {
        addIndent(ss, indent + 1);
        *ss << "Child " << i << ":\n";
        children[i]->appendToString(ss, indent + 2);
        *ss << '\n';
    }
}

bool OrNode::fetched() const {
    // Any WSM output from this stage came exactly one child stage.  Given that we don't know
    // what child stage it came from, we require that all children provide fetched data in order
    // to guarantee that our output is fetched.
    for (size_t i = 0; i < children.size(); ++i) {
        if (!children[i]->fetched()) {
            return false;
        }
    }
    return true;
}

/**
 * Any WSM output from this stage came from exactly one child stage.  Therefore, if
 * we want to guarantee that any output has a certain field, all of our children must
 * have that field.
 */
FieldAvailability OrNode::getFieldAvailability(const string& field) const {
    auto result = FieldAvailability::kFullyProvided;
    for (size_t i = 0; i < children.size(); ++i) {
        result = std::min(result, children[i]->getFieldAvailability(field));
    }
    return result;
}

std::unique_ptr<QuerySolutionNode> OrNode::clone() const {
    auto copy = std::make_unique<OrNode>();
    cloneBaseData(copy.get());

    copy->dedup = this->dedup;

    return copy;
}

//
// MergeSortNode
//

MergeSortNode::MergeSortNode() : dedup(true) {}

MergeSortNode::~MergeSortNode() {}

void MergeSortNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "MERGE_SORT\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString() << '\n';
    }
    addCommon(ss, indent);
    for (size_t i = 0; i < children.size(); ++i) {
        addIndent(ss, indent + 1);
        *ss << "Child " << i << ":\n";
        children[i]->appendToString(ss, indent + 2);
        *ss << '\n';
    }
}

bool MergeSortNode::fetched() const {
    // Any WSM output from this stage came exactly one child stage.  Given that we don't know
    // what child stage it came from, we require that all children provide fetched data in order
    // to guarantee that our output is fetched.
    for (size_t i = 0; i < children.size(); ++i) {
        if (!children[i]->fetched()) {
            return false;
        }
    }
    return true;
}

/**
 * Any WSM output from this stage came from exactly one child stage.  Therefore, if
 * we want to guarantee that any output has a certain field, all of our children must
 * have that field.
 */
FieldAvailability MergeSortNode::getFieldAvailability(const string& field) const {
    auto result = FieldAvailability::kFullyProvided;
    for (size_t i = 0; i < children.size(); ++i) {
        result = std::min(result, children[i]->getFieldAvailability(field));
    }
    return result;
}

std::unique_ptr<QuerySolutionNode> MergeSortNode::clone() const {
    auto copy = std::make_unique<MergeSortNode>();
    cloneBaseData(copy.get());

    copy->dedup = this->dedup;
    copy->sort = this->sort;

    return copy;
}

//
// FetchNode
//

void FetchNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "FETCH\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        StringBuilder sb;
        *ss << "filter:\n";
        filter->debugString(sb, indent + 2);
        *ss << sb.str();
    }
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> FetchNode::clone() const {
    auto copy = std::make_unique<FetchNode>();
    cloneBaseData(copy.get());
    return copy;
}

//
// IndexScanNode
//

IndexScanNode::IndexScanNode(IndexEntry indexEntry)
    : index(std::move(indexEntry)),
      direction(1),
      addKeyMetadata(false),
      shouldDedup(index.multikey),
      queryCollator(nullptr) {}

void IndexScanNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "IXSCAN\n";
    addIndent(ss, indent + 1);
    *ss << "indexName = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern << '\n';
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << "filter = " << filter->debugString();
    }
    addIndent(ss, indent + 1);
    *ss << "direction = " << direction << '\n';
    addIndent(ss, indent + 1);
    *ss << "bounds = " << bounds.toString(index.collator != nullptr) << '\n';
    if (!iets.empty()) {
        addIndent(ss, indent + 1);
        *ss << "iets = " << ietsToString(index, iets) << '\n';
    }
    addCommon(ss, indent);
}

FieldAvailability IndexScanNode::getFieldAvailability(const string& field) const {
    // A $** index whose bounds overlap the object type bracket cannot provide covering, since the
    // index only contains the leaf nodes along each of the object's subpaths.
    if (index.type == IndexType::INDEX_WILDCARD && wcp::isWildcardObjectSubpathScan(this)) {
        return FieldAvailability::kNotProvided;
    }

    // The index is multikey but does not have any path-level multikeyness information. Such indexes
    // can never provide covering.
    if (index.multikey && index.multikeyPaths.empty()) {
        return FieldAvailability::kNotProvided;
    }

    // Compound hashed indexes can be covered when the projection is not on the hashed field. Other
    // custom index access methods may return non-exact key data - this function is currently used
    // for covering exact key data only.
    auto indexPluginName = IndexNames::findPluginName(index.keyPattern);
    switch (IndexNames::nameToType(indexPluginName)) {
        case IndexType::INDEX_BTREE:
        case IndexType::INDEX_HASHED:
            break;
        default:
            // All other index types provide no fields.
            return FieldAvailability::kNotProvided;
    }

    // If the index has a non-simple collation and we have collation keys inside 'field', then this
    // index scan does not provide that field (and the query cannot be covered).
    if (index.collator) {
        std::set<StringData> collatedFields = getFieldsWithStringBounds(bounds, index.keyPattern);
        if (collatedFields.find(field) != collatedFields.end()) {
            return FieldAvailability::kNotProvided;
        }
    }

    size_t keyPatternFieldIndex = 0;
    for (auto&& elt : index.keyPattern) {
        // For $** indexes, the keyPattern is prefixed by a virtual field, '$_path'. We therefore
        // skip the first keyPattern field when deciding whether we can provide the requested field.
        if (index.type == IndexType::INDEX_WILDCARD &&
            keyPatternFieldIndex == index.wildcardFieldPos - 1) {
            tassert(7246701,
                    "Expected element at the position before the wildcard field to be the virtual "
                    "field $_path.",
                    elt.fieldNameStringData() == "$_path"_sd);
            ++keyPatternFieldIndex;
            continue;
        }
        // The index can provide this field if the requested path appears in the index key pattern,
        // and that path has no multikey components. We can't cover a field that has multikey
        // components because the index keys contain individual array elements, and we can't
        // reconstitute the array from the index keys in the right order. In order for the field to
        // be fully provided by the scan, it must be ascending (1) or descending (-1).
        if (field == elt.fieldName() &&
            (!index.multikey || index.multikeyPaths[keyPatternFieldIndex].empty())) {
            // We already know that the index is either ascending, descending or hashed. If the
            // field is hashed, we only provide hashed value.
            return elt.isNumber() ? FieldAvailability::kFullyProvided
                                  : FieldAvailability::kHashedValueProvided;
        }
        ++keyPatternFieldIndex;
    }
    return FieldAvailability::kNotProvided;
}

bool IndexScanNode::sortedByDiskLoc() const {
    // Indices use RecordId as an additional key after the actual index key.
    // Therefore, if we're only examining one index key, the output is sorted
    // by RecordId.

    // If it's a simple range query, it's easy to determine if the range is a point.
    if (bounds.isSimpleRange) {
        return 0 == bounds.startKey.woCompare(bounds.endKey, index.keyPattern);
    }

    // If it's a more complex bounds query, we make sure that each field is a point.
    for (size_t i = 0; i < bounds.fields.size(); ++i) {
        const OrderedIntervalList& oil = bounds.fields[i];
        if (1 != oil.intervals.size()) {
            return false;
        }
        const Interval& interval = oil.intervals[0];
        if (0 != interval.start.woCompare(interval.end, false)) {
            return false;
        }
    }

    return true;
}

// static
std::set<StringData> IndexScanNode::getFieldsWithStringBounds(const IndexBounds& inputBounds,
                                                              const BSONObj& indexKeyPattern) {
    // Produce a copy of the bounds which are all ascending, as we can only compute intersections
    // of ascending bounds.
    IndexBounds bounds = inputBounds.forwardize();

    BSONObjIterator keyPatternIterator(indexKeyPattern);

    if (bounds.isSimpleRange) {
        // With a simple range, the only cases we can say for sure do not contain strings
        // are those with point bounds.
        BSONObjIterator startKeyIterator(bounds.startKey);
        BSONObjIterator endKeyIterator(bounds.endKey);
        while (keyPatternIterator.more() && startKeyIterator.more() && endKeyIterator.more()) {
            BSONElement startKey = startKeyIterator.next();
            BSONElement endKey = endKeyIterator.next();
            if (SimpleBSONElementComparator::kInstance.evaluate(startKey != endKey) ||
                CollationIndexKey::isCollatableType(startKey.type())) {
                BoundInclusion boundInclusion = bounds.boundInclusion;
                if (startKeyIterator.more()) {
                    boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
                }
                if (!rangeCanContainString(startKey, endKey, boundInclusion)) {
                    // If the first non-point range cannot contain strings, we don't need to
                    // add it to the return set.
                    keyPatternIterator.next();
                }

                // Any remaining keys could have strings.
                std::set<StringData> ret;
                while (keyPatternIterator.more()) {
                    ret.insert(keyPatternIterator.next().fieldNameStringData());
                }
                return ret;
            }

            keyPatternIterator.next();
        }

        return std::set<StringData>{};
    }

    std::set<StringData> ret;
    invariant(bounds.fields.size() == static_cast<size_t>(indexKeyPattern.nFields()));
    for (const auto& oil : bounds.fields) {
        invariant(keyPatternIterator.more());
        BSONElement el = keyPatternIterator.next();
        OrderedIntervalList intersection = buildStringBoundsOil(el.fieldName());
        IndexBoundsBuilder::intersectize(oil, &intersection);
        if (!intersection.intervals.empty()) {
            ret.insert(el.fieldNameStringData());
        }
    }

    return ret;
}

namespace {
std::set<StringData> getMultikeyFields(const BSONObj& keyPattern,
                                       const MultikeyPaths& multikeyPaths) {
    std::set<StringData> multikeyFields;
    size_t i = 0;
    for (auto&& elem : keyPattern) {
        if (!multikeyPaths[i].empty()) {
            multikeyFields.insert(elem.fieldNameStringData());
        }
        ++i;
    }
    return multikeyFields;
}

/**
 * Returns true if the index scan described by 'multikeyFields' and 'bounds' can legally provide the
 * 'sortPatternComponent' field, or false if the sort cannot be provided. A multikey index cannot
 * provide a sort if either of the following is true: 1) the sort spec includes a multikey field
 * that has bounds other than [minKey, maxKey], 2) there are bounds other than [minKey, maxKey] over
 * a multikey field which share a path prefix with a component of the sort pattern. These cases are
 * further explained in SERVER-31898.
 */
bool confirmBoundsProvideSortComponentGivenMultikeyness(
    StringData sortPatternComponent,
    const IndexBounds& bounds,
    const std::set<StringData>& multikeyFields) {
    // Forwardize the bounds to correctly apply checks to descending sorts and well as ascending
    // sorts.
    const auto ascendingBounds = bounds.forwardize();
    const auto& fields = ascendingBounds.fields;
    if (multikeyFields.count(sortPatternComponent) == 0) {
        // If this component of the sort pattern (which must be one of the components of
        // the index spec) is not multikey, we don't need additional checks.
        return true;
    }

    // Return false if the bounds are specified as a simple range. As a future improvement, we could
    // extend this optimization to allow simple multikey scans to provide a sort.
    if (ascendingBounds.isSimpleRange) {
        return false;
    }

    // Checks if the 'sortPatternComponent' has [MinKey, MaxKey].
    for (auto&& intervalList : fields) {
        if (sortPatternComponent == intervalList.name && !intervalList.isMinToMax()) {
            return false;
        }
    }

    // Checks if there is a shared path prefix between the bounds and the sort pattern of
    // multikey fields.
    FieldRef refName(sortPatternComponent);
    for (const auto& intervalList : fields) {
        // Ignores the prefix if the bounds are [MinKey, MaxKey] or if the field is not
        // multikey.
        if (intervalList.isMinToMax() ||
            (multikeyFields.find(intervalList.name) == multikeyFields.end())) {
            continue;
        }
        FieldRef boundsPath(intervalList.name);
        const auto commonPrefixSize = boundsPath.commonPrefixSize(refName);
        // The interval list name and the sort pattern name will never be equal at this point.
        // This is because if they are equal and do not have [minKey, maxKey] bounds, we would
        // already have bailed out of the function. If they do have [minKey, maxKey] bounds,
        // they will be skipped in the check for [minKey, maxKey] bounds above.
        invariant(refName != boundsPath);
        // Checks if there's a common prefix between the interval list name and the sort pattern
        // name.
        if (commonPrefixSize > 0) {
            return false;
        }
    }
    return true;
}

std::set<std::string> extractEqualityFields(
    const IndexBounds& bounds,
    const IndexEntry& index,
    const std::vector<interval_evaluation_tree::IET>* iets) {
    std::set<std::string> equalityFields;

    // Find all equality predicate fields.
    if (!bounds.isSimpleRange) {
        // Figure out how many fields are point intervals.
        for (size_t i = 0; i < bounds.fields.size(); ++i) {
            const OrderedIntervalList& oil = bounds.fields[i];
            if (oil.intervals.size() != 1) {
                continue;
            }
            const Interval& ival = oil.intervals[0];
            if (!ival.isPoint()) {
                continue;
            }

            // If we have an IET for this field in our index bounds, we determine whether it
            // guarantees that, upon evaluation, we will have point bounds for the corresponding
            // field in our index. In particular, if the IET evaluates to a ConstNode, an equality
            // EvalNode, or an ExplodeNode, then this field represents an equality.
            if (iets && !iets->empty()) {
                const auto& iet = (*iets)[i];
                auto mustBePointInterval = [&]() {
                    if (const auto* constNode = iet.cast<interval_evaluation_tree::ConstNode>();
                        constNode) {
                        // If we have 'constNodePtr', it must be the case that the interval that it
                        // contains is the same as 'ival'.
                        tassert(7426201,
                                "'constNode' must have a single interval",
                                constNode->oil.intervals.size() == 1);
                        tassert(
                            7426202,
                            "'constNode' must have the same point interval as the one in 'bounds'",
                            constNode->oil.intervals[0].equals(ival));
                        return true;
                    } else if (const auto* evalNode =
                                   iet.cast<interval_evaluation_tree::EvalNode>();
                               evalNode) {
                        if (evalNode->matchType() == MatchExpression::MatchType::EQ) {
                            return true;
                        }
                    } else if (const auto* explodeNode =
                                   iet.cast<interval_evaluation_tree::ExplodeNode>();
                               explodeNode) {
                        return true;
                    }
                    return false;
                }();

                if (!mustBePointInterval) {
                    continue;
                }
            }
            equalityFields.insert(oil.name);
        }
    } else {
        tassert(7426200,
                "Should not have IETs when evaluating min/max index bounds",
                !iets || iets->empty());
        BSONObjIterator keyIter(index.keyPattern);
        BSONObjIterator startIter(bounds.startKey);
        BSONObjIterator endIter(bounds.endKey);
        while (keyIter.more() && startIter.more() && endIter.more()) {
            BSONElement key = keyIter.next();
            if (SimpleBSONElementComparator::kInstance.evaluate(startIter.next() ==
                                                                endIter.next())) {
                equalityFields.insert(key.fieldName());
            }
        }
    }
    return equalityFields;
}

/**
 * Returns a 'ProvidedSortSet' with the sort orders provided by an index scan over 'index',
 * with the given 'bounds' and 'direction'.
 */
ProvidedSortSet computeSortsForScan(const IndexEntry& index,
                                    int direction,
                                    const IndexBounds& bounds,
                                    const CollatorInterface* queryCollator,
                                    const std::set<StringData>& multikeyFields,
                                    const std::vector<interval_evaluation_tree::IET>* iets) {
    // If 'index' is the result of expanding a wildcard index, then its key pattern should look like
    // {<optional non-wildcard prefix>, $_path: 1, <field>: 1, <optional non-wildcard suffix>}. The
    // "$_path" field stores the path(s) to be examined inside the wildcard index, as opposed to
    // real user data. We shouldn't report any sort orders including "$_path". In fact, $-prefixed
    // path components are illegal in queries in most contexts, so misinterpreting this as a path in
    // user-data could trigger subsequent assertions.
    //
    // A compound wildcard index can provide sorts on non-wildcard prefix fields. For example, index
    // {a: 1, "b.$**": 1} can provide a sort on "a".
    //
    // A compound wildcard index can also provide sorts on the wildcard field and suffix fields
    // under some conditions. For example, index {"b.$**": 1, c: 1} can provide the sort {c: 1}
    // given query {"b.d": {$eq: 1}}.
    //
    // However, there are restrictions on the wildcard + suffix fields support:
    // - Example: index {"b.$**": 1, c: 1} with query {"b.d": {$exists: true}} cannot provide a sort
    //   on {"b.d": 1} or {"b.d": 1, c: 1}. That's because the bounds for "$_path" are [["b.d",
    //   "b.d"], ["b.d.", "b.d/")], where the second bound is there so that keys from documents
    //   where "b.d" is a nested object are in bounds. This does not produce sorted order for "b.d",
    //   since all of the nested objects will be returned last by the scan.
    // - The same is true when the query does not contain a predicate on a wildcard field. Example:
    //   index {a: 1, "b.$**": 1, c: 1} cannot provide a sort on {"b.d": 1, c: 1} if there is no
    //   predicate on "b.d". That's because the bounds for "$_path" are [[MinKey, MinKey], ["",
    //   {})]. This captures both documents that don't have a "b" field and documents that do. As
    //   above, this scan will not return documents in "b.d" sorted order.
    // In summary, the restrction is: when the bounds for "$_path" are not an equality, we truncate
    // the provided sort starting with the "$_path" field.
    const bool isWildcardIndex = index.type == IndexType::INDEX_WILDCARD;
    if (isWildcardIndex) {
        tassert(7246700,
                "The bounds did not have as many fields as the key pattern.",
                static_cast<size_t>(index.keyPattern.nFields()) == bounds.fields.size());
    }

    //
    // There are two buckets of field names "equality fields" and "unsupported fields". The
    // "equality fields" are those over which we have an equality predicate. These fields can
    // optionally be ignored when checking whether a pattern is provided or not. The "unsupported
    // fields" are fields for which we cannot provide a sort. Currently we cannot provide sort when
    // the field is collated or multikey.
    //
    // The intersection of the "equality fields" and "unsupported fields" (called 'ignoreFields')
    // can simply be ignored. The index scan does not provide these fields in sorted order (e.g.
    // because of a mismatched collation), but due to the point bounds in the index scan, this
    // doesn't affect our ability to provide a sort on any subsequent fields. Fields in this
    // intersection set can simply be dropped when constructing the "base sort pattern".
    //
    // The remaining are 'unsupportedFields', which we get when we do a set subtraction of
    // the initial "unsupported fields" minus 'ignoreFields'. The index scan will never provide a
    // sort order on this field or any subsequent fields. When we encounter such a field in the
    // index key pattern, we truncate it and any later fields to form the "base sort pattern".
    //
    // When dealing with autoparameterization (that is, when 'iets' is non-empty), the requirement
    // for a field to be considered an equality field is stricter. In particular, we must be able to
    // prove that the index bounds will always yield point bounds for any future value of the input
    // parameter.
    //
    // Example, consider an index pattern {a: 1, b: 1, c: 1, d: 1},
    // - If the query predicate is {a: 1} and 'c' is a multikey field then, unsupportedFields = {c},
    // equalityFields = {a}, ignoreFields = {} and baseSortPattern = {b: 1}. Field 'a' is dropped
    // from the base sort pattern because it is an equality field. Fields 'c' and 'd' are truncted
    // from the base sort pattern because 'c' is an unsupported field.
    // - If the query predicate is {} and 'a' is a multikey field then, unsupportedFields = {a},
    // equalityFields = {}, ignoreFields = {} and baseSortPattern = {}. The entire sort pattern is
    // truncated since the first field 'a' is an unsupported field.
    // - If the query predicate is {b: 1} with 'b' is a multikey field then, unsupportedFields = {},
    // equalityFields = {}, ignoreFields = {b} and baseSortPattern = {a: 1, c: 1, d: 1}. Field 'b'
    // has to be dropped from the base sort pattern, since although we are not sorted by 'b', we
    // have point bounds on it.
    // - If the query predicate is {b: 1, c: 1} and 'b' is a multikey field then, unsupportedFields
    // = {}, equalityFields = {c}, ignoreFields = {b} and baseSortPattern = {a: 1, d: 1}. Field 'b'
    // has to be dropped from the base sort pattern, since although we are not sorted by 'b', we
    // have point bounds on it. Field 'c' is removed because of the presence of equality predicate.
    // So we can provide sorts {a: 1, d: 1}, {a: 1, c: 1, d: 1} but not sort patterns that include
    // field 'b'.
    //
    std::set<std::string> equalityFields = extractEqualityFields(bounds, index, iets);
    std::set<StringData> unsupportedFields;
    std::set<StringData> ignoreFields;
    if (!CollatorInterface::collatorsMatch(queryCollator, index.collator)) {
        for (auto&& collatedField :
             IndexScanNode::getFieldsWithStringBounds(bounds, index.keyPattern)) {
            if (equalityFields.count(std::string{collatedField})) {
                ignoreFields.insert(collatedField);
                equalityFields.erase(std::string{collatedField});
            } else {
                unsupportedFields.insert(collatedField);
            }
        }
    }
    if (index.multikey) {
        for (auto&& multikeyField : multikeyFields) {
            if (!confirmBoundsProvideSortComponentGivenMultikeyness(
                    multikeyField, bounds, multikeyFields)) {
                if (equalityFields.count(std::string{multikeyField})) {
                    ignoreFields.insert(multikeyField);
                    equalityFields.erase(std::string{multikeyField});
                } else {
                    unsupportedFields.insert(multikeyField);
                }
            }
        }
    }

    // Iterate through the index key pattern and determine the "maximal sort" that this index can
    // provide. The constructed sort pattern is in normalized form (all elements have value 1 or
    // -1). Very similar to QueryPlannerAnalysis::getSortPattern(), with the additional logic for
    // wildcard indexes, collation, and multikey indexes, as described above.
    const bool reverse = (direction == -1);
    BSONObjBuilder sortBob;
    BSONObjIterator kpIt(index.keyPattern);
    while (kpIt.more()) {
        BSONElement elt = kpIt.next();
        const auto& fieldName = elt.fieldNameStringData();

        // Exclude the "$_path" field, which shouldn't end up in the sort order.
        if (isWildcardIndex && fieldName == "$_path"_sd) {
            // If the bounds for the "$_path" field are multi-interval then we can't provide sorts
            // on any following fields.
            tassert(7767200,
                    "The bounds cannot be empty.",
                    bounds.fields[index.wildcardFieldPos - 1].intervals.size() > 0u);
            if (bounds.fields[index.wildcardFieldPos - 1].intervals.size() != 1u) {
                break;
            }
            // At this point we know the bounds are an equality.
            continue;
        }

        // Equality fields can be ignored for the purposes of sort order; see above.
        if (equalityFields.contains(std::string{fieldName})) {
            continue;
        }

        // "Hashed" fields (or other special kinds of index) prohibit sorts on the later fields,
        // unless there is an equality on the hashed field (handled above).
        if (elt.type() == BSONType::string) {
            break;
        }

        // Ignored fields should be excluded. The index scan does not provide these fields in sorted
        // order but we may still be able to provide a sort on subsequent fields; see comment above.
        if (ignoreFields.contains(fieldName)) {
            continue;
        }

        // Unsupported fields (due to multikeyness or collation) prohibit sorts on the later fields.
        if (unsupportedFields.contains(fieldName)) {
            break;
        }

        // The canonical check as to whether a key pattern element is "ascending" or
        // "descending" is (elt.number() >= 0). This is defined by the Ordering class.
        int sortOrder = (elt.number() >= 0) ? 1 : -1;
        if (reverse)
            sortOrder *= -1;
        sortBob.append(fieldName, sortOrder);
    }

    return ProvidedSortSet(sortBob.obj(), std::move(equalityFields));
}

/**
 * Computes sort orders for index scans, including DISTINCT_SCAN. Returns a pair where the first
 * field is 'ProvidedSortSet', which contains all the sort orders that can be provided by the index
 * scan. The second field is a set populated with the names of all fields that the index indicates
 * are multikey.
 */
std::pair<ProvidedSortSet, std::set<StringData>> computeSortsAndMultikeyPathsForScan(
    const IndexEntry& index,
    int direction,
    const IndexBounds& bounds,
    const CollatorInterface* queryCollator,
    const std::vector<interval_evaluation_tree::IET>* iets) {
    // If the index is multikey but does not have path-level multikey metadata, then this index
    // cannot provide any sorts and we need not populate 'multikeyFieldsOut'.
    if (index.multikey && index.multikeyPaths.empty()) {
        return {};
    }

    std::set<StringData> multikeyFieldsOut;
    if (index.multikey) {
        multikeyFieldsOut = getMultikeyFields(index.keyPattern, index.multikeyPaths);
    }
    return {computeSortsForScan(index, direction, bounds, queryCollator, multikeyFieldsOut, iets),
            std::move(multikeyFieldsOut)};
}
}  // namespace

void IndexScanNode::computeProperties() {
    std::tie(sortSet, multikeyFields) =
        computeSortsAndMultikeyPathsForScan(index, direction, bounds, queryCollator, &iets);
}

std::unique_ptr<QuerySolutionNode> IndexScanNode::clone() const {
    auto copy = std::make_unique<IndexScanNode>(this->index);
    cloneBaseData(copy.get());

    copy->direction = this->direction;
    copy->addKeyMetadata = this->addKeyMetadata;
    copy->bounds = this->bounds;
    copy->queryCollator = this->queryCollator;

    return copy;
}

namespace {

bool filtersAreEquivalent(const MatchExpression* lhs, const MatchExpression* rhs) {
    if (!lhs && !rhs) {
        return true;
    } else if (!lhs || !rhs) {
        return false;
    } else {
        return lhs->equivalent(rhs);
    }
}

}  // namespace

bool IndexScanNode::operator==(const IndexScanNode& other) const {
    return filtersAreEquivalent(filter.get(), other.filter.get()) && index == other.index &&
        direction == other.direction && addKeyMetadata == other.addKeyMetadata &&
        bounds == other.bounds && iets == other.iets;
}

//
// ReturnKeyNode
//

void ReturnKeyNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "RETURN_KEY\n";
    addIndent(ss, indent + 1);

    *ss << "sortKeyMetaFields = ["
        << boost::algorithm::join(
               sortKeyMetaFields |
                   boost::adaptors::transformed([](const auto& field) { return field.fullPath(); }),
               ", ");
    *ss << "]\n";
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> ReturnKeyNode::clone() const {
    return std::make_unique<ReturnKeyNode>(children[0]->clone(), std::vector(sortKeyMetaFields));
}

//
// MatchNode
//

void MatchNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "MATCH\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << "filter:\n";
        StringBuilder sb;
        filter->debugString(sb, indent + 2);
        *ss << sb.str();
    }
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> MatchNode::clone() const {
    return std::make_unique<MatchNode>(children[0]->clone(), filter ? filter->clone() : nullptr);
}

//
// UnwindNode
//

void UnwindNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "UNWIND\n";
    addIndent(ss, indent + 1);
    *ss << "preserveNullAndEmptyArrays = " << spec.preserveNullAndEmptyArrays << "\n";
    if (spec.indexPath) {
        addIndent(ss, indent + 1);
        *ss << "indexPath = " << spec.indexPath->fullPath() << "\n";
    }

    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> UnwindNode::clone() const {
    return std::make_unique<UnwindNode>(children[0]->clone(), spec);
}

//
// ReplaceRootNode
//

void ReplaceRootNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "REPLACE_ROOT\n";
    if (nullptr != newRoot) {
        addIndent(ss, indent + 1);
        *ss << "newRoot:\n";

        *ss << newRoot->serialize().toString();
    }
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> ReplaceRootNode::clone() const {
    return std::make_unique<ReplaceRootNode>(children[0]->clone(), newRoot ? newRoot : nullptr);
}

//
// ProjectionNode
//

void ProjectionNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "PROJ\n";
    addIndent(ss, indent + 1);
    *ss << "proj = " << projection_ast::astToDebugBSON(proj.root()).toString() << '\n';
    addIndent(ss, indent + 1);
    *ss << "type = " << projectionImplementationTypeToString() << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

void ProjectionNode::computeProperties() {
    invariant(children.size() == 1U);
    children[0]->computeProperties();

    // Our input sort is not necessarily maintained if we project some fields that are part of the
    // sort out.
    BSONObjBuilder prefixBob;
    const auto& inputSorts = children[0]->providedSorts();
    for (auto&& key : inputSorts.getBaseSortPattern()) {
        if (!proj.isFieldRetainedExactly(key.fieldNameStringData())) {
            break;
        }
        prefixBob.append(key);
    }
    sortSet = ProvidedSortSet(prefixBob.obj(), inputSorts.getIgnoredFields());
}

void ProjectionNode::cloneProjectionData(ProjectionNode* copy) const {
    // ProjectionNode should not populate filter. This should be a no-op.
    if (this->filter)
        copy->filter = this->filter->clone();

    copy->sortSet = this->sortSet;
}

std::unique_ptr<QuerySolutionNode> ProjectionNodeDefault::clone() const {
    auto copy = std::make_unique<ProjectionNodeDefault>(children[0]->clone(), fullExpression, proj);
    ProjectionNode::cloneProjectionData(copy.get());
    return copy;
}

std::unique_ptr<QuerySolutionNode> ProjectionNodeCovered::clone() const {
    auto copy = std::make_unique<ProjectionNodeCovered>(
        children[0]->clone(), fullExpression, proj, coveredKeyObj);
    ProjectionNode::cloneProjectionData(copy.get());
    return copy;
}

std::unique_ptr<QuerySolutionNode> ProjectionNodeSimple::clone() const {
    auto copy = std::make_unique<ProjectionNodeSimple>(children[0]->clone(), fullExpression, proj);
    ProjectionNode::cloneProjectionData(copy.get());
    return copy;
}


//
// SortKeyGeneratorNode
//

void SortKeyGeneratorNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SORT_KEY_GENERATOR\n";
    addIndent(ss, indent + 1);
    *ss << "sortSpec = " << sortSpec.toString() << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> SortKeyGeneratorNode::clone() const {
    auto copy = std::make_unique<SortKeyGeneratorNode>();
    cloneBaseData(copy.get());
    copy->sortSpec = this->sortSpec;
    return copy;
}

//
// SortNode
//

SortNode::SortNode(const SortNode& other) {
    other.cloneSortData(this);
}

void SortNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SORT\n";
    addIndent(ss, indent + 1);
    *ss << "type = " << sortImplementationTypeToString() << '\n';
    addIndent(ss, indent + 1);
    *ss << "pattern = " << pattern.toString() << '\n';
    addIndent(ss, indent + 1);
    *ss << "limit = " << limit << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

void SortNode::cloneSortData(SortNode* copy) const {
    cloneBaseData(copy);
    copy->pattern = this->pattern;
    copy->limit = this->limit;
    copy->canBeParameterized = this->canBeParameterized;
    copy->addSortKeyMetadata = this->addSortKeyMetadata;
}

std::unique_ptr<QuerySolutionNode> SortNodeDefault::clone() const {
    return std::make_unique<SortNodeDefault>(*this);
}

std::unique_ptr<QuerySolutionNode> SortNodeSimple::clone() const {
    return std::make_unique<SortNodeSimple>(*this);
}

uint64_t SortNode::_loadMaxMemoryUsageBytes() {
    return loadMemoryLimit(StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes);
}

//
// LimitNode
//

LimitNode::LimitNode(const LimitNode& other) {
    other.cloneBaseData(this);
    limit = other.limit;
    canBeParameterized = other.canBeParameterized;
}

void LimitNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "LIMIT\n";
    addIndent(ss, indent + 1);
    *ss << "limit = " << limit << '\n';
    addIndent(ss, indent + 1);
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> LimitNode::clone() const {
    return std::make_unique<LimitNode>(*this);
}

//
// SkipNode
//

SkipNode::SkipNode(const SkipNode& other) {
    other.cloneBaseData(this);
    skip = other.skip;
    canBeParameterized = other.canBeParameterized;
}

void SkipNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SKIP\n";
    addIndent(ss, indent + 1);
    *ss << "skip= " << skip << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> SkipNode::clone() const {
    return std::make_unique<SkipNode>(*this);
}

//
// GeoNear2DNode
//

void GeoNear2DNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "GEO_NEAR_2D\n";
    addIndent(ss, indent + 1);
    *ss << "name = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern.toString() << '\n';
    addCommon(ss, indent);
    *ss << "nearQuery = " << nq->toString() << '\n';
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString();
    }
}

std::unique_ptr<QuerySolutionNode> GeoNear2DNode::clone() const {
    auto copy = std::make_unique<GeoNear2DNode>(this->index);
    cloneBaseData(copy.get());

    copy->nq = this->nq;
    copy->baseBounds = this->baseBounds;
    copy->addPointMeta = this->addPointMeta;
    copy->addDistMeta = this->addDistMeta;

    return copy;
}

//
// GeoNear2DSphereNode
//

void GeoNear2DSphereNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "GEO_NEAR_2DSPHERE\n";
    addIndent(ss, indent + 1);
    *ss << "name = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern.toString() << '\n';
    addCommon(ss, indent);
    *ss << "baseBounds = " << baseBounds.toString(index.collator != nullptr) << '\n';
    addIndent(ss, indent + 1);
    *ss << "nearQuery = " << nq->toString() << '\n';
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString();
    }
}

std::unique_ptr<QuerySolutionNode> GeoNear2DSphereNode::clone() const {
    auto copy = std::make_unique<GeoNear2DSphereNode>(this->index);
    cloneBaseData(copy.get());

    copy->nq = this->nq;
    copy->baseBounds = this->baseBounds;
    copy->addPointMeta = this->addPointMeta;
    copy->addDistMeta = this->addDistMeta;

    return copy;
}

//
// ShardingFilterNode
//

void ShardingFilterNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SHARDING_FILTER\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        StringBuilder sb;
        *ss << "filter:\n";
        filter->debugString(sb, indent + 2);
        *ss << sb.str();
    }
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> ShardingFilterNode::clone() const {
    auto copy = std::make_unique<ShardingFilterNode>();
    cloneBaseData(copy.get());
    return copy;
}

//
// DistinctNode
//

void DistinctNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "DISTINCT\n";
    addIndent(ss, indent + 1);
    *ss << "name = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern << '\n';
    addIndent(ss, indent + 1);
    *ss << "direction = " << direction << '\n';
    addIndent(ss, indent + 1);
    *ss << "bounds = " << bounds.toString(index.collator != nullptr) << '\n';
}

std::unique_ptr<QuerySolutionNode> DistinctNode::clone() const {
    auto copy = std::make_unique<DistinctNode>(this->index);
    cloneBaseData(copy.get());

    copy->direction = this->direction;
    copy->bounds = this->bounds;
    copy->queryCollator = this->queryCollator;
    copy->fieldNo = this->fieldNo;

    return copy;
}

void DistinctNode::computeProperties() {
    // Note that we don't need to save the returned multikey fields for a DISTINCT_SCAN. They are
    // only needed for explodeForSort(), which works on IXSCAN but not DISTINCT_SCAN.
    sortSet = computeSortsAndMultikeyPathsForScan(
                  index, direction, bounds, queryCollator, nullptr /* iets */)
                  .first;
}

//
// CountScanNode
//

void CountScanNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "COUNT\n";
    addIndent(ss, indent + 1);
    *ss << "name = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern << '\n';
    addIndent(ss, indent + 1);
    *ss << "startKey = " << startKey << '\n';
    addIndent(ss, indent + 1);
    *ss << "endKey = " << endKey << '\n';
}

std::unique_ptr<QuerySolutionNode> CountScanNode::clone() const {
    auto copy = std::make_unique<CountScanNode>(this->index);
    cloneBaseData(copy.get());

    copy->startKey = this->startKey;
    copy->startKeyInclusive = this->startKeyInclusive;
    copy->endKey = this->endKey;
    copy->endKeyInclusive = this->endKeyInclusive;

    return copy;
}

//
// EofNode
//

void EofNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "EOF\n";
}

std::unique_ptr<QuerySolutionNode> EofNode::clone() const {
    auto copy = std::make_unique<EofNode>(this->type);
    cloneBaseData(copy.get());
    return copy;
}

//
// TextOrNode
//
void TextOrNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "TEXT_OR\n";
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString() << '\n';
    }
    addCommon(ss, indent);
    for (size_t i = 0; i < children.size(); ++i) {
        addIndent(ss, indent + 1);
        *ss << "Child " << i << ":\n";
        children[i]->appendToString(ss, indent + 2);
        *ss << '\n';
    }
}

std::unique_ptr<QuerySolutionNode> TextOrNode::clone() const {
    auto copy = std::make_unique<TextOrNode>();
    cloneBaseData(copy.get());
    copy->dedup = this->dedup;
    return copy;
}

//
// TextMatchNode
//
void TextMatchNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "TEXT_MATCH\n";
    addIndent(ss, indent + 1);
    *ss << "name = " << index.identifier.catalogName << '\n';
    addIndent(ss, indent + 1);
    *ss << "keyPattern = " << index.keyPattern.toString() << '\n';
    addIndent(ss, indent + 1);
    *ss << "query = " << ftsQuery->getQuery() << '\n';
    addIndent(ss, indent + 1);
    *ss << "language = " << ftsQuery->getLanguage() << '\n';
    addIndent(ss, indent + 1);
    *ss << "caseSensitive= " << ftsQuery->getCaseSensitive() << '\n';
    addIndent(ss, indent + 1);
    *ss << "diacriticSensitive= " << ftsQuery->getDiacriticSensitive() << '\n';
    addIndent(ss, indent + 1);
    *ss << "indexPrefix = " << indexPrefix.toString() << '\n';
    addIndent(ss, indent + 1);
    *ss << "wantTextScorex = " << wantTextScore << '\n';
    if (nullptr != filter) {
        addIndent(ss, indent + 1);
        *ss << " filter = " << filter->debugString();
    }
    addCommon(ss, indent);
}

std::unique_ptr<QuerySolutionNode> TextMatchNode::clone() const {
    auto copy = std::make_unique<TextMatchNode>(index, ftsQuery->clone(), wantTextScore);
    cloneBaseData(copy.get());
    copy->indexPrefix = indexPrefix;
    return copy;
}

/**
 * GroupNode.
 */
void GroupNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "GROUP\n";
    addIndent(ss, indent + 1);
    *ss << "key = ";
    auto idx = 0;
    if (auto exprObj = dynamic_cast<const ExpressionObject*>(groupByExpression.get()); exprObj) {
        for (auto&& [groupName, expr] : exprObj->getChildExpressions()) {
            if (idx > 0) {
                *ss << ", ";
            }
            *ss << "{" << groupName << ": " << exprObj->serialize().toString() << "}";
            ++idx;
        }
    } else {
        *ss << "{_id: " << groupByExpression->serialize().toString() << "}";
    }
    *ss << '\n';
    addIndent(ss, indent + 1);
    *ss << "accs = [";
    for (size_t idx = 0; idx < accumulators.size(); ++idx) {
        if (idx > 0) {
            *ss << ", ";
        }
        auto& acc = accumulators[idx];
        *ss << "{" << acc.fieldName << ": {" << acc.expr.name << ": "
            << acc.expr.argument
                   ->serialize(SerializationOptions{
                       .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)})
                   .toString()
            << "}}";
    }
    *ss << "]" << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> GroupNode::clone() const {
    auto copy = std::make_unique<GroupNode>(children[0]->clone(),
                                            groupByExpression,
                                            accumulators,
                                            doingMerge,
                                            willBeMerged,
                                            shouldProduceBson);
    return copy;
}

/**
 * EqLookupNode.
 */
void EqLookupNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "EQ_LOOKUP\n";
    addIndent(ss, indent + 1);
    *ss << "from = " << toStringForLogging(foreignCollection) << "\n";
    addIndent(ss, indent + 1);
    *ss << "as = " << joinField.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "localField = " << joinFieldLocal.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "foreignField = " << joinFieldForeign.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "lookupStrategy = " << serializeLookupStrategy(lookupStrategy) << "\n";
    if (idxEntry) {
        addIndent(ss, indent + 1);
        *ss << "indexName = " << idxEntry->identifier.catalogName << "\n";
        addIndent(ss, indent + 1);
        *ss << "indexKeyPattern = " << idxEntry->keyPattern << "\n";
    }
    addIndent(ss, indent + 1);
    *ss << "shouldProduceBson = " << shouldProduceBson << "\n";

    addIndent(ss, indent + 1);
    *ss << "scanDirection = " << scanDirection << "\n";

    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> EqLookupNode::clone() const {
    return std::make_unique<EqLookupNode>(children[0]->clone(),
                                          foreignCollection,
                                          joinFieldLocal,
                                          joinFieldForeign,
                                          joinField,
                                          lookupStrategy,
                                          idxEntry,
                                          shouldProduceBson,
                                          scanDirection);
}

/**
 * EqLookupUnwindNode.
 */
void EqLookupUnwindNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "EQ_LOOKUP_UNWIND\n";

    addIndent(ss, indent + 1);
    *ss << "from = " << toStringForLogging(foreignCollection) << "\n";
    addIndent(ss, indent + 1);
    *ss << "as = " << joinField.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "localField = " << joinFieldLocal.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "foreignField = " << joinFieldForeign.fullPath() << "\n";
    addIndent(ss, indent + 1);
    *ss << "lookupStrategy = " << EqLookupNode::serializeLookupStrategy(lookupStrategy) << "\n";
    if (idxEntry) {
        addIndent(ss, indent + 1);
        *ss << "indexName = " << idxEntry->identifier.catalogName << "\n";
        addIndent(ss, indent + 1);
        *ss << "indexKeyPattern = " << idxEntry->keyPattern << "\n";
    }
    addIndent(ss, indent + 1);
    *ss << "shouldProduceBson = " << shouldProduceBson << "\n";

    addIndent(ss, indent + 1);
    *ss << "preserveNullAndEmptyArrays = " << unwindSpec.preserveNullAndEmptyArrays << "\n";
    if (unwindSpec.indexPath) {
        addIndent(ss, indent + 1);
        *ss << "indexPath = " << unwindSpec.indexPath->fullPath() << "\n";
    }

    addIndent(ss, indent + 1);
    *ss << "scanDirection = " << scanDirection << "\n";

    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}

std::unique_ptr<QuerySolutionNode> EqLookupUnwindNode::clone() const {
    return std::make_unique<EqLookupUnwindNode>(children[0]->clone(),
                                                // Shared data members.
                                                joinField,
                                                // $lookup-specific data members.
                                                foreignCollection,
                                                joinFieldLocal,
                                                joinFieldForeign,
                                                lookupStrategy,
                                                idxEntry,
                                                shouldProduceBson,
                                                // $unwind-specific data members.
                                                unwindSpec.preserveNullAndEmptyArrays,
                                                unwindSpec.indexPath,
                                                scanDirection);
}

/**
 * SentinelNode.
 */
std::unique_ptr<QuerySolutionNode> SentinelNode::clone() const {
    return std::make_unique<SentinelNode>();
}

void SentinelNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SENTINEL\n";
}

std::unique_ptr<QuerySolutionNode> SearchNode::clone() const {
    return std::make_unique<SearchNode>(
        isSearchMeta, searchQuery, limit, sortSpec, remoteCursorId, remoteCursorVars);
}

void SearchNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "SEARCH\n";
    addIndent(ss, indent + 1);
    *ss << "isSearchMeta = " << isSearchMeta << '\n';
    addIndent(ss, indent + 1);
    *ss << "searchQuery = " << searchQuery << '\n';
    *ss << "remoteCursorId = " << remoteCursorId << '\n';
    if (limit) {
        addIndent(ss, indent + 1);
        *ss << "limit = " << limit << '\n';
    }
}

/**
 * WindowNode.
 */
std::unique_ptr<QuerySolutionNode> WindowNode::clone() const {
    return std::make_unique<WindowNode>(children[0]->clone(), partitionBy, sortBy, outputFields);
}

void WindowNode::appendToString(str::stream* ss, int indent) const {
    addIndent(ss, indent);
    *ss << "WINDOW\n";
    if (partitionBy) {
        addIndent(ss, indent + 1);
        *ss << "partitionBy = " << (*partitionBy)->serialize().toString() << '\n';
    }
    if (sortBy) {
        addIndent(ss, indent + 1);
        *ss << "sortBy = "
            << sortBy->serialize(SortPattern::SortKeySerialization::kForExplain).toBson().toString()
            << '\n';
    }
    addIndent(ss, indent + 1);
    *ss << "outputFields = [";
    for (size_t idx = 0; idx < outputFields.size(); ++idx) {
        if (idx > 0) {
            *ss << ", ";
        }
        auto& outputField = outputFields[idx];
        MutableDocument boundsDoc;
        outputField.expr->bounds().serialize(boundsDoc, SerializationOptions{});
        auto boundsBson = boundsDoc.freeze().toBson();
        *ss << "{" << outputField.fieldName << ": {" << outputField.expr->getOpName() << ": "
            << outputField.expr->input()->serialize().toString()
            << "window: " << boundsBson.toString() << "}}";
    }
    *ss << "]" << '\n';
    addCommon(ss, indent);
    addIndent(ss, indent + 1);
    *ss << "Child:" << '\n';
    children[0]->appendToString(ss, indent + 2);
}
}  // namespace mongo
