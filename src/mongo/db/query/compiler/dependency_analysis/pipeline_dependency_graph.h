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

#pragma once


#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::pipeline::dependency_graph {

namespace detail {
/**
 * Strongly typed alias for int to avoid mixing IDs of different types.
 */
template <typename T>
struct TypedId {
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

    int32_t value{-1};
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

    Id intern(StringData str);
    Id lookup(StringData str) const;

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

/**
 * Represents a DocumentSource that references or defines fields (or both).
 */
struct Stage {
    // TODO(SERVER-119840): Track field dependencies
    Stage(ScopeId scope,
          boost::intrusive_ptr<DocumentSource> documentSource,
          bool isSingleDocumentTransformation,
          ScopeId nextNewScope)
        : documentSource(std::move(documentSource)),
          scope(scope),
          nextNewScope(nextNewScope),
          isSingleDocumentTransformation(isSingleDocumentTransformation) {}

    boost::intrusive_ptr<DocumentSource> documentSource;
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

    // TODO(SERVER-119840): Track field dependencies
    Field(ScopeId declaringScope, ScopeId embeddedScope = ScopeId::none(), Metadata metadata = {})
        : metadata(std::move(metadata)),
          declaringScope(declaringScope),
          embeddedScope(embeddedScope) {}

    // This could also be stored as a node, and referenced here by its ID.
    Metadata metadata{};
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
        : _modPaths(documentSource.getModifiedPaths()),
          _isSingleDocumentTransformation(documentSource.getId() ==
                                          DocumentSourceSingleDocumentTransformation::id) {
        documentSource.getDependencies(&_deps);
    }

    bool modifiesAnyPaths() const {
        return !(_modPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet &&
                 _modPaths.paths.empty() && _modPaths.renames.empty() &&
                 _modPaths.complexRenames.empty());
    }

    DocumentSource::GetModPathsReturn::Type getModPathsType() const {
        return _modPaths.type;
    }

    const OrderedPathSet& getModifiedPaths() const {
        return _modPaths.paths;
    }

    const StringMap<std::string>& getRenames() const {
        return _modPaths.renames;
    }

    const StringMap<std::string>& getComplexRenames() const {
        return _modPaths.complexRenames;
    }

    const OrderedPathSet& getPathDependencies() const {
        return _deps.fields;
    }

    bool isSingleDocumentTransformation() const {
        return _isSingleDocumentTransformation;
    }

private:
    // Modified paths.
    DocumentSource::GetModPathsReturn _modPaths;
    // Dependencies.
    DepsTracker _deps;
    // Is the document source an instance of DocumentSourceSingleDocumentTransformation.
    bool _isSingleDocumentTransformation;
};
}  // namespace detail

using PathRef = StringData;

/**
 * Represents dependencies between fields and stages in a pipeline. Can be partially rebuilt when
 * the pipeline changes.
 */
class DependencyGraph {
public:
    DependencyGraph(const DocumentSourceContainer& container);

    /**
     * Return the stage which last modified the path visible from the given DocumentSource. The
     * stage must have either declared, modified or removed the path. If nullptr, the path is
     * unmodified and assumed to originate from the pipeline input.
     *
     * For example, the following stages all modify the path 'a'.
     * - {$set: {a: 1}}
     * - {$set: {a.b: 1}}
     * - {$project: {a: 0}}
     * - {$group: {_id: ...}}
     */
    boost::intrusive_ptr<mongo::DocumentSource> getDeclaringStage(DocumentSource* stage,
                                                                  PathRef path) const;

    /**
     * Invalidate and recompute the subgraph starting from the earliest nodes which correspond to
     * the stage pointed to by 'stageIt'.
     */
    void recomputeFromStage(DocumentSourceContainer::const_iterator stageIt,
                            const DocumentSourceContainer& container);

    /**
     * Generate a string which describes the graph.
     */
    std::string toDebugString() const;

    /**
     * Generate a BSONObj which describes the graph.
     */
    BSONObj toBSON() const;

    size_t numStages() const {
        return _stages.size();
    }

private:
    using StageId = detail::StageId;
    using ScopeId = detail::ScopeId;
    using FieldId = detail::FieldId;

    using Stage = detail::Stage;
    using Scope = detail::Scope;
    using Field = detail::Field;

    using ParsedPath = boost::container::small_vector<detail::StringPool::Id, 8>;
    using ParsedPathView = std::span<detail::StringPool::Id>;

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
    void declareScope(StageId stage, ScopeId exhaustiveScope, ScopeId parentScope);

    /**
     * Declare a field for the given (possibly dotted) path in the scope.
     * For a path like 'a.b' declares 'a' with embedded scope holding 'b'.
     * If 'a' already exists, any fields are preserved.
     * Returns the FieldId for the base component in path (for 'a.b' returns 'a').
     */
    FieldId declareField(ScopeId scope, ParsedPathView path);

    /**
     * Includes the field from the parent scope into the given scope.
     * This has the semantics of an inclusion projection.
     */
    void includeField(ScopeId scope, ScopeId parentScope, ParsedPathView path);

    /**
     * Gets the Field node that defines the given path in the given scope.
     */
    FieldLookupResult lookupField(ScopeId scopeId, ParsedPathView path) const;

    /**
     * Same as 'lookupField()', but returns FieldId::none() unless the full requested path is in
     * scope.
     */
    FieldId lookupFullPath(ScopeId scopeId, ParsedPathView path) const;

    /**
     * Gets the stage node that represents the given DocumentSource in the graph.
     */
    StageId getStageId(DocumentSource* stage) const;

    /**
     * Create graph nodes to represent the scope declared by the document source.
     */
    ScopeId processScope(const detail::DocumentSourceInfo& ds, StageId stage, ScopeId parentScope);

    /**
     * Creates graph nodes to represent the document source.
     */
    void processStage(boost::intrusive_ptr<DocumentSource> documentSource,
                      const detail::DocumentSourceInfo& dsInfo);

    /**
     * Clears the DocumentSource <-> StageId mapping and rebuilds it, stopping at 'end'. Returns a
     * StageId of the next stage after 'end'.
     */
    StageId clearAndRebuildMapping(const DocumentSourceContainer& container,
                                   DocumentSourceContainer::const_iterator end);

    /**
     * Find the lowest ID scope and field nodes corresponding to the given stage.
     * These IDs can be used to determine the valid portion of a graph.
     * If the range [container.begin(), stageIt) is valid, then so are
     * [0, minId) for all node types.
     */
    std::tuple<ScopeId, FieldId> earliestDescendants(StageId) const;

    /**
     * Invalidate the portion of the graph corresponding to [stageIt, container.end()).
     */
    void invalidate(const DocumentSourceContainer& container,
                    DocumentSourceContainer::const_iterator stageIt);

    /**
     * Interns the path components and returns a list of string IDs.
     */
    ParsedPath internPath(PathRef path);

    /**
     * Maps the path components to the interned strings.
     * Unknown components are replaced with NoStringId.
     */
    ParsedPath parsePath(PathRef path) const;

    // Stages map 1:1 to DocumentSources and are stored in the same order. If DocumentSource at
    // position 5 is modified, we invalidate stages from that index onwards.
    detail::NodeContainer<Stage> _stages;

    // Scopes are referenced by stages. *Some* stages can declare a scope. Others inherit the scope
    // from the previous stage. Scopes are also appended at the end of the container. This means
    // that if stage N is erased and it declares a scope, we can erase all scopes past the scope
    // declared by the stage.
    detail::NodeContainer<Scope> _scopes;

    // Fields referenced by scopes and are added in declaration order. That is, stages append fields
    // to the end. Each stage defines a "missing" field to start. If a scope is removed, we can
    // erase everything past the missing field for that scope.
    detail::NodeContainer<Field> _fields;

    // String interning pool. Each entry is a path component.
    detail::StringPool _strings;

    // Mapping between DocumentSource and StageId, recomputed when the graph is recomputed.
    absl::flat_hash_map<const DocumentSource*, StageId> _dsToStageId;
};

}  // namespace mongo::pipeline::dependency_graph
