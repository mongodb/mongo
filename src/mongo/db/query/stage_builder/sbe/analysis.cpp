/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/analysis.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <sstream>
#include <tuple>

#include <absl/container/inlined_vector.h>

namespace mongo::stage_builder {
FieldEffect FieldEffects::getComposedEffect(FieldEffect child, FieldEffect parent) {
    if (child == FieldEffect::kKeep ||
        (parent == FieldEffect::kGeneric || parent == FieldEffect::kDrop ||
         parent == FieldEffect::kAdd)) {
        return parent;
    }
    if (child == FieldEffect::kModify) {
        return parent == FieldEffect::kSet ? FieldEffect::kSet : FieldEffect::kModify;
    }
    if (parent == FieldEffect::kSet) {
        return child == FieldEffect::kDrop ? FieldEffect::kAdd : FieldEffect::kGeneric;
    }
    return child;
}

FieldEffects::FieldEffects(bool isInclusion,
                           const std::vector<std::string>& paths,
                           const std::vector<NodeType>& nodeTypes) {
    tassert(9294700,
            "Expected 'paths' and 'nodeTypes' to be the same size",
            paths.size() == nodeTypes.size());

    _defaultEffect = isInclusion ? FieldEffect::kDrop : FieldEffect::kKeep;

    // Loop over 'paths' / 'nodeTypes'.
    for (size_t i = 0; i < nodeTypes.size(); ++i) {
        auto nodeType = nodeTypes[i];
        const auto& path = paths[i];

        bool isDottedPath = path.find('.') != std::string::npos;
        std::string field = std::string{getTopLevelField(path)};

        // Add 'field' to '_fields' if needed.
        const bool presentInEffectsMap = _effects.count(field);
        if (!presentInEffectsMap) {
            _fields.emplace_back(field);
        }

        // Update '_effects[field]'.
        if (!isDottedPath) {
            switch (nodeType) {
                case NodeType::kExpr:
                case NodeType::kSbExpr:
                    _effects[field] = isInclusion ? FieldEffect::kAdd : FieldEffect::kSet;
                    break;
                case NodeType::kBool:
                    _effects[field] = isInclusion ? FieldEffect::kKeep : FieldEffect::kDrop;
                    break;
                case NodeType::kSlice:
                    _effects[field] = FieldEffect::kModify;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        } else {
            switch (nodeType) {
                case NodeType::kExpr:
                case NodeType::kSbExpr:
                    _effects[field] = FieldEffect::kSet;
                    break;
                case NodeType::kBool:
                case NodeType::kSlice:
                    // If 'field' is not currently present in the '_effects' map, then we add
                    // a kModify effect. Otherwise if 'field' is already in the '_effects' map,
                    // then we do nothing and keep the current effect for 'field' as-is.
                    if (!presentInEffectsMap) {
                        _effects[field] = FieldEffect::kModify;
                    }
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
}

FieldEffects::FieldEffects(bool isInclusion,
                           const std::vector<std::string>& paths,
                           const std::vector<ProjectNode>& nodes)
    : FieldEffects(isInclusion, paths, ProjectNode::getNodeTypes(nodes)) {}

FieldEffects::FieldEffects(std::vector<std::string> fieldsIn,
                           const std::vector<FieldEffect>& effects,
                           FieldEffect defaultEffect)
    : _fields(std::move(fieldsIn)), _defaultEffect(defaultEffect) {
    tassert(9294701,
            "Expected default effect to be Keep or Drop",
            defaultEffect == FieldEffect::kKeep || defaultEffect == FieldEffect::kDrop);
    tassert(9294702,
            "Expected 'fields' and 'effects' to be the same size",
            _fields.size() == effects.size());

    for (size_t i = 0; i < _fields.size(); ++i) {
        tassert(9294703, "Expected field names to be unique", !_effects.count(_fields[i]));

        _effects[_fields[i]] = effects[i];
    }
}

void FieldEffects::init(const FieldSet& allowedFieldSet) {
    bool allowedIsClosed = allowedFieldSet.getScope() == FieldListScope::kClosed;

    _defaultEffect = allowedIsClosed ? FieldEffect::kDrop : FieldEffect::kKeep;

    for (auto&& name : allowedFieldSet.getList()) {
        _fields.emplace_back(name);
        _effects[name] = allowedIsClosed ? FieldEffect::kKeep : FieldEffect::kDrop;
    }
}

void FieldEffects::init(const FieldSet& allowedFieldSet,
                        const FieldSet& changedFieldSet,
                        const CreatedFieldVectorType& createdFieldVec) {
    tassert(9294704,
            "Expected 'changedFieldSet' to be closed",
            changedFieldSet.getScope() == FieldListScope::kClosed);

    bool allowedIsClosed = allowedFieldSet.getScope() == FieldListScope::kClosed;

    auto addEffect = [&](const std::string& name, FieldEffect effect) {
        if (!_effects.count(name)) {
            _fields.emplace_back(name);
            _effects[name] = effect;
        }
    };

    for (const auto& [name, effect] : createdFieldVec) {
        addEffect(name, effect);
    }
    for (auto&& name : changedFieldSet.getList()) {
        addEffect(name, FieldEffect::kModify);
    }
    for (auto&& name : allowedFieldSet.getList()) {
        addEffect(name, allowedIsClosed ? FieldEffect::kKeep : FieldEffect::kDrop);
    }

    _defaultEffect = allowedIsClosed ? FieldEffect::kDrop : FieldEffect::kKeep;
}

void FieldEffects::setFieldOrder(const std::vector<std::string>& order) {
    if (_fields.size() < order.size() || !std::equal(order.begin(), order.end(), _fields.begin())) {
        // If 'order' is not a prefix of '_fields', then add all the fields from 'order' to
        // '_fields', reorder '_fields' so that 'order' is a prefix of it, and update
        // '_effects'.
        _fields = appendVectorUnique(order, std::move(_fields));

        for (const auto& field : order) {
            if (!_effects.count(field)) {
                _effects[field] = _defaultEffect;
            }
        }
    }
}

void FieldEffects::compose(const FieldEffects& parent) {
    std::vector<std::string> newFields;

    // Loop over '_fields'.
    for (const std::string& field : _fields) {
        // If this field is a not a created field in 'parent', then process it now.

        if (!parent.isCreatedField(field)) {
            FieldEffect& effect = _effects[field];
            FieldEffect parentEffect = parent.get(field);

            newFields.emplace_back(field);
            effect = getComposedEffect(effect, parentEffect);
        }
    }

    // Loop over 'parent._fields'.
    for (const std::string& field : parent._fields) {
        // If this field was not processed by the previous loop, then process it now.
        if (parent.isCreatedField(field) || !_effects.count(field)) {
            FieldEffect effect = get(field);
            FieldEffect parentEffect = parent.get(field);

            newFields.emplace_back(field);
            _effects[field] = getComposedEffect(effect, parentEffect);
        }
    }

    // Update '_fields'.
    _fields = std::move(newFields);

    // Update '_defaultEffect'.
    _defaultEffect = getComposedEffect(_defaultEffect, parent._defaultEffect);
}

void FieldEffects::narrow(const FieldSet& domainSet) {
    if (domainSet.getScope() == FieldListScope::kOpen || _defaultEffect == FieldEffect::kKeep) {
        // Compute 'visitFieldSet = ~(domainSet union keepFieldSet)', where "keepFieldSet" is
        // the set of fields whose effects are kKeep.
        auto visitFieldSet = getAllowedFields();
        visitFieldSet.setDifference(getChangedFields());
        visitFieldSet.setUnion(domainSet);
        visitFieldSet.setComplement();
        // For each field in 'visitFieldSet.getList()', add it to '_fields' if it's not already
        // present and set its effect to kKeep.
        for (const auto& field : visitFieldSet.getList()) {
            if (!_effects.count(field)) {
                _fields.emplace_back(field);
            }
            _effects[field] = FieldEffect::kKeep;
        }
    } else {
        // For each field in '_fields' that's not present in 'domainSet', set its effect to
        // kKeep.
        for (const auto& field : _fields) {
            if (!domainSet.count(field)) {
                _effects[field] = FieldEffect::kKeep;
            }
        }

        // Before we change '_defaultEffect', loop over all the fields in 'domainSet' and make
        // sure they are explicitly listed in '_fields' and '_effects'.
        for (const auto& field : domainSet.getList()) {
            if (!_effects.count(field)) {
                _fields.emplace_back(field);
                _effects[field] = _defaultEffect;
            }
        }

        // Change '_defaultEffect' to kKeep.
        _defaultEffect = FieldEffect::kKeep;
    }
}

void FieldEffects::removeRedundantEffects() {
    size_t outIdx = 0;
    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto& field = _fields[idx];

        if (_effects[field] != _defaultEffect) {
            if (outIdx != idx) {
                _fields[outIdx] = std::move(field);
            }
            ++outIdx;
        } else {
            _effects.erase(field);
        }
    }

    if (outIdx != _fields.size()) {
        _fields.resize(outIdx);
    }
}

FieldSet FieldEffects::getAllowedFields() const {
    return getFieldsWithEffects([](auto e) { return e != FieldEffect::kDrop; });
}

FieldSet FieldEffects::getDroppedFields() const {
    return getFieldsWithEffects([](auto e) { return e == FieldEffect::kDrop; });
}

FieldSet FieldEffects::getChangedFields() const {
    return getFieldsWithEffects(
        [](auto e) { return e != FieldEffect::kKeep && e != FieldEffect::kDrop; });
}

FieldSet FieldEffects::getDroppedOrChangedFields() const {
    return getFieldsWithEffects([](auto e) { return e != FieldEffect::kKeep; });
}

FieldSet FieldEffects::getCreatedFields() const {
    return getFieldsWithEffects([](auto e) {
        return e != FieldEffect::kKeep && e != FieldEffect::kDrop && e != FieldEffect::kModify;
    });
}

FieldEffects::CreatedFieldVectorType FieldEffects::getCreatedFieldVector() const {
    CreatedFieldVectorType fieldEffectPairs;

    for (auto&& field : _fields) {
        if (isCreatedField(field)) {
            fieldEffectPairs.emplace_back(std::pair(field, get(field)));
        }
    }

    return fieldEffectPairs;
}

bool FieldEffects::hasAllowedFields() const {
    return hasFieldsWithEffects([](auto e) { return e != FieldEffect::kDrop; });
}

bool FieldEffects::hasDroppedFields() const {
    return hasFieldsWithEffects([](auto e) { return e == FieldEffect::kDrop; });
}

bool FieldEffects::hasChangedFields() const {
    return hasFieldsWithEffects(
        [](auto e) { return e != FieldEffect::kKeep && e != FieldEffect::kDrop; });
}

bool FieldEffects::hasDroppedOrChangedFields() const {
    return hasFieldsWithEffects([](auto e) { return e != FieldEffect::kKeep; });
}

bool FieldEffects::hasCreatedFields() const {
    return hasFieldsWithEffects([](auto e) {
        return e != FieldEffect::kKeep && e != FieldEffect::kDrop && e != FieldEffect::kModify;
    });
}

std::string FieldEffects::toString() const {
    std::stringstream ss;

    auto effectToString = [&](FieldEffect e) -> StringData {
        switch (e) {
            case FieldEffect::kKeep:
                return "Keep"_sd;
            case FieldEffect::kDrop:
                return "Drop"_sd;
            case FieldEffect::kModify:
                return "Modify"_sd;
            case FieldEffect::kSet:
                return "Set"_sd;
            case FieldEffect::kAdd:
                return "Add"_sd;
            case FieldEffect::kGeneric:
                return "Generic"_sd;
            default:
                return "UNKNOWN"_sd;
        }
    };

    ss << "{";

    bool first = true;
    for (auto&& field : _fields) {
        if (!first) {
            ss << ", ";
        }
        first = false;

        auto effect = _effects.find(field)->second;
        ss << field << " : " << effectToString(effect);
    }

    ss << (!first ? ", * : " : "* : ") << effectToString(_defaultEffect) << "}";

    return ss.str();
}

boost::optional<FieldEffects> composeEffectsForResultInfo(const FieldEffects& narrowedProjEffects,
                                                          const FieldEffects& resultInfoEffects) {
    boost::optional<FieldEffects> composedEffects;
    composedEffects.emplace(narrowedProjEffects);
    composedEffects->compose(resultInfoEffects);

    if (hasNonSpecificEffects(*composedEffects)) {
        // If 'composedEffects' contains non-specific effects, return boost::none to indicate that
        // the projection can't participate with the ResultInfo req.
        return boost::none;
    }

    // If 'narrowedProjEffects' doesn't drop an infinite number of fields -AND- if 'composedEffects'
    // doesn't contain any non-specific effects, then return 'composedEffects' to indicate that the
    // projection can participate with the ResultInfo req.
    return composedEffects;
}

FieldSet makeAllowedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    return FieldEffects(isInclusion, paths, nodes).getAllowedFields();
}

FieldSet makeModifiedOrCreatedFieldSet(bool isInclusion,
                                       const std::vector<std::string>& paths,
                                       const std::vector<ProjectNode>& nodes) {
    return FieldEffects(isInclusion, paths, nodes).getChangedFields();
}

FieldSet makeCreatedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    return FieldEffects(isInclusion, paths, nodes).getCreatedFields();
}

FieldEffect getMergedEffect(FieldEffect e1, FieldEffect e2) {
    static constexpr auto kKeep = FieldEffect::kKeep;
    static constexpr auto kDrop = FieldEffect::kDrop;
    static constexpr auto kModify = FieldEffect::kModify;
    static constexpr auto kSet = FieldEffect::kSet;
    static constexpr auto kAdd = FieldEffect::kAdd;
    static constexpr auto kGeneric = FieldEffect::kGeneric;

    // clang-format off
    static FieldEffect mergeTable[6][6] =
        /*                  kKeep     kDrop     kModify   kSet      kAdd      kGeneric  */
        /*          +------------------------------------------------------------------ */
        /* kKeep    | */ {{ kKeep,    kSet,     kModify,  kSet,     kGeneric, kGeneric },
        /* kDrop    | */  { kSet,     kDrop,    kSet,     kSet,     kAdd,     kGeneric },
        /* kModify  | */  { kModify,  kSet,     kModify,  kSet,     kGeneric, kGeneric },
        /* kSet     | */  { kSet,     kSet,     kSet,     kSet,     kGeneric, kGeneric },
        /* kAdd     | */  { kGeneric, kAdd,     kGeneric, kGeneric, kAdd,     kGeneric },
        /* kGeneric | */  { kGeneric, kGeneric, kGeneric, kGeneric, kGeneric, kGeneric }};
    // clang-format on

    size_t i = static_cast<size_t>(e1);
    size_t j = static_cast<size_t>(e2);
    tassert(9294705, "Encountered unexpected FieldEffect value", i < 6);
    tassert(9294706, "Encountered unexpected FieldEffect value", j < 6);

    return mergeTable[i][j];
}

boost::optional<FieldEffects> mergeEffects(const FieldEffects& e1, const FieldEffects& e2) {
    // If 'e1' and 'e2' has the same default effect, then the merged effects will have the same
    // default effect as 'e1' and 'e2'. Otherwise, if 'e1' and 'e2' do not have the same default
    // effect, then it's not possible to produce the merge effects and so we return boost::none.
    FieldEffect defaultEffect = e1.getDefaultEffect();
    if (defaultEffect != e2.getDefaultEffect()) {
        return boost::none;
    }

    std::vector<std::string> fields;
    std::vector<FieldEffect> effects;
    StringDataSet fieldSet;

    auto processFields = [&](const FieldEffects& e) {
        for (const std::string& field : e.getFieldList()) {
            if (!fieldSet.count(field)) {
                fieldSet.emplace(field);
                fields.emplace_back(field);
                effects.emplace_back(getMergedEffect(e1.get(field), e2.get(field)));
            }
        }
    };

    // First process the fields from 'e1', and then process the fields from 'e2'.
    processFields(e1);
    processFields(e2);

    // Construct a FieldEffects from 'fields' / 'effects' / 'defaultEffect' and return it.
    boost::optional<FieldEffects> result;
    result.emplace(std::move(fields), std::move(effects), defaultEffect);
    return result;
}

boost::optional<FieldEffects> mergeEffects(const std::vector<FieldEffects>& args) {
    auto n = args.size();
    auto result = n > 0 ? boost::make_optional(args[0]) : boost::none;
    for (size_t i = 1; i < n && result; ++i) {
        result = mergeEffects(*result, args[i]);
    }
    return result;
}

bool canUseCoveredProjection(const QuerySolutionNode* root,
                             bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    // The "covered projection" optimization can be used for this projection if and only if
    // all of the following conditions are met:
    //   0) 'root' is a projection.
    //   1) 'isInclusion' is true.
    //   2) The 'nodes' vector contains Keep nodes only.
    //   3) The 'root' subtree has at least one unfetched IXSCAN node. (A corollary of this
    //      condition is that 'root->fetched()' must also be false.)
    //   4) For each unfetched IXSCAN in the root's subtree, the IXSCAN's pattern contains
    //      all of the paths needed by the 'root' projection to materialize the result object.
    if (root->fetched() || !isInclusion ||
        !std::all_of(nodes.begin(), nodes.end(), [](auto&& n) { return n.isBool(); })) {
        return false;
    }

    boost::optional<UnfetchedIxscans> unfetchedIxns = getUnfetchedIxscans(root);
    if (!unfetchedIxns || unfetchedIxns->ixscans.empty() || unfetchedIxns->hasFetchesOrCollScans) {
        return false;
    }

    for (auto& ixNode : unfetchedIxns->ixscans) {
        auto ixn = static_cast<const IndexScanNode*>(ixNode);

        // Read the pattern for this IXSCAN and build a part set.
        StringDataSet patternPartSet;
        for (BSONObjIterator it(ixn->index.keyPattern); it.more();) {
            auto part = it.next().fieldNameStringData();
            patternPartSet.insert(part);
        }

        // If this pattern does not contain all of the projection paths needed by
        // root's parent, then we can't use the "covered projection" optimization.
        for (const auto& path : paths) {
            if (!patternPartSet.count(path)) {
                return false;
            }
        }
    }

    return true;
}

/**
 * When doing a covered projection, we need to re-order 'paths' so that it matches the field
 * order from 'keyPatternObj'.
 */
std::pair<std::vector<std::string>, std::vector<ProjectNode>> sortPathsAndNodesForCoveredProjection(
    const QuerySolutionNode* qsNode,
    std::vector<std::string> paths,
    std::vector<ProjectNode> nodes) {
    tassert(8323502, "Expected paths and nodes to be the same size", paths.size() == nodes.size());

    StringDataMap<size_t> pathMap;
    std::vector<std::string> newPaths;
    std::vector<ProjectNode> newNodes;
    absl::InlinedVector<char, 64> moved;
    const size_t n = paths.size();

    newPaths.reserve(n);
    newNodes.reserve(n);
    moved.resize(n, 0);

    // Build a map that maps each path to its position in the 'paths' vector.
    for (size_t idx = 0; idx < n; ++idx) {
        pathMap.emplace(paths[idx], idx);
    }

    // Get the key pattern for this covered projection.
    const BSONObj& keyPatternObj = qsNode->getType() != STAGE_PROJECTION_COVERED
        ? static_cast<const IndexScanNode*>(getUnfetchedIxscans(qsNode)->ixscans[0])
              ->index.keyPattern
        : static_cast<const ProjectionNodeCovered*>(qsNode)->coveredKeyObj;

    // Loop over the key pattern. For each path from the key pattern which is present in 'pathMap',
    // add the path and its corresponding ProjectNode to 'newPaths' and 'newNodes', respectively.
    for (BSONObjIterator it(keyPatternObj); it.more();) {
        if (auto mapIt = pathMap.find(it.next().fieldNameStringData()); mapIt != pathMap.end()) {
            size_t idx = mapIt->second;
            if (!moved[idx]) {
                newPaths.emplace_back(std::move(paths[idx]));
                newNodes.emplace_back(std::move(nodes[idx]));
                moved[idx] = 1;
            }
        }
    }

    // If there are any path/node pairs remaining that haven't been moved yet, move them now.
    for (size_t idx = 0; idx < n; ++idx) {
        if (!moved[idx]) {
            newPaths.emplace_back(std::move(paths[idx]));
            newNodes.emplace_back(std::move(nodes[idx]));
        }
    }

    return {std::move(newPaths), std::move(newNodes)};
}

std::vector<ProjectNode::Type> getTransformedNodeTypesForCoveredProjection(size_t numNewPaths) {
    std::vector<ProjectNode::Type> newNodeTypes;
    newNodeTypes.resize(numNewPaths, ProjectNode::Type::kSbExpr);
    return newNodeTypes;
}

FieldEffects getTransformedEffectsForCoveredProjection(const QuerySolutionNode* root,
                                                       const std::vector<std::string>& paths) {
    const bool isInclusion = true;
    auto nodeTypes = getTransformedNodeTypesForCoveredProjection(paths.size());
    return FieldEffects(isInclusion, paths, nodeTypes);
}

const FieldSet QsnAnalysis::kEmptyFieldSet = FieldSet::makeEmptySet();
const FieldSet QsnAnalysis::kUniverseFieldSet = FieldSet::makeUniverseSet();
const FieldEffects QsnAnalysis::kAllKeepEffects = FieldEffects();
const FieldEffects QsnAnalysis::kAllDropEffects = FieldEffects(FieldSet::makeEmptySet());

void QsnAnalysis::analyzeTree(const QuerySolutionNode* root) {
    tassert(8323505, "analyzeTree() should only be called once", _analysis.empty());

    using DfsItem = std::tuple<const QuerySolutionNode*, QsnInfo*, size_t>;

    absl::InlinedVector<DfsItem, 32> dfs;
    dfs.emplace_back(DfsItem(root, nullptr, 0));

    while (!dfs.empty()) {
        auto& [qsNode, qsnInfo, childIdx] = dfs.back();

        if (!qsnInfo) {
            auto [it, _] = _analysis.emplace(qsNode, std::make_unique<QsnInfo>());
            qsnInfo = it->second.get();
        }

        if (childIdx < qsNode->children.size()) {
            auto* child = qsNode->children[childIdx++].get();
            dfs.emplace_back(DfsItem(child, nullptr, 0));
        } else {
            analyzeQsNode(qsNode, *qsnInfo);
            dfs.pop_back();
        }
    }
}

void QsnAnalysis::analyzeQsNode(const QuerySolutionNode* qsNode, QsnInfo& qsnInfo) {
    switch (qsNode->getType()) {
        case STAGE_LIMIT:
        case STAGE_SKIP:
        case STAGE_MATCH:
        case STAGE_SHARDING_FILTER:
        case STAGE_SORT_SIMPLE:
        case STAGE_SORT_DEFAULT: {
            // Use the postimage allowed set and postimage present set from qsNode's child.
            const auto& childQsnInfo = getQsnInfo(qsNode->children[0]);
            qsnInfo.setPostimageAllowedFields(childQsnInfo.postimageAllowedFields);
            return;
        }
        case STAGE_OR:
        case STAGE_SORT_MERGE: {
            // qsNode's postimage allowed set is equal to the union of all of its children's
            // postimage allowed sets, and its postimage present set is equal to the intersection
            // of its children's postimage present sets.
            auto allowedFields = *getQsnInfo(qsNode->children[0]).postimageAllowedFields;

            for (size_t i = 1; i < qsNode->children.size(); ++i) {
                const auto& childQsnInfo = getQsnInfo(qsNode->children[i]);
                allowedFields.setUnion(*childQsnInfo.postimageAllowedFields);
            }

            qsnInfo.setPostimageAllowedFields(std::move(allowedFields));

            return;
        }
        case STAGE_IXSCAN: {
            std::vector<std::string> ixscanFields;
            StringSet ixscanSet;
            // Loop over the parts of the index's keyPattern and add each top-level field
            // that is referenced to 'ixscanFields', and then return 'ixscanFields'.
            auto ixn = static_cast<const IndexScanNode*>(qsNode);
            BSONObjIterator it(ixn->index.keyPattern);
            while (it.more()) {
                auto f = getTopLevelField(it.next().fieldNameStringData());
                if (!ixscanSet.count(f)) {
                    auto str = std::string{f};
                    ixscanFields.emplace_back(str);
                    ixscanSet.emplace(std::move(str));
                }
            }

            auto fieldSet = FieldSet::makeClosedSet(std::move(ixscanFields));

            qsnInfo.setPostimageAllowedFields(std::move(fieldSet));

            return;
        }
        case STAGE_GROUP: {
            auto groupNode = static_cast<const GroupNode*>(qsNode);

            // Build a list of $group's output fields.
            std::vector<std::string> groupFields;
            FieldEffects::CreatedFieldVectorType createdFieldVec;
            StringDataSet dedup;
            bool hasDuplicateNames = false;

            // Add "_id" to 'groupFields' and 'createdFieldVec'.
            groupFields.emplace_back("_id"_sd);
            createdFieldVec.emplace_back("_id"_sd, FieldEffect::kAdd);
            dedup.emplace("_id"_sd);

            // Add each accumulator's output field to 'groupFields' and 'createdFieldVec'.
            for (const auto& accStmt : groupNode->accumulators) {
                const auto& field = accStmt.fieldName;

                if (dedup.insert(field).second) {
                    groupFields.emplace_back(field);
                    createdFieldVec.emplace_back(field, FieldEffect::kAdd);
                } else {
                    hasDuplicateNames = true;
                }
            }

            auto fieldSet = FieldSet::makeClosedSet(std::move(groupFields));

            // Store the FieldEffects for this $group into 'qsnInfo.effects'. (For the weird case
            // where $group has two or more fields with the same name, we don't attempt to construct
            // a FieldEffects for it and we just set 'qsnInfo.effects' to boost::none.)
            if (!hasDuplicateNames) {
                qsnInfo.effects.emplace(FieldEffects(fieldSet, fieldSet, createdFieldVec));
            }

            // Store the postimage allowed field set into the QsnInfo.
            qsnInfo.setPostimageAllowedFields(std::move(fieldSet));

            return;
        }
        case STAGE_WINDOW: {
            auto windowNode = static_cast<const WindowNode*>(qsNode);

            // Get the output fields of the window node and add them to the createdFieldSet.
            FieldEffects::CreatedFieldVectorType createdFieldVec;
            bool hasDottedPath = false;
            for (size_t i = 0; i < windowNode->outputFields.size(); i++) {
                const auto& field = windowNode->outputFields[i].fieldName;
                createdFieldVec.emplace_back(getTopLevelField(field), FieldEffect::kSet);
                hasDottedPath |= (field.find('.') != std::string::npos);
            }

            if (hasDottedPath) {
                qsnInfo.setPostimageAllowedFields(&kUniverseFieldSet);
                return;
            }

            // The effects the window node has.
            auto effects = FieldEffects(
                FieldSet::makeUniverseSet(), FieldSet::makeEmptySet(), createdFieldVec);

            // Compute the postimage allowed field set combining the pre-image effects with the
            // window node effects.
            const auto& preimageAllowedFields =
                *getQsnInfo(qsNode->children[0]).postimageAllowedFields;
            auto effectsOverPreimage = FieldEffects(preimageAllowedFields);
            effectsOverPreimage.compose(effects);

            qsnInfo.effects.emplace(std::move(effects));

            // Store the postimage allowed field set into the QsnInfo.
            auto postimageAllowedFields = effectsOverPreimage.getAllowedFields();
            qsnInfo.setPostimageAllowedFields(std::move(postimageAllowedFields));

            return;
        }
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE: {
            auto pn = static_cast<const ProjectionNode*>(qsNode);

            auto [paths, nodes] = getProjectNodes(pn->proj);
            bool isInclusion = pn->proj.type() == projection_ast::ProjectType::kInclusion;
            bool isCoveredProjection = canUseCoveredProjection(qsNode, isInclusion, paths, nodes);

            const auto& preimageAllowedFields = !isCoveredProjection
                ? *getQsnInfo(qsNode->children[0]).postimageAllowedFields
                : kEmptyFieldSet;

            if (isCoveredProjection) {
                // If this is a covered projection, re-order 'paths' and 'nodes' to match the
                // covered projection's key pattern.
                auto [newPaths, newNodes] = sortPathsAndNodesForCoveredProjection(
                    qsNode, std::move(paths), std::move(nodes));
                paths = std::move(newPaths);
                nodes = std::move(newNodes);
            }

            if (!isCoveredProjection) {
                // If this is a non-covered projection, identify any parts of the projection that
                // are effectively no-ops and remove these parts.
                size_t outIdx = 0;
                for (size_t idx = 0; idx < nodes.size(); ++idx) {
                    if (nodes[idx].isExpr() || nodes[idx].isSbExpr() ||
                        preimageAllowedFields.count(getTopLevelField(paths[idx]))) {
                        if (outIdx != idx) {
                            paths[outIdx] = std::move(paths[idx]);
                            nodes[outIdx] = std::move(nodes[idx]);
                        }
                        ++outIdx;
                    }
                }
                if (outIdx < nodes.size()) {
                    paths.resize(outIdx);
                    nodes.resize(outIdx);
                }
            }

            // Get the FieldEffects for this projection.
            auto effects = !isCoveredProjection
                ? FieldEffects(isInclusion, paths, nodes)
                : getTransformedEffectsForCoveredProjection(qsNode, paths);

            // Compute the postimage allowed field set.
            auto effectsOverPreimage = FieldEffects(preimageAllowedFields);
            effectsOverPreimage.compose(effects);
            auto postimageAllowedFields = effectsOverPreimage.getAllowedFields();

            // Store the FieldEffects for this projection into the QsnInfo.
            qsnInfo.effects.emplace(std::move(effects));

            // Store the postimage allowed field set into the QsnInfo.
            qsnInfo.setPostimageAllowedFields(std::move(postimageAllowedFields));

            // Compute the dependencies needed by this projection.
            std::vector<std::string> depFields;
            bool needWholeDocument = false;

            if (!isCoveredProjection) {
                // Here we are only interested in the dependencies needed by the _expressions_
                // contained in the projection (ex. "{a: {$add: ["$x", 1]}}"), but we don't care
                // about the keep/drop parts of the projection (ex. "{a: 1}" or "{a: 0}"). Thus
                // we can't use analyzeProjection() here, and instead we need to manually retrieve
                // dependencies for each expression in the projection.
                DepsTracker deps;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    auto& node = nodes[i];
                    if (node.isExpr()) {
                        expression::addDependencies(node.getExpr(), &deps);
                    }
                }

                depFields = getTopLevelFields(deps.fields);
                needWholeDocument = deps.needWholeDocument;
            } else {
                depFields = paths;
                needWholeDocument = false;
            }

            // Store the projection info into the QsnInfo.
            qsnInfo.projectionInfo = std::make_unique<ProjectionInfo>(isInclusion,
                                                                      std::move(paths),
                                                                      std::move(nodes),
                                                                      isCoveredProjection,
                                                                      std::move(depFields),
                                                                      needWholeDocument);
            return;
        }
        default: {
            qsnInfo.setPostimageAllowedFields(&kUniverseFieldSet);
            return;
        }
    }
}

ProjectionInfo::ProjectionInfo(bool isInclusion,
                               std::vector<std::string> paths,
                               std::vector<ProjectNode> nodes,
                               bool isCoveredProjection,
                               std::vector<std::string> depFields,
                               bool needWholeDocument)
    : isInclusion(isInclusion),
      isCoveredProjection(isCoveredProjection),
      needWholeDocument(needWholeDocument),
      paths(std::move(paths)),
      nodes(std::move(nodes)),
      depFields(std::move(depFields)) {}
}  // namespace mongo::stage_builder
