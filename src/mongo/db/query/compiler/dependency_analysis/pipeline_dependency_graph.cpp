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

#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"
#include "mongo/util/string_map.h"

#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pipeline::dependency_graph {
namespace {
/**
 * Strongly typed alias for int to avoid mixing IDs of different types.
 */
template <typename T>
struct TypedId {
    using ValueType = int32_t;

    static constexpr TypedId none() {
        return TypedId{};
    }

    explicit operator bool() const {
        return *this != none();
    };

    template <typename H>
    friend H AbslHashValue(H h, TypedId id) {
        return H::combine(std::move(h), id.value);
    }

    friend auto operator<=>(const TypedId&, const TypedId&) = default;

    ValueType value{-1};
};

/**
 * NodeContainer stores nodes, when inserting the data structure appends at the end and deals with
 * memory management. Node IDs are unique in the data structure, and are monotonically increasing.
 */
template <typename T>
struct NodeContainer {
    using Id = TypedId<T>;

    T& operator[](Id id) {
        return _nodes.at(id.value);
    }

    const T& operator[](Id id) const {
        return _nodes.at(id.value);
    }

    bool empty() const {
        return _nodes.empty();
    }

    size_t size() const {
        return _nodes.size();
    }

    T& back() {
        return _nodes.back();
    }

    const T& back() const {
        return _nodes.back();
    }

    void eraseFrom(Id id) {
        _nodes.erase(_nodes.begin() + id.value, _nodes.end());
    }

    Id append(T&& t) {
        Id id = getNextId();
        _nodes.emplace_back(std::move(t));
        return id;
    }

    Id getLastId() const {
        return _nodes.empty() ? Id::none() : Id(_nodes.size() - 1);
    }

    Id getNextId() const {
        return Id(_nodes.size());
    }

    friend bool operator==(const NodeContainer&, const NodeContainer&) = default;

private:
    std::vector<T> _nodes;
};

class StringPool {
public:
    using Id = TypedId<StringPool>;

    Id intern(StringData str) {
        auto it = _stringToId.find(str);
        if (it != _stringToId.end()) {
            return it->second;
        }
        Id id(_strings.size());
        _strings.emplace_back(str);
        _stringToId.emplace_hint(it, _strings.back(), id);
        return id;
    }

    Id lookup(StringData str) const {
        if (auto it = _stringToId.find(str); it != _stringToId.end()) {
            return it->second;
        }
        return Id::none();
    }

    StringData get(Id id) const {
        return _strings.at(id.value);
    }

private:
    std::vector<std::string> _strings;
    StringMap<Id> _stringToId;
};

struct Stage;
struct Scope;
struct Field;

using StageId = TypedId<Stage>;
using ScopeId = TypedId<Scope>;
using FieldId = TypedId<Field>;

using FieldMap = absl::flat_hash_map<StringPool::Id, FieldId>;

/// Represents the set of field definition nodes that a stage or a field definition node depends on.
/// When a stage depends on a field definition, it means that the stage references a field that was
/// most recently modified by that field definition node. Similarly, field definition A can depend
/// on field definition B if A references B through a rename or in an expression.
using FieldDependencies = absl::flat_hash_set<FieldId>;

/**
 * Represents a DocumentSource that references or defines fields (or both).
 */
struct Stage {
    Stage(ScopeId scope,
          boost::intrusive_ptr<DocumentSource> documentSource,
          FieldDependencies dependencies,
          bool isSingleDocumentTransformation,
          ScopeId nextNewScope)
        : documentSource(std::move(documentSource)),
          dependencies(std::move(dependencies)),
          scope(scope),
          nextNewScope(nextNewScope),
          isSingleDocumentTransformation(isSingleDocumentTransformation) {}

    boost::intrusive_ptr<DocumentSource> documentSource;
    FieldDependencies dependencies;
    // The scope representing visible definitions immediately _after_ this stage. Stages which
    // don't modify fields just inherit this from the previous stage.
    ScopeId scope;
    // Points to the next new top-level scope. If this stage introduces a new scope, then
    // scope == nextNewScope. Otherwise this either points to a real scope created by some later
    // Stage, or to scopes.end(). Used only for finding the scope to invalidate before rebuilding
    // the graph.
    ScopeId nextNewScope;
    bool isSingleDocumentTransformation;
};

/**
 * Represents the full set of known field definitions that are visible at a specific point in the
 * pipeline.
 *
 * Scopes and Fields together form a trie-like structure. The Scope associated with each stage only
 * contains the visible top-level field definitions, which each point to all visible sub-field
 * definitions and so on.
 *
 * For example, fields and scopes in the pipeline [{$set: {"foo.bar": 1}}, {$set: {"foo.baz": 1}}]
 * would be represented like this:
 *   - fields = {
 *       Field{declaringScope=ScopeId(0), embeddedScope=ScopeId(1)}, // Id 0
 *       Field{declaringScope=ScopeId(1), Metadata=Value(1)},        // Id 1
 *       Field{declaringScope=ScopeId(2), embeddedScope=ScopeId(3)}, // Id 2
 *       Field{declaringScope=ScopeId(3), Metadata=Value(1)},        // Id 3
 *     }
 *   - scopes = {
 *       Scope{fields={"foo": FieldId(0)}, stage=StageId(0)},  // Id 0
 *       Scope{fields={"bar": FieldId(1)}, stage=StageId(0)},  // Id 1
 *       Scope{fields={"foo": FieldId(2)}, stage=StageId(1)},  // Id 2
 *       // Note that "bar" is not redefined here, so this scope points to the original definition.
 *       Scope{fields={"bar": FieldId(1), "baz": FieldId(3)}, stage=StageId(1)},  // Id 3
 *     }
 */
struct Scope {
    Scope(StageId stage, ScopeId exhaustiveScope, FieldId missingField, FieldMap fields = {})
        : fields(std::move(fields)),
          stage(stage),
          exhaustiveScope(exhaustiveScope),
          missingField(missingField) {}

    // Set of fields visible in this scope.
    FieldMap fields;
    // Declaring stage.
    StageId stage;
    // If valid, points to the scope which is considered to have modified any unknown fields.
    // For a {$group} followed by {$set}, the $group scope has exhaustiveScope = $group.scope
    // (self), the $set scope has exhaustiveScope = $group.scope also, since any field not known
    // to $set is considered modified by $group. If we do not have an exhaustiveScope, the field
    // can be assumed to come from the document.
    ScopeId exhaustiveScope;
    // FieldId for field made "missing" by this stage.
    // This is also the minimum FieldId declared by this stage.
    FieldId missingField;
};

/**
 * Represents a definition of a single path component. Each redefinition of a field is represented
 * as a separate node.
 */
struct Field {
    /**
     * Holds arrayness information or a constant value (if known).
     *
     * TODO(SERVER-119384): Implement arrayness tracking
     * TODO(SERVER-119392): Implement constant propagation
     */
    struct Metadata {};

    Field(ScopeId declaringScope,
          ScopeId embeddedScope = ScopeId::none(),
          FieldDependencies dependencies = {},
          Metadata metadata = {})
        : metadata(std::move(metadata)),
          dependencies(std::move(dependencies)),
          declaringScope(declaringScope),
          embeddedScope(embeddedScope) {}

    // This could also be stored as a node, and referenced here by its ID.
    Metadata metadata{};
    FieldDependencies dependencies{};
    // Scope declaring this field.
    // NOTE: We might not need to track this, since we know the scope in lookupField.
    ScopeId declaringScope{ScopeId::none()};
    // Embedded scope containing nested fields (e.g. 'a.b').
    ScopeId embeddedScope{ScopeId::none()};
};

// Information extracted from DocumentSource to populate the graph.
class DocumentSourceInfo {
public:
    explicit DocumentSourceInfo(const DocumentSource& documentSource)
        : _documentSource(documentSource),
          _isSingleDocumentTransformation(documentSource.getId() ==
                                          DocumentSourceSingleDocumentTransformation::id) {
        documentSource.getDependencies(&_deps);
    }

    void describeTransformation(document_transformation::DocumentOperationVisitor& visitor) const {
        _documentSource.describeTransformation(visitor);
    }

    const OrderedPathSet& getPathDependencies() const {
        return _deps.fields;
    }

    bool isSingleDocumentTransformation() const {
        return _isSingleDocumentTransformation;
    }

private:
    const DocumentSource& _documentSource;
    // Dependencies.
    DepsTracker _deps;
    // Is the document source an instance of DocumentSourceSingleDocumentTransformation.
    bool _isSingleDocumentTransformation;
};

static_assert(document_transformation::DescribesDocumentTransformation<DocumentSourceInfo>);
}  // namespace

class DependencyGraph::Impl {
public:
    Impl(const DocumentSourceContainer& container) {
        recompute(container);
    }

    boost::intrusive_ptr<mongo::DocumentSource> getDeclaringStage(DocumentSource* ds,
                                                                  PathRef path) const {
        auto stageId = getStageId(ds);
        auto scopeId = _stages[stageId].scope;
        auto parsedPath = parsePath(path);
        if (auto [fieldId, _] = lookupField(scopeId, parsedPath); fieldId) {
            ScopeId declaringScopeId = _fields[fieldId].declaringScope;
            StageId declaringStageId = _scopes[declaringScopeId].stage;
            return _stages[declaringStageId].documentSource;
        }

        return nullptr;
    }

    void recompute(const DocumentSourceContainer& container,
                   boost::optional<DocumentSourceContainer::const_iterator> stageIt = {}) {
        auto recomputeFrom = stageIt ? *stageIt : container.begin();
        invalidate(container, recomputeFrom);
        for (auto it = recomputeFrom; it != container.end(); it++) {
            DocumentSourceInfo dsInfo{**it};
            processStage(*it, dsInfo);
        }
    }

    size_t numStages() const;
    BSONObj toBSON() const;

private:
    using ParsedPath = boost::container::small_vector<StringPool::Id, 8>;
    using ParsedPathView = std::span<StringPool::Id>;

    class Serializer;

    /**
     * Result from field lookup. If found, the fieldId field is set. If the looked up field was
     * shadowed by another, we return the shadowing field instead. This is the case of looking up
     * for 'a.b' when 'a' was set previously, shadowing any subfields.
     */
    struct FieldLookupResult {
        FieldId fieldId{FieldId::none()};
        bool shadowed{false};
    };

    /**
     * Declares a scope (or embedded scope), which is defined by the given state and
     * inherits fields from parentScope (if any). The scope missing field is also created.
     */
    void declareScope(StageId stage, ScopeId exhaustiveScope, ScopeId parentScope) {
        auto scopeId = _scopes.getNextId();
        auto missingField = _fields.append(Field{scopeId});
        _scopes.append(Scope{
            stage,
            exhaustiveScope,
            missingField,
        });
        if (parentScope) {
            _scopes[scopeId].fields = _scopes[parentScope].fields;
        }
    }

    /**
     * Declare a field for the given (possibly dotted) path in the scope.
     * For a path like 'a.b' declares 'a' with embedded scope holding 'b'.
     * If 'a' already exists, any fields are preserved.
     * Returns the FieldId for the base component in path (for 'a.b' returns 'a').
     */
    FieldId declareField(ScopeId scope, ParsedPathView path, FieldDependencies dependencies) {
        // Declaring 'a' should create field 'a' and exit.
        if (path.size() == 1) {
            auto field = _fields.append(Field{scope, ScopeId::none(), std::move(dependencies)});
            _scopes[scope].fields[path.front()] = field;
            return field;
        }

        // Declaring a.b should use the scope for 'a' or declare a new embedded scope, in which to
        // create the subpath 'b'.
        auto basePath = path.subspan(0, 1);
        auto subPath = path.subspan(1);

        // Check if we already have 'a' in the current scope that we are building. If we do, we will
        // preserve any fields it may already contain.
        auto existingBaseField = lookupFullPath(scope, basePath);

        // Scope for declaring 'b' field of 'a'.
        FieldId newBaseField;
        if (existingBaseField && _fields[existingBaseField].declaringScope == scope &&
            _fields[existingBaseField].embeddedScope) {
            // We already have such base field in the current scope. We can mutate the scope in this
            // case.
            newBaseField = existingBaseField;
        } else {
            // No such field, we need to declare it and an embedded scope;
            // OR existing base field from previous scope, we need to re-declare the base here with
            // updated embedded scope.
            auto embeddedScope = _scopes.getNextId();
            newBaseField = _fields.append(Field{scope, embeddedScope});
            auto parentEmbeddedScope =
                existingBaseField ? _fields[existingBaseField].embeddedScope : ScopeId::none();
            // We should only set the exhaustiveScope as ScopeId::none() if we can verify the field
            // comes from the document and cannot be modified elsewhere. If baseField ==
            // FieldId::none(), it comes from the document. If baseField == <missing>, it is
            // modified by the scope definining it.
            auto exhaustiveEmbeddedScope = existingBaseField
                ? _scopes[_fields[existingBaseField].declaringScope].exhaustiveScope
                : ScopeId::none();
            if (parentEmbeddedScope) {
                // The new field depends on the previous field, since it inherits paths.
                _fields[newBaseField].dependencies.emplace(existingBaseField);
            }
            declareScope(_scopes[scope].stage, exhaustiveEmbeddedScope, parentEmbeddedScope);
            _scopes[scope].fields[basePath.front()] = newBaseField;
        }
        // Finally, declare the subPath in the embeddedScope we found or created.
        auto embeddedField =
            declareField(_fields[newBaseField].embeddedScope, subPath, std::move(dependencies));
        _fields[newBaseField].dependencies.emplace(embeddedField);
        return newBaseField;
    }

    /**
     * Includes the field from the parent scope into the given scope.
     * This has the semantics of an inclusion projection.
     */
    void includeField(ScopeId scope, ScopeId parentScope, ParsedPathView path) {
        // Including 'a' should reference field 'a' and exit.
        if (path.size() == 1) {
            auto existingField = lookupFullPath(parentScope, path);
            if (existingField && existingField != _scopes[parentScope].missingField) {
                _scopes[scope].fields[path.front()] = existingField;
                return;
            }
            // TODO (SERVER-119374): Deps should be either missingField or whatever we use to
            // represent a field from the base collection.
            declareField(scope, path, {});
            return;
        }

        // Including a.b:
        // - if 'a' already included in the current scope (maybe 'a.c' was included), call
        // includeField for 'b'.
        // - if 'a' is not included in the current scope, declare 'a', then include 'b' into it.
        auto basePath = path.subspan(0, 1);
        auto subPath = path.subspan(1);

        auto previousBaseField = lookupFullPath(parentScope, basePath);
        auto existingBaseField = lookupFullPath(scope, basePath);

        if (existingBaseField && existingBaseField != _scopes[scope].missingField) {
            // Due to the rule that we cannot have clashing paths, we know that we cannot have just
            // "include a" and then "include a.b".
            tassert(11937306, "Clashing paths", _fields[existingBaseField].embeddedScope);
            // Since we are calling includeField, we expect that we cannot already have 'a' included
            // trivially either.
            tassert(11936307, "Already exists", _fields[existingBaseField].declaringScope == scope);

            if (previousBaseField && _fields[previousBaseField].embeddedScope) {
                // Great, we know exactly where this 'b' comes from, just re-include in 'a'.
                includeField(_fields[existingBaseField].embeddedScope,
                             _fields[previousBaseField].embeddedScope,
                             subPath);
                return;
            }
            // We cannot figure out where this 'b' comes from. We will re-declare it here.
            // TODO (SERVER-119374): How to represent fields that come from the base collection?
            declareField(_fields[existingBaseField].embeddedScope, subPath, {});
            return;
        }

        // 'a' is not included in the current scope, so we need to declare it then include 'b'.
        auto newBaseField = declareField(scope, basePath, {});
        if (previousBaseField && _fields[previousBaseField].embeddedScope) {
            // We know about 'a' from before, so we can include 'b'.
            auto embeddedScope = _scopes.getNextId();
            declareScope(_scopes[scope].stage, embeddedScope, ScopeId::none());
            _fields[newBaseField].embeddedScope = embeddedScope;
            includeField(embeddedScope, _fields[previousBaseField].embeddedScope, subPath);
            return;
        }

        // We do not know about 'a', so we don't know what is being included at all.
        // We will need to just declare 'a.b' anew.
        // TODO (SERVER-119374): How to represent fields that come from the base collection?
        declareField(scope, path, {});
    }

    /**
     * Gets the Field node that defines the given path in the given scope.
     */
    FieldLookupResult lookupField(ScopeId scopeId, ParsedPathView path) const {
        const auto& scope = _scopes[scopeId];
        auto fieldNameId = path.front();
        if (auto it = scope.fields.find(fieldNameId); it != scope.fields.end()) {
            auto fieldId = it->second;
            if (!fieldId) {
                // Not found.
                return {FieldId::none()};
            }
            if (path.size() == 1) {
                // This is the last component in the dotted path we're looking for.
                return {fieldId};
            }
            if (!_fields[fieldId].embeddedScope) {
                // We found 'a', but it has no embedded scope and we are looking for 'a.b'.
                return FieldLookupResult{fieldId, true /*shadowed*/};
            }
            // We're resolving a dotted path and there are known subpaths.
            return lookupField(_fields[fieldId].embeddedScope, path.subspan(1));
        }

        if (scope.exhaustiveScope) {
            // Not every possible field is known; the field could be coming from the base document.
            return {_scopes[scope.exhaustiveScope].missingField};
        }

        return {FieldId::none()};
    }

    /**
     * Same as 'lookupField()', but returns FieldId::none() unless the full requested path is in
     * scope.
     */
    FieldId lookupFullPath(ScopeId scopeId, ParsedPathView path) const {
        auto [fieldId, shadowed] = lookupField(scopeId, path);
        return shadowed ? FieldId::none() : fieldId;
    }

    /**
     * Gets the stage node that represents the given DocumentSource in the graph.
     */
    StageId getStageId(DocumentSource* ds) const {
        if (!ds) {
            return _stages.getLastId();
        }
        if (auto it = _dsToStageId.find(ds); it != _dsToStageId.end()) {
            return it->second;
        }
        tasserted(11937307, "Unknown DocumentSource");
    }

    /**
     * Create graph nodes to represent the scope declared by the document source.
     */
    ScopeId processScope(const DocumentSourceInfo& ds,
                         StageId stage,
                         ScopeId parentScope,
                         const FieldDependencies& depsFromStage) {
        using namespace mongo::document_transformation;
        const ScopeId scopeId = _scopes.getNextId();

        // If the stage modified any fields, we will declare a scope.
        // Otherwise, we will reuse the previous scope.
        bool hasDeclaredScope = false;
        // We delay creating the scope until the first operation we observe.
        auto maybeDeclareInheritedScope = [&]() {
            if (!hasDeclaredScope) {
                ScopeId exhaustiveScope =
                    parentScope ? _scopes[parentScope].exhaustiveScope : ScopeId::none();
                // Parent scope names are valid.
                declareScope(stage, exhaustiveScope, parentScope);
                hasDeclaredScope = true;
            }
        };

        document_transformation::describeTransformation(
            OverloadedVisitor{
                [&](const ReplaceRoot&) {
                    // TODO(SERVER-119374): Revisit how to handle kNotSupported and kAllPaths.
                    // All paths might be modified. This scope does not inherit the parent scope
                    // fields. It also doesn't know all fields - all lookups will fail and we should
                    // say we have no information about the field (as opposed to saying the field is
                    // definitely missing like for kAllExcept).
                    declareScope(stage, scopeId /*exhaustiveScope*/, ScopeId::none());
                    tassert(
                        11996201, "Did not expect ReplaceRoot in this position", !hasDeclaredScope);
                    hasDeclaredScope = true;
                },
                [&](const PreservePath& p) {
                    tassert(11996202, "Expected operation before PreservePath", hasDeclaredScope);
                    auto parsedPath = internPath(p.getPath());
                    includeField(scopeId, parentScope, parsedPath);
                },
                [&](const ModifyPath& p) {
                    maybeDeclareInheritedScope();
                    auto parsedPath = internPath(p.getPath());
                    declareField(scopeId, parsedPath, depsFromStage);
                },
                [&](const RenamePath& p) {
                    maybeDeclareInheritedScope();
                    auto parsedOldPath = internPath(p.getOldPath());
                    auto parsedNewPath = internPath(p.getNewPath());

                    BSONDepthIndex oldPathArrays = p.getOldPathMaxArrayTraversals();
                    BSONDepthIndex newPathArrays = p.getNewPathMaxArrayTraversals();

                    if (oldPathArrays == 0 && newPathArrays == 0) {
                        if (parentScope) {
                            if (auto dependency = lookupFullPath(parentScope, parsedOldPath)) {
                                // We know exactly which 1 field this one depends on.
                                declareField(scopeId, parsedNewPath, {dependency});
                            } else {
                                declareField(scopeId, parsedNewPath, depsFromStage);
                            }
                        } else {
                            tasserted(11937303, "Unimplemented renames when no parent scope");
                        }
                    } else if (oldPathArrays == 0 && newPathArrays == 1) {
                        if (parentScope) {
                            tasserted(11937305, "Unimplemented complex renames");
                        } else {
                            tasserted(11937304,
                                      "Unimplemented complex renames when no parent scope");
                        }
                    } else {
                        // Treat potential reshaping as path modification, not as rename.
                        declareField(scopeId, parsedNewPath, depsFromStage);
                    }
                },
            },
            ds);

        // Special case: When this is the first stage and it did not modify anything,
        // we create a empty initial scope.
        if (!parentScope && !hasDeclaredScope) {
            maybeDeclareInheritedScope();
            return scopeId;
        }

        return hasDeclaredScope ? scopeId : parentScope;
    }

    /**
     * Creates graph nodes to represent the document source.
     */
    void processStage(boost::intrusive_ptr<DocumentSource> documentSource,
                      const DocumentSourceInfo& dsInfo) {
        StageId stageId = _stages.getNextId();
        _dsToStageId.emplace(documentSource.get(), stageId);

        auto parentScopeId = _stages.empty() ? ScopeId::none() : _stages.back().scope;
        FieldDependencies dependencies;
        if (parentScopeId) {
            for (auto&& path : dsInfo.getPathDependencies()) {
                auto parsedPath = internPath(path);
                auto fieldId = lookupFullPath(parentScopeId, parsedPath);
                if (fieldId) {
                    dependencies.insert(fieldId);
                }
            }
        }
        const auto nextNewScopeId = _scopes.getNextId();
        auto scopeId = processScope(dsInfo, stageId, parentScopeId, dependencies);

        _stages.append(Stage{scopeId,
                             std::move(documentSource),
                             std::move(dependencies),
                             dsInfo.isSingleDocumentTransformation(),
                             nextNewScopeId});
    }

    /**
     * Clears the DocumentSource <-> StageId mapping and rebuilds it, stopping at 'end'. Returns a
     * StageId of the next stage after 'end'.
     */
    StageId clearAndRebuildMapping(const DocumentSourceContainer& container,
                                   DocumentSourceContainer::const_iterator stageIt) {
        _dsToStageId.clear();
        // Rebuild mapping from begin to stageIt.
        StageId index{0};
        for (auto it = container.begin(); it != stageIt; it++) {
            _dsToStageId.emplace(it->get(), index);
            ++index.value;
        }
        return index;
    }

    /**
     * Find the lowest ID scope and field nodes corresponding to the given stage.
     * These IDs can be used to determine the valid portion of a graph.
     * If the range [container.begin(), stageIt) is valid, then so are
     * [0, minId) for all node types.
     */
    std::tuple<ScopeId, FieldId> earliestDescendants(StageId stageId) const {
        if (stageId.value < 1) {
            return {ScopeId{0}, FieldId{0}};
        }

        ScopeId scopeId = _stages[stageId].nextNewScope;
        FieldId fieldId = scopeId == _scopes.getNextId()
            // No scope to invalidate so no fields to invalidate.
            ? _fields.getNextId()
            // Invalidate every field declared by or after the scope.
            : _scopes[scopeId].missingField;

        return {scopeId, fieldId};
    }

    /**
     * Invalidate the portion of the graph corresponding to [stageIt, container.end()).
     */
    void invalidate(const DocumentSourceContainer& container,
                    DocumentSourceContainer::const_iterator stageIt) {
        // TODO(SERVER-119842): See if we can just leave the dangling entries
        StageId invalidStage = clearAndRebuildMapping(container, stageIt);

        // Invalidate all nodes originating from the given stage.
        auto [invalidScope, invalidField] = earliestDescendants(invalidStage);
        _stages.eraseFrom(invalidStage);
        _scopes.eraseFrom(invalidScope);
        _fields.eraseFrom(invalidField);

        LOGV2_DEBUG(11937301,
                    5,
                    "Invalidated the dependency graph",
                    "invalidField"_attr = invalidField.value,
                    "invalidScope"_attr = invalidScope.value,
                    "invalidStage"_attr = invalidStage.value);
    }

    /**
     * Interns the path components and returns a list of string IDs.
     */
    ParsedPath internPath(PathRef path) {
        ParsedPath vec;
        for (auto s : std::views::split(path, '.')) {
            vec.push_back(_strings.intern({s.begin(), s.end()}));
        }
        return vec;
    }

    /**
     * Maps the path components to the interned strings.
     * Unknown components are replaced with NoStringId.
     */
    ParsedPath parsePath(PathRef path) const {
        ParsedPath vec;
        for (auto s : std::views::split(path, '.')) {
            auto id = _strings.lookup({s.begin(), s.end()});
            vec.push_back(id);
        }
        return vec;
    }

    // Stages map 1:1 to DocumentSources and are stored in the same order. If DocumentSource at
    // position 5 is modified, we invalidate stages from that index onwards.
    NodeContainer<Stage> _stages;

    // Scopes are referenced by stages. *Some* stages can declare a scope. Others inherit the scope
    // from the previous stage. Scopes are also appended at the end of the container. This means
    // that if stage N is erased and it declares a scope, we can erase all scopes past the scope
    // declared by the stage.
    NodeContainer<Scope> _scopes;

    // Fields referenced by scopes and are added in declaration order. That is, stages append fields
    // to the end. Each stage defines a "missing" field to start. If a scope is removed, we can
    // erase everything past the missing field for that scope.
    NodeContainer<Field> _fields;

    // String interning pool. Each entry is a path component.
    StringPool _strings;

    // Mapping between DocumentSource and StageId, recomputed when the graph is recomputed.
    absl::flat_hash_map<const DocumentSource*, StageId> _dsToStageId;
};

DependencyGraph::DependencyGraph(const DocumentSourceContainer& container)
    : _impl(std::make_unique<Impl>(container)) {}

DependencyGraph::~DependencyGraph() = default;
DependencyGraph::DependencyGraph(DependencyGraph&&) noexcept = default;
DependencyGraph& DependencyGraph::operator=(DependencyGraph&&) noexcept = default;

boost::intrusive_ptr<mongo::DocumentSource> DependencyGraph::getDeclaringStage(DocumentSource* ds,
                                                                               PathRef path) const {
    return _impl->getDeclaringStage(ds, path);
}

void DependencyGraph::recompute(const DocumentSourceContainer& container,
                                boost::optional<DocumentSourceContainer::const_iterator> stageIt) {
    _impl->recompute(container, stageIt);
}

BSONObj DependencyGraph::toBSON() const {
    return _impl->toBSON();
}

std::string DependencyGraph::toDebugString() const {
    auto bson = toBSON();
    return tojson(bson, ExtendedRelaxedV2_0_0, true /*pretty*/);
}

class DependencyGraph::Impl::Serializer {
public:
    Serializer(const DependencyGraph::Impl& graph) : _graph(graph) {}

    BSONObj serializeToBson() {
        BSONObjBuilder stagesBob;
        for (StageId stageId{0}; stageId < _graph._stages.getNextId(); stageId.value++) {
            serializeStage(stageId, stagesBob);
        }
        return stagesBob.obj();
    }

private:
    /**
     * Reassigns sequential "ordered" IDs.
     */
    template <typename T>
    class OrderedIdMap {
    public:
        using Id = TypedId<T>;
        using OrderedId = TypedId<OrderedIdMap<T>>;

        OrderedId get(Id id) const {
            if (!id) {
                return OrderedId::none();
            }
            auto [it, inserted] = _orderedIds.emplace(id, OrderedId{_nextFieldId});
            if (inserted) {
                ++_nextFieldId;
            }
            return it->second;
        }

    private:
        // 'getModifiedPaths()' reports renames in an non-deterministic order so we assign each
        // field a "normalized" ID after by sorting the fields within a scope.
        mutable Id::ValueType _nextFieldId{0};
        mutable absl::flat_hash_map<Id, OrderedId> _orderedIds;
    };

    using OrderedFieldId = OrderedIdMap<Field>::OrderedId;
    using OrderedFields = std::vector<std::pair<std::string, OrderedFieldId>>;

    std::vector<StringPool::Id> sortedStrings(std::vector<StringPool::Id> s) const {
        std::vector<StringPool::Id> results = std::move(s);
        std::sort(results.begin(), results.end(), [&](auto lhs, auto rhs) {
            return _graph._strings.get(lhs) < _graph._strings.get(rhs);
        });
        return results;
    }

    template <typename It>
    std::vector<StringPool::Id> getSortedStringKeys(It begin, It end) const {
        std::vector<StringPool::Id> result;
        for (; begin != end; ++begin) {
            result.push_back(begin->first);
        }
        return sortedStrings(result);
    }

    auto sortedFieldDeps(const FieldDependencies& fields) const {
        auto result = std::vector(fields.begin(), fields.end());
        std::sort(result.begin(), result.end(), [&](FieldId lhs, FieldId rhs) {
            return std::make_pair(resolveFieldName(lhs), _orderedFieldIds.get(lhs)) <
                std::make_pair(resolveFieldName(rhs), _orderedFieldIds.get(rhs));
        });
        return result;
    }

    void serializeStage(StageId stageId, BSONObjBuilder& bob) {
        const auto& stage = _graph._stages[stageId];
        BSONObjBuilder stageBob = bob.subobjStart(formatStage(stageId));
        serializeScope(stage.scope, stageBob);
        serializeDependencies(stage.dependencies, stageBob);
    }

    void serializeScope(ScopeId scopeId, BSONObjBuilder& bob) {
        const auto& scope = _graph._scopes[scopeId];
        auto scopeName = formatScope(scopeId);
        if (_visitedScopes.count(scopeId)) {
            bob.append(scopeName, scopeName);
        } else {
            _visitedScopes.insert(scopeId);
            BSONObjBuilder scopeBob = bob.subobjStart(scopeName);
            scopeBob.append("exhaustiveScope", formatScope(scope.exhaustiveScope));
            {
                BSONObjBuilder fieldsBob = scopeBob.subobjStart("fields");
                {
                    BSONObjBuilder fieldObj = fieldsBob.subobjStart(fmt::format(
                        "<missing>:{}", _orderedFieldIds.get(scope.missingField).value));
                    serializeField(scope.missingField, fieldObj);
                }

                // FieldMap doesn't guarantee any order. We need a stable order for golden testing.
                for (auto&& name : getSortedStringKeys(scope.fields.begin(), scope.fields.end())) {
                    auto fieldId = scope.fields.at(name);
                    auto scopeFieldName = formatField(_graph._strings.get(name), fieldId);
                    if (fieldId < scope.missingField) {
                        // Field is inherited if the field ID is lower than the <missing> field of
                        // the scope.
                        fieldsBob.append(scopeFieldName, scopeFieldName);
                    } else {
                        BSONObjBuilder fieldObj = fieldsBob.subobjStart(scopeFieldName);
                        serializeField(fieldId, fieldObj);
                    }
                }
            }
        }
    }

    void serializeField(FieldId fieldId, BSONObjBuilder& bob) {
        const auto& field = _graph._fields[fieldId];
        auto fieldScopeName = formatScope(field.declaringScope);
        bob.append("declaringScope", fieldScopeName);
        if (field.embeddedScope) {
            serializeScope(field.embeddedScope, bob);
        }
        serializeDependencies(field.dependencies, bob);
    }

    void serializeDependencies(const FieldDependencies& deps, BSONObjBuilder& bob) {
        BSONArrayBuilder depsBuilder = bob.subarrayStart("dependencies");
        for (auto depFieldId : sortedFieldDeps(deps)) {
            depsBuilder.append(formatField(depFieldId));
        }
    }

    std::string formatStage(StageId stageId) const {
        return fmt::format(
            "{}:{}", _graph._stages[stageId].documentSource->getSourceName(), stageId.value);
    }

    std::string formatScope(ScopeId scopeId) const {
        return fmt::format("scope:{}", _orderedScopeIds.get(scopeId).value);
    }

    std::string formatField(PathRef name, FieldId fieldId) const {
        return fmt::format("{}:{}", name, _orderedFieldIds.get(fieldId).value);
    }

    std::string formatField(FieldId fieldId) const {
        return formatField(resolveFieldName(fieldId), fieldId);
    }

    std::string resolveFieldName(FieldId fieldId) const {
        const auto& field = _graph._fields[fieldId];
        // The field doesn't know the field name. We need to find the name from the declaring scope,
        // which contains a mapping from field name to field id.
        const auto& fieldMap = _graph._scopes[field.declaringScope].fields;
        auto fieldIt = std::find_if(fieldMap.begin(), fieldMap.end(), [fieldId](const auto& p) {
            return p.second == fieldId;
        });
        if (fieldIt == fieldMap.end() &&
            _graph._scopes[field.declaringScope].missingField == fieldId) {
            return "<missing>";
        }
        tassert(11937309, "Cannot find field in parent scope", fieldIt != fieldMap.end());
        return std::string{_graph._strings.get(fieldIt->first)};
    }

    const DependencyGraph::Impl& _graph;
    absl::flat_hash_set<ScopeId> _visitedScopes;
    OrderedIdMap<Field> _orderedFieldIds;
    OrderedIdMap<Scope> _orderedScopeIds;
};

BSONObj DependencyGraph::Impl::toBSON() const {
    Serializer serializer{*this};
    return serializer.serializeToBson();
}
}  // namespace mongo::pipeline::dependency_graph
