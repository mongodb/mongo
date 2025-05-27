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

#pragma once

#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/util/field_set.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <boost/optional.hpp>

namespace mongo {
struct QuerySolutionNode;

namespace stage_builder {
/**
 * Here is a Venn diagram showing what possible changes to a value are permitted for each kind
 * of FieldEffect:
 *   +-----------------------------------+
 *   | Generic                           |
 *   | +-----------------------+ +-----+ |
 *   | | Set                   | | Add | |
 *   | | +----------+ +------+ | +-----+ |
 *   | | | Modify   | | Drop | |         |
 *   | | | +------+ | +------+ |         |
 *   | | | | Keep | |          |         |
 *   | | | +------+ |          |         |
 *   | | +----------+          |         |
 *   | +-----------------------+         |
 *   +-----------------------------------+
 *
 * The "Keep" effect is only allowed to return the input field unmodified, and the "Drop" effect
 * is only allowed to return Nothing. Thus, Keep's set contains only 1 possibility and Drop's
 * set contains only 1 possibility and the two sets do not intersect.
 *
 * The "Modify" effect is allowed to return any non-Nothing value when the input field exists, and
 * it must return Nothing when the input field doesn't exist. Therefore, Modify's set is a superset
 * of Keep's set, and Modify's set does not intersect with Drop's set.
 *
 * When the input field exists, the "Keep" and "Modify" effects are required to preserve the input
 * field's current position in the object.
 *
 * The "Add", "Set", and "Generic" effects are all allowed to return the input field with any
 * modifications without restrictions. These effects may return a non-Nothing value when the input
 * field doesn't exist.
 *
 * The "Add", "Set", and "Generic" effects differ from each other with respect to how they can
 * affect the input field's position within the object. The "Set" effect must preserve the input
 * field's current position (if it exists). The "Add" effect must move the input field (if it
 * exists) to the end of the object. The "Generic" effect can make arbitrary changes to the input
 * field's position within the object.
 *
 * Set's set is a superset of Modify's set. Generic's set is a superset of all other sets.
 *
 * If we don't have proof that a given input field is non-existent, then we assume Add's set and
 * Set's set are disjoint (as shown in the diagram above). If we have proof that a given input field
 * is non-existent, then Add and Set are equivalent to each other and we can assume Add's set is
 * equal to Set's set.
 */
enum class FieldEffect : int { kKeep, kDrop, kModify, kSet, kAdd, kGeneric };

/**
 * The 'FieldEffects' class is used to represented the "effects" that a projection (either
 * (a single projection or multiple projections combined together) has on the set of all possible
 * top-level field names.
 *
 * Conceptully, a FieldEffects object can be thought of as a field name/FieldEffect map plus
 * a "default" FieldEffect to be applied to all fields that are not present in the map.
 *
 * The six possible effects modeled by this class are: Keep, Drop, Modify, Set, Add, and Generic.
 * The "default" FieldEffect may be set to Keep or Drop.
 *
 * For details on each kind of FieldEffect, see the docblock above the FieldEffect enum.
 *
 * A FieldEffects object can be constructed from a projection. Two FieldEffects objects can also
 * be combined together using the compose() method.
 */
class FieldEffects {
public:
    using CreatedFieldVectorType = std::vector<std::pair<std::string, FieldEffect>>;
    using NodeType = ProjectNode::Type;

    static FieldEffect getComposedEffect(FieldEffect child, FieldEffect parent);

    /**
     * Constructs a FieldEffects that has the kKeep effect for all fields.
     */
    FieldEffects() = default;

    /**
     * Constructs a FieldEffects from a projection as specified by 'isInclusion', 'paths',
     * and 'nodes'. 'isInclusion' indicates whether the projection is an inclusion or an
     * exclusion. 'paths' and 'nodes' are parallel vectors (one a vector of strings, the
     * other a vector of ProjectNodes) that specify the actions performed by the projection
     * on each path.
     */
    FieldEffects(bool isInclusion,
                 const std::vector<std::string>& paths,
                 const std::vector<NodeType>& nodeTypes);

    FieldEffects(bool isInclusion,
                 const std::vector<std::string>& paths,
                 const std::vector<ProjectNode>& nodes);

    /**
     * Constructs a FieldEffects containing only kKeep and kDrop effects whose allowed field
     * set matches 'allowedFieldSet'.
     */
    FieldEffects(const FieldSet& allowedFieldSet) {
        init(allowedFieldSet);
    }

    /**
     * Constructs a FieldEffects whose allowed field set, changed field set, and created field
     * vector match the specified sets/vectors.
     */
    FieldEffects(const FieldSet& allowedFieldSetIn,
                 const FieldSet& changedFieldSetIn,
                 const CreatedFieldVectorType& createdFieldVec) {
        std::vector<std::string> createdFields;
        for (const auto& [field, _] : createdFieldVec) {
            createdFields.emplace_back(field);
        }

        auto changedFieldSet = changedFieldSetIn;
        changedFieldSet.setUnion(FieldSet::makeClosedSet(std::move(createdFields)));

        auto allowedFieldSet = allowedFieldSetIn;
        allowedFieldSet.setUnion(changedFieldSet);

        init(allowedFieldSet, changedFieldSet, createdFieldVec);
    }

    /**
     * Constructs a FieldEffects with the specified field/effect pairs and with the specified
     * default effect. All the strings in 'fields' must be unique, and 'defaultEffects' must be
     * kKeep or kDrop.
     */
    FieldEffects(std::vector<std::string> fields,
                 const std::vector<FieldEffect>& effects,
                 FieldEffect defaultEffect);

    /**
     * For each field, compose() will compute the composition of the child's ('*this') effect
     * on the field with the parent's ('parent') effect on the field.
     *
     * Given two effects, the "composed" effect is computed using the following table:
     *
     *   Parent's effect  | Child's effect  | Composed effect
     *   -----------------+-----------------+----------------
     *   Generic          | Any effect      | Generic
     *   Add              | Any effect      | Add
     *   Drop             | Any effect      | Drop
     *   Set              | Drop            | Add
     *   Set              | Keep/Modify     | Set
     *   Set              | Other effect    | Generic
     *   Modify           | Keep/Modify     | Modify
     *   Modify           | Other effect    | Child's effect
     *   Keep             | Any effect      | Child's effect
     *
     * compose() is associative, but not commutative. The "Keep" effect behaves as the neutral
     * element for compose().
     */
    void compose(const FieldEffects& parent);

    /**
     * This method "narrows" this FieldEffects's non-Keep effects to the set of fields
     * specified by 'domainSet'.
     *
     * For each field in 'domainSet', the field's effect will stay the same. For each field
     * not in 'domainSet', the field's effect will be changed to Keep.
     */
    void narrow(const FieldSet& domainSet);

    /**
     * Returns the list of explicitly tracked fields.
     */
    const std::vector<std::string>& getFieldList() const {
        return _fields;
    }

    /**
     * Returns the "default" FieldEffect.
     */
    FieldEffect getDefaultEffect() const {
        return _defaultEffect;
    }

    /**
     * Returns the FieldEffect for the specified field.
     */
    FieldEffect get(StringData field) const {
        auto it = _effects.find(field);
        return it != _effects.end() ? it->second : _defaultEffect;
    }

    inline bool isAllowedField(StringData field) const {
        auto effect = get(field);
        return effect != FieldEffect::kDrop;
    }
    inline bool isChangedField(StringData field) const {
        auto effect = get(field);
        return effect != FieldEffect::kKeep && effect != FieldEffect::kDrop;
    }
    inline bool isCreatedField(StringData field) const {
        auto effect = get(field);
        return effect != FieldEffect::kKeep && effect != FieldEffect::kDrop &&
            effect != FieldEffect::kModify;
    }

    /**
     * Returns a FieldSet containing all the fields whose effect is not kDrop.
     *
     * If there are a _finite_ number of fields whose effect is not kDrop, then this function will
     * return a "closed" FieldSet, otherwise it will return an "open" FieldSet.
     */
    FieldSet getAllowedFields() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is kDrop.
     *
     * If there are a _finite_ number of fields whose effect is kDrop, then this function will
     * return a "closed" FieldSet, otherwise it will return an "open" FieldSet.
     */
    FieldSet getDroppedFields() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is not kKeep or kDrop.
     *
     * This function always returns a "closed" FieldSet.
     */
    FieldSet getChangedFields() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is not kKeep.
     *
     * If there are a _finite_ number of fields whose effect is kDrop, then this function will
     * return a "closed" FieldSet, otherwise it will return an "open" FieldSet.
     */
    FieldSet getDroppedOrChangedFields() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is not kKeep, kDrop, or kModify.
     *
     * This function always returns a "closed" FieldSet.
     */
    FieldSet getCreatedFields() const;

    /**
     * Returns a vector of pairs corresponding to all the fields whose effect is kSet, kAdd,
     * or kGeneric. The first part of each pair gives the field name, and the second part of
     * each pair gives the field's Effect.
     */
    CreatedFieldVectorType getCreatedFieldVector() const;

    template <typename FuncT>
    FieldSet getFieldsWithEffects(const FuncT& fn) const {
        std::vector<std::string> fields;
        bool isClosed = !fn(_defaultEffect);

        for (auto&& field : _fields) {
            if (fn(get(field)) == isClosed) {
                fields.emplace_back(field);
            }
        }

        FieldListScope scope = isClosed ? FieldListScope::kClosed : FieldListScope::kOpen;
        return FieldSet(std::move(fields), scope);
    }

    /**
     * Returns true if at least one field has an effect that is not Drop. This method provides a
     * more efficient way to compute '!getAllowedFields().isEmptySet()'.
     */
    bool hasAllowedFields() const;

    /**
     * Returns true if at least one field has a Drop effect. This method provides a more efficient
     * way to compute '!getAllowedFields().isUniverseSet()'.
     */
    bool hasDroppedFields() const;

    /**
     * Returns true if at least one field has an effect that is not Keep or Drop. This method
     * provides a more efficient way to compute '!getChangedFields().isEmptySet()'.
     */
    bool hasChangedFields() const;

    /**
     * Returns true if at least one field has an effect that is not Keep. This method provides a
     * more efficient way to compute 'hasDroppedFields() || hasChangedFields()'.
     */
    bool hasDroppedOrChangedFields() const;

    /**
     * Returns true if at least one field has an effect that is not Keep, Drop, or Modify. This
     * method provides a more efficient way to compute '!getCreatedFields().isEmptySet()'.
     */
    bool hasCreatedFields() const;

    template <typename FuncT>
    bool hasFieldsWithEffects(const FuncT& fn) const {
        return fn(_defaultEffect) ||
            std::any_of(_effects.begin(), _effects.end(), [&](auto&& p) { return fn(p.second); });
    }

    void setFieldOrder(const std::vector<std::string>& order);

    void removeRedundantEffects();

    std::string toString() const;

private:
    void init(const FieldSet& allowedFieldSet);

    void init(const FieldSet& allowedFieldSet,
              const FieldSet& changedFieldSet,
              const CreatedFieldVectorType& createdFieldVec);

    std::vector<std::string> _fields;
    StringMap<FieldEffect> _effects;
    FieldEffect _defaultEffect = FieldEffect::kKeep;
};

inline bool isNonSpecificEffect(FieldEffect effect) {
    return effect == FieldEffect::kGeneric;
}

inline bool hasNonSpecificEffects(const FieldEffects& effects) {
    return effects.hasFieldsWithEffects(&isNonSpecificEffect);
}

FieldSet makeAllowedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes);

FieldSet makeModifiedOrCreatedFieldSet(bool isInclusion,
                                       const std::vector<std::string>& paths,
                                       const std::vector<ProjectNode>& nodes);

FieldSet makeCreatedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes);

/**
 * This method returns the merged effect for the two specified FieldEffects. This function is
 * both commutative and associative.
 */
FieldEffect getMergedEffect(FieldEffect e1, FieldEffect e2);

/**
 * This method merges two FieldEffects into a single FieldEffects if possible, otherwise it returns
 * boost::none. This function is not a closed operation (as it can return boost::none), and it is
 * neither commmutative nor associative.
 */
boost::optional<FieldEffects> mergeEffects(const FieldEffects& e1, const FieldEffects& e2);

boost::optional<FieldEffects> mergeEffects(const std::vector<FieldEffects>& args);

/**
 * If the narrowed effects of a projection ('narrowedProjEffects') are compatible with a ResultInfo
 * req's effects ('resultInfoEffects'), this function will return 'narrowedProjEffects' composed
 * with 'resultInfoEffects'.
 *
 * Otherwise, this function returns boost::none.
 */
boost::optional<FieldEffects> composeEffectsForResultInfo(const FieldEffects& narrowedProjEffects,
                                                          const FieldEffects& resultInfoEffects);

bool canUseCoveredProjection(const QuerySolutionNode* root,
                             bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes);

std::pair<std::vector<std::string>, std::vector<ProjectNode>> sortPathsAndNodesForCoveredProjection(
    const QuerySolutionNode* qsNode,
    std::vector<std::string> paths,
    std::vector<ProjectNode> nodes);

std::vector<ProjectNode::Type> getTransformedNodeTypesForCoveredProjection(size_t numNewPaths);

FieldEffects getTransformedEffectsForCoveredProjection(const QuerySolutionNode* root,
                                                       const std::vector<std::string>& paths);

/**
 * This struct is used to stash common info about a projection QSN node (ProjectionNodeSimple,
 * ProjectionNodeDefault, and ProjectionNodeCovered).
 */
struct ProjectionInfo {
    ProjectionInfo(bool isInclusion,
                   std::vector<std::string> paths,
                   std::vector<ProjectNode> nodes,
                   bool isCoveredProjection,
                   std::vector<std::string> depFields,
                   bool needWholeDocument);

    bool isInclusion = false;
    bool isCoveredProjection = false;
    bool needWholeDocument = false;
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
    std::vector<std::string> depFields;
};

/**
 * We use one of these structs per node in the QSN tree to store the results of the
 * analyze phase.
 */
struct QsnInfo {
    QsnInfo() = default;
    ~QsnInfo() = default;

    QsnInfo(const QsnInfo&) = delete;
    QsnInfo(QsnInfo&&) = delete;

    QsnInfo& operator=(const QsnInfo&) = delete;
    QsnInfo& operator=(QsnInfo&&) = delete;

    void setPostimageAllowedFields(FieldSet allowedFields) {
        postimageAllowedFieldsStorage.emplace(std::move(allowedFields));
        postimageAllowedFields = &*postimageAllowedFieldsStorage;
    }
    void setPostimageAllowedFields(const FieldSet* allowedFields) {
        postimageAllowedFields = allowedFields;
    }

    // For all QSN stages, 'postimageAllowedFields' and 'postimageAllowedFieldsStorage' should be
    // initialized during QSN analysis via one of the setPostimageAllowedFields() methods.
    const FieldSet* postimageAllowedFields = nullptr;
    boost::optional<FieldSet> postimageAllowedFieldsStorage;

    // If the corresponding QSN stage is capable of participating with result info reqs (i.e. if
    // the QSN stage's buildXXX() method has been updated to participate with result info reqs when
    // possible), then this field should be initialized during QSN analysis with the FieldEffects
    // for the QSN stage. If the corresponding QSN stage is _not_ capable of participating with
    // result info reqs, then this field should be set to boost::none.
    boost::optional<FieldEffects> effects;

    // If the corresponding QSN stage is a projection, this field will be initialized during QSN
    // analysis. Otherwise this field will be null. See the ProjectionInfo struct for more detail
    // about what kind of information is stored here.
    std::unique_ptr<ProjectionInfo> projectionInfo;
};

/**
 * The QsnAnalysis class is designed to analyze a QuerySolutionNode (QSN) tree and store information
 * about each node in the tree.
 *
 * The analyzeTree() method will perform analysis on the specified tree and populate QsnAnalysis's
 * internal maps. After analyzeTree() is called, the getQsnInfo() method can be used to retrieve
 * info about a specific node in the tree.
 */
class QsnAnalysis {
public:
    static const FieldSet kEmptyFieldSet;
    static const FieldSet kUniverseFieldSet;
    static const FieldEffects kAllKeepEffects;
    static const FieldEffects kAllDropEffects;

    void analyzeTree(const QuerySolutionNode* root);

    inline const QsnInfo& getQsnInfo(const QuerySolutionNode* qsNode) const {
        auto it = _analysis.find(qsNode);
        tassert(8323506, "Expected to find QsnInfo for specified qsNode", it != _analysis.end());

        return *it->second;
    }
    inline const QsnInfo& getQsnInfo(const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return getQsnInfo(qsNode.get());
    }

    bool hasProjectionInfo(const QuerySolutionNode* qsNode) const {
        return getQsnInfo(qsNode).projectionInfo.get() != nullptr;
    }
    bool hasProjectionInfo(const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return hasProjectionInfo(qsNode.get());
    }

    const ProjectionInfo& getProjectionInfo(const QuerySolutionNode* qsNode) const {
        tassert(8323511, "Expected ProjectionInfo to be set", hasProjectionInfo(qsNode));
        return *getQsnInfo(qsNode).projectionInfo;
    }
    const ProjectionInfo& getProjectionInfo(
        const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return getProjectionInfo(qsNode.get());
    }

private:
    void analyzeQsNode(const QuerySolutionNode* qsNode, QsnInfo& qsnInfo);

    absl::flat_hash_map<const QuerySolutionNode*, std::unique_ptr<QsnInfo>> _analysis;
};
}  // namespace stage_builder
}  // namespace mongo
