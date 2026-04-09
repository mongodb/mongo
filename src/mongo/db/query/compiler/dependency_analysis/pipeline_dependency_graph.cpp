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
        if constexpr (kDebugBuild) {
            invariant(id, str::stream() << "Invalid access, container size " << size());
        }
        return _nodes.at(id.value);
    }

    const T& operator[](Id id) const {
        if constexpr (kDebugBuild) {
            invariant(id, str::stream() << "Invalid access, container size " << size());
        }
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
class FieldDependencies {
public:
    static FieldDependencies wholeDocument() {
        return FieldDependencies(true);
    }

    /// Empty field dependencies.
    FieldDependencies() {}

    /// Initialize with known field dependencies.
    FieldDependencies(std::initializer_list<FieldId> fields) : _fields(fields) {}

    FieldDependencies(const FieldDependencies&) = default;
    FieldDependencies(FieldDependencies&&) = default;

    FieldDependencies& operator=(const FieldDependencies&) = default;
    FieldDependencies& operator=(FieldDependencies&&) = default;

    /**
     * Checks if the entire document is a dependency (including all fields).
     */
    bool dependsOnWholeDocument() const {
        return _dependsOnWholeDocument;
    }

    auto begin() const {
        tassert(12194201,
                "Called begin() when dependsOnWholeDocument() is true",
                !_dependsOnWholeDocument);
        return _fields.begin();
    }

    auto end() const {
        tassert(12194202,
                "Called end() when dependsOnWholeDocument() is true",
                !_dependsOnWholeDocument);
        return _fields.end();
    }

    /**
     * Adds a dependency on the specified field.
     * Does nothing if the field is already a dependency.
     */
    void insert(FieldId field) {
        if (_dependsOnWholeDocument) {
            // We depend on all fields in this case, including this one.
            return;
        }
        _fields.insert(field);
    }

private:
    /// Private constructor for creating dependency on the whole document.
    explicit FieldDependencies(bool dependsOnWholeDocument)
        : _dependsOnWholeDocument(dependsOnWholeDocument) {}

    absl::flat_hash_set<FieldId> _fields;
    bool _dependsOnWholeDocument{false};
};

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
    // Sentinel FieldId for any field made "missing" by this stage.
    // This is also the minimum FieldId declared by this stage.
    FieldId missingField;
};

/**
 * Holds arrayness information or a constant value (if known).
 *
 * FieldMetadata is stored within every Field node in the graph. Since Field nodes are pretty
 * numerous, we try to keep the size of this struct as small as possible.
 *
 * TODO(SERVER-119392): Implement constant propagation
 */
struct FieldMetadata {
    bool isDefault() const {
        return *this == FieldMetadata{};
    }

    bool operator==(const FieldMetadata&) const = default;

    /**
     * True if the value of this field can be type array.
     */
    bool canFieldBeArray : 1 {true};

    /**
     * True if the value of this field is known to be the BSON missing value. I.e., the field is
     * guaranteed to be absent.
     */
    bool knownToBeMissing : 1 {false};
};

// It's fine to change the assert and increase the size of the FieldMetadata, but there should be a
// good reason to do so, since the knock-on effect is large (many fields in a graph). If you managed
// to reduce it - thanks! :)
static_assert(sizeof(FieldMetadata) == 1, "FieldMetadata size has changed");

/**
 * Represents a definition of a single path component. Each redefinition of a field is represented
 * as a separate node.
 *
 * Since Field nodes are pretty numerous, we try to keep the size of this struct as small as
 * possible.
 */
struct Field {
    Field(ScopeId declaringScope,
          ScopeId embeddedScope = ScopeId::none(),
          FieldDependencies dependencies = {},
          FieldMetadata metadata = {})
        : dependencies(std::move(dependencies)),
          declaringScope(declaringScope),
          embeddedScope(embeddedScope),
          metadata(std::move(metadata)) {}

    // Note: Field order is dictated by type alignment as opposed to semantics, to reduce
    // padding and the overall size of the structure.

    FieldDependencies dependencies{};
    // Scope declaring this field.
    // NOTE: We might not need to track this, since we know the scope in lookupField.
    ScopeId declaringScope{ScopeId::none()};
    // Embedded scope containing nested fields (e.g. 'a.b').
    ScopeId embeddedScope{ScopeId::none()};
    // This could also be stored as a node, and referenced here by its ID.
    FieldMetadata metadata{};
};

// It's fine to change the assert and increase the size of a Field, but there should be a good
// reason to do so, since the knock-on effect is large (many fields in a graph). If you managed to
// reduce it - thanks! :)
// The Abseil flat_hash_set seems to change size under a sanitized build. Itis easier to ignore it
// and check the remaining overheads.
static_assert(sizeof(Field) - sizeof(FieldDependencies) == 16, "Field size has changed");

// Information extracted from DocumentSource to populate the graph.
class DocumentSourceInfo {
public:
    explicit DocumentSourceInfo(const DocumentSource& documentSource)
        : _documentSource(documentSource),
          _isSingleDocumentTransformation(typeid(documentSource) ==
                                          typeid(DocumentSourceSingleDocumentTransformation)) {
        if (documentSource.getDependencies(&_deps) == DepsTracker::State::NOT_SUPPORTED) {
            _deps.needWholeDocument = true;
        }
    }

    void describeTransformation(document_transformation::DocumentOperationVisitor& visitor) const {
        _documentSource.describeTransformation(visitor);
    }

    bool dependsOnWholeDocument() const {
        return _deps.needWholeDocument;
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

bool canExpressionEvaluateToArray(const Expression& expr) {
    if (auto* constantExpr = dynamic_cast<const ExpressionConstant*>(&expr)) {
        return constantExpr->getValue().isArray();
    }
    return true;
}

void updateMetadataFromExpression(FieldMetadata& metadata, const Expression& expr) {
    metadata.canFieldBeArray = canExpressionEvaluateToArray(expr);
}

void updateMetadataForMissingValue(FieldMetadata& metadata) {
    metadata.canFieldBeArray = false;
    metadata.knownToBeMissing = true;
}

/**
 * Extracts the remaining suffix from a dotted path string after skipping 'count' path components.
 */
StringData skipPathComponents(StringData path, size_t count) {
    size_t pos = 0;
    for (size_t i = 0; i < count && pos < path.size(); i++) {
        auto dot = path.find('.', pos);
        if (dot == std::string::npos) {
            return {};
        }
        pos = dot + 1;
    }
    return path.substr(pos);
}

/// Result type used when looking up a field.
enum class FieldMatchType : uint8_t {
    /**
     * We found the exact field node matching the requested path.
     * Example: lookup was for x.y.z and we found x.y.z.
     */
    kExact,
    /**
     * We found a field node with no embedded scope which shadowed the requested path.
     * Example: lookup was for x.y.z, but we found x.y and there is no embedded scope. We return x.y
     * and consider it as shadowing x.y.z
     */
    kShadowed,
    /**
     * We did not find a matching field node, but we know that it could originate at the scope
     * whose <missing> field we return. The <missing> field's metadata indicates whether the field
     * is truly absent (knownToBeMissing=true, e.g. after inclusion projection) or merely unknown
     * (knownToBeMissing=false, e.g. after $replaceRoot with an expression).
     */
    kMissing,
    /**
     * We did not find a matching field node, and *we are certain* that the result of the path
     * expression is unchanged by the pipeline, and is the same as when evaluated against the base
     * document.
     * Example: lookup was for x.y.z, and the scope inherits the base document fields
     * (non-exhaustive), but it does not provide an in-graph definition for 'x', and there is no
     * scope which could modify 'x' implicitly, therefore, it comes from the collection.
     * Note that this is the type that should be used for any match holding a base document field
     * reference, even if it would otherwise be an exact match due to an inclusion projection.
     * Example:
     * {$project: {x: 1}} and x comes from the base document, a lookup for x will return
     * kBaseDocument instead of kExact.
     */
    kBaseDocument,
};

/**
 * Result from field lookup.
 */
struct FieldMatch {
    static FieldMatch exact(FieldId fieldId) {
        tassert(12266801, "Invalid field ID in kExact match", fieldId);
        return {fieldId, FieldMatchType::kExact};
    }

    static FieldMatch shadowed(FieldId fieldId) {
        tassert(12266802, "Invalid field ID in kShadowed match", fieldId);
        return {fieldId, FieldMatchType::kShadowed};
    }

    static FieldMatch missing(FieldId fieldId) {
        tassert(12266803, "Invalid field ID in kMissing match", fieldId);
        return {fieldId, FieldMatchType::kMissing};
    }

    static FieldMatch baseDocument() {
        return {FieldId::none(), FieldMatchType::kBaseDocument};
    }

    FieldId fieldId;
    FieldMatchType type;
};

}  // namespace

bool defaultCanPathBeArray(StringData path) {
    return true;
}

class DependencyGraph::Impl {
public:
    explicit Impl(const DocumentSourceContainer& container,
                  CanPathBeArray canMainCollPathBeArray = defaultCanPathBeArray)
        : _canMainCollPathBeArray(std::move(canMainCollPathBeArray)) {
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

    bool canPathBeArray(DocumentSource* ds, PathRef path) const {
        auto stageId = getStageId(ds);
        auto scopeId = _stages[stageId].scope;
        auto parsedPath = parsePath(path);

        FieldList prefix;
        auto [fieldId, type] = lookupField(scopeId, parsedPath, &prefix);

        switch (type) {
            case FieldMatchType::kExact:
                return canPrefixContainArrays(prefix) || _fields[fieldId].metadata.canFieldBeArray;
            case FieldMatchType::kShadowed: {
                // Check if the shadowing field has an alias to a collection path. If so, resolve
                // the full path and query the PathArrayness API with it.
                if (auto* alias = getAlias(fieldId)) {
                    if (canPrefixContainArrays(prefix) ||
                        _fields[fieldId].metadata.canFieldBeArray) {
                        return true;
                    }
                    auto suffix = skipPathComponents(path, prefix.size() + 1);
                    return _canMainCollPathBeArray(buildDottedPath(*alias, suffix));
                }
                // TODO(SERVER-119392): If our field is shadowed by another, and we know the value
                // for that shadowing field, we can determine if the result can be array. For
                // example, if {$set: {a: 1}}, then a.b cannot be array (it is missing).
                return true;
            }
            case FieldMatchType::kMissing:
                // Check the <missing> field's metadata: if it's known to be BSON Missing, the
                // field is definitely absent and cannot be an array. Otherwise, it's unknown.
                return !_fields[fieldId].metadata.knownToBeMissing;
            case FieldMatchType::kBaseDocument:
                return _canMainCollPathBeArray(path);
        }

        MONGO_UNREACHABLE_TASSERT(12266805);
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

    BSONObj toBSON() const;

private:
    using ParsedPath = boost::container::small_vector<StringPool::Id, 8>;
    using ParsedPathView = std::span<StringPool::Id>;
    using FieldList = boost::container::small_vector<FieldId, 8>;

    class Serializer;


    /**
     * Declares a scope (or embedded scope), which is defined by the given state and
     * inherits fields from parentScope (if any). The scope missing field is also created.
     */
    void declareScope(StageId stage, ScopeId exhaustiveScope, ScopeId parentScope) {
        auto scopeId = _scopes.getNextId();
        auto missingField = _fields.append(Field{scopeId});
        _scopes.append(Scope{stage, exhaustiveScope, missingField});
        if (parentScope) {
            _scopes[scopeId].fields = _scopes[parentScope].fields;
        }
    }

    /**
     * Declare a field for the given (possibly dotted) path in the scope.
     * For a path like 'a.b' declares 'a' with embedded scope holding 'b'.
     * If 'a' already exists, any fields are preserved.
     * The 'dependencies' and 'metadata' are assigned to the declared field 'a.b'.
     * Returns the FieldId for the base component in path (for 'a.b' returns 'a').
     */
    FieldId declareField(ScopeId scope,
                         ParsedPathView path,
                         FieldDependencies dependencies,
                         FieldMetadata metadata = {},
                         ParsedPathView collectionPathPrefix = {}) {
        // Declaring 'a' should create field 'a' and exit.
        if (path.size() == 1) {
            auto field = _fields.append(
                Field{scope, ScopeId::none(), std::move(dependencies), std::move(metadata)});
            _scopes[scope].fields[path.front()] = field;
            return field;
        }

        // Declaring a.b should use the scope for 'a' or declare a new embedded scope, in which to
        // create the subpath 'b'.
        auto basePath = path.subspan(0, 1);
        auto subPath = path.subspan(1);

        // Check if we already have 'a' in the current scope that we are building. If we do, we will
        // preserve any fields it may already contain.
        auto [existingBaseField, existingBaseFieldType] = lookupField(scope, basePath);
        bool canReuseBaseField = [&] {
            switch (existingBaseFieldType) {
                case FieldMatchType::kExact:
                    return _fields[existingBaseField].declaringScope == scope &&
                        _fields[existingBaseField].embeddedScope;
                case FieldMatchType::kShadowed:
                case FieldMatchType::kMissing:
                case FieldMatchType::kBaseDocument:
                    return false;
            }
            MONGO_UNREACHABLE_TASSERT(12227301);
        }();

        // Scope for declaring 'b' field of 'a'.
        FieldId newBaseField;
        if (canReuseBaseField) {
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
                _fields[newBaseField].dependencies.insert(existingBaseField);
            }
            declareScope(_scopes[scope].stage, exhaustiveEmbeddedScope, parentEmbeddedScope);
            _scopes[scope].fields[basePath.front()] = newBaseField;
            populateBaseFieldMetadata(
                newBaseField, existingBaseField, collectionPathPrefix, basePath.front());
        }
        // Finally, declare the subPath in the embeddedScope we found or created.
        ParsedPath nestedPrefix(collectionPathPrefix.begin(), collectionPathPrefix.end());
        nestedPrefix.push_back(basePath.front());
        auto embeddedField = declareField(_fields[newBaseField].embeddedScope,
                                          subPath,
                                          std::move(dependencies),
                                          std::move(metadata),
                                          nestedPrefix);
        _fields[newBaseField].dependencies.insert(embeddedField);
        return newBaseField;
    }

    /**
     * Includes the field from the parent scope into the given scope.
     * This has the semantics of an inclusion projection.
     *
     * If given, 'defaultFieldId' tells us the assumed origin of the field in case there is no
     * parent scope:
     *  - A real FieldId if some prefix of the field is defined (e.g. if "a" is defined but we don't
     *    know if "a.b" is).
     *  - FieldId::none() if the field originates from the base collection.
     *  - parentScope.missingField if the field was made missing by an exhaustive stage.
     */
    void includeField(ScopeId scope,
                      ScopeId parentScope,
                      ParsedPathView path,
                      FieldId defaultFieldId = FieldId::none()) {
        // Gets the field which defines the last component in the given path in the parent scope, or
        // 'defaultFieldId' if there is no parent scope.
        auto resolvePathInParent = [this, parentScope, defaultFieldId](ParsedPathView path) {
            if (!parentScope) {
                return defaultFieldId;
            }

            auto [fieldId, type] = lookupField(parentScope, path);
            switch (type) {
                case FieldMatchType::kShadowed:
                    return FieldId::none();
                case FieldMatchType::kExact:
                case FieldMatchType::kMissing:
                case FieldMatchType::kBaseDocument:
                    return fieldId;
            }
            MONGO_UNREACHABLE_TASSERT(12266804);
        };

        // Including 'a' should reference field 'a' and exit.
        if (path.size() == 1) {
            // Note that all four possible cases are covered by resolvePathInParent():
            // 1) Included field is defined in parentScope.
            // 2) Included field is not defined in parentScope, but not all fields are known
            //    (return FieldId::none()).
            // 3) Included field is not defined in parentScope and all fields are known (return the
            //    "missing" field id of the last exhaustive stage).
            // 4) There is no parentScope to resolve paths in (return the given 'defaultFieldId').
            _scopes[scope].fields[path.front()] = resolvePathInParent(path);
            return;
        }

        // Including a.b:
        // - if 'a' already included in the current scope (maybe 'a.c' was included), call
        // includeField for 'b'.
        // - if 'a' is not included in the current scope, declare 'a', then include 'b' into it.
        auto basePath = path.subspan(0, 1);
        auto subPath = path.subspan(1);

        FieldId parentBaseField = resolvePathInParent(basePath);
        ScopeId parentEmbeddedScope =
            parentBaseField ? _fields[parentBaseField].embeddedScope : ScopeId::none();
        ScopeId parentDeclaringScope =
            parentBaseField ? _fields[parentBaseField].declaringScope : ScopeId::none();

        bool shadowedByParent = parentDeclaringScope && !parentEmbeddedScope &&
            parentBaseField != _scopes[parentDeclaringScope].missingField;

        // If the parent field is defined but has no embedded scope, it's a scalar or an unknown
        // value that shadows any subpath. That is, the subfield that is being included is either
        // not known or is known to not exist. We currently don't properly distinguish between the
        // two, so we declare the field to avoid incorrect dependency tracking.
        // TODO(SERVER-119392): Track whether or not the parent field is known to be a scalar.
        if (shadowedByParent) {
            declareField(scope, path, {parentBaseField});
            return;
        }

        auto [existingBaseField, existingBaseFieldType] = lookupField(scope, basePath);
        switch (existingBaseFieldType) {
            case FieldMatchType::kExact:
                // Due to the rule that we cannot have clashing paths, we know that we cannot have
                // just "include a" and then "include a.b".
                tassert(11937306, "Clashing paths", _fields[existingBaseField].embeddedScope);
                // Since we are calling includeField, we expect that we cannot already have 'a'
                // included trivially either.
                tassert(
                    11936307, "Already exists", _fields[existingBaseField].declaringScope == scope);
                // 'a' is already defined in the current scope.
                includeField(_fields[existingBaseField].embeddedScope,
                             parentEmbeddedScope,
                             subPath,
                             parentBaseField /*defaultFieldId*/);
                break;
            case FieldMatchType::kShadowed:
            case FieldMatchType::kMissing:
            case FieldMatchType::kBaseDocument:
                // 'a' is not included in the current scope, so we need to declare it then include
                // 'b'.
                FieldId newBaseField = declareField(scope, basePath, {parentBaseField});
                ScopeId newEmbeddedScope = _scopes.getNextId();
                declareScope(_scopes[scope].stage, newEmbeddedScope, ScopeId::none());
                _fields[newBaseField].embeddedScope = newEmbeddedScope;
                includeField(newEmbeddedScope, parentEmbeddedScope, subPath, parentBaseField);
                break;
        }
    }

    /**
     * Gets the Field node that defines the given path in the given scope.
     * The traversed base fields are appended to 'prefix' (if provided).
     */
    FieldMatch lookupField(ScopeId scopeId,
                           ParsedPathView path,
                           FieldList* prefix = nullptr) const {
        tassert(11937401, "Missing scopeId", scopeId);

        const auto& scope = _scopes[scopeId];
        auto fieldNameId = path.front();
        if (auto it = scope.fields.find(fieldNameId); it != scope.fields.end()) {
            auto fieldId = it->second;
            if (!fieldId) {
                // Not found.
                return FieldMatch::baseDocument();
            }
            if (path.size() == 1) {
                // This is the last component in the dotted path we're looking for.
                return FieldMatch::exact(fieldId);
            }
            if (!_fields[fieldId].embeddedScope) {
                // We found 'a', but it has no embedded scope and we are looking for 'a.b'.
                return FieldMatch::shadowed(fieldId);
            }
            if (prefix) {
                prefix->push_back(fieldId);
            }
            // We're resolving a dotted path and there are known subpaths.
            return lookupField(_fields[fieldId].embeddedScope, path.subspan(1), prefix);
        }

        if (scope.exhaustiveScope) {
            // The field is not explicitly known in the graph but we know the scope that could
            // have modified it. The <missing> field's metadata indicates whether the field is
            // truly absent (knownToBeMissing) or merely unknown.
            return FieldMatch::missing(_scopes[scope.exhaustiveScope].missingField);
        }

        // The field is coming from the document.
        return FieldMatch::baseDocument();
    }

    /**
     * Returns the collection path alias for the given field, or nullptr if none.
     */
    const ParsedPath* getAlias(FieldId fieldId) const {
        auto it = _aliases.find(fieldId);
        return it != _aliases.end() ? &it->second : nullptr;
    }

    /**
     * Populate metadata when a base field is redefined.
     */
    void populateBaseFieldMetadata(FieldId newBaseField,
                                   FieldId existingBaseField,
                                   ParsedPathView collectionPathPrefix,
                                   StringPool::Id fieldNameId) {
        if (existingBaseField) {
            _fields[newBaseField].metadata.canFieldBeArray =
                _fields[existingBaseField].metadata.canFieldBeArray;
        } else {
            // The included base field is a collection field. Query the PathArrayness API to
            // determine if the field can be an array, using the full collection path.
            ParsedPath fullCollectionPath(collectionPathPrefix.begin(), collectionPathPrefix.end());
            fullCollectionPath.push_back(fieldNameId);
            _fields[newBaseField].metadata.canFieldBeArray =
                _canMainCollPathBeArray(buildDottedPath(fullCollectionPath));
        }
    }

    /**
     * Returns true if any field in the list can contain arrays.
     */
    bool canPrefixContainArrays(const FieldList& prefix) const {
        for (auto& containingField : prefix) {
            if (_fields[containingField].metadata.canFieldBeArray) {
                return true;
            }
        }
        return false;
    }

    /**
     * Joins the given parsed path and the given suffix by '.'.
     */
    std::string buildDottedPath(const ParsedPath& path, StringData suffix = {}) const {
        str::stream ss;
        for (size_t i = 0; i < path.size(); i++) {
            if (i > 0) {
                ss << '.';
            }
            ss << _strings.get(path[i]);
        }
        if (!suffix.empty()) {
            if (!path.empty()) {
                ss << '.';
            }
            ss << suffix;
        }
        return ss;
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
                [&](const ReplaceRoot& op) {
                    // All paths might be modified. This scope does not inherit the parent scope
                    // fields. If 'isEmpty' is true (e.g. inclusion projection), any
                    // field not explicitly added is truly missing. Otherwise (e.g. $replaceRoot
                    // with expression), unknown fields may still exist.
                    declareScope(stage, scopeId /*exhaustiveScope*/, ScopeId::none());
                    if (op.isEmpty()) {
                        auto& missingField = _fields[_scopes[scopeId].missingField];
                        updateMetadataForMissingValue(missingField.metadata);
                    }
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
                    FieldDependencies deps{};
                    FieldMetadata metadata{};
                    if (p.isRemoved()) {
                        updateMetadataForMissingValue(metadata);
                    } else if (auto expr = p.getExpression()) {
                        updateMetadataFromExpression(metadata, *expr);
                        deps = processExpressionDependencies(*expr, parentScope);
                    } else {
                        // If the modification is not determined by an expression, we cannot get
                        // more precise dependency information. The stage dependencies will always
                        // be a superset of any modified path dependencies, so we can use those.
                        // Example: {$unwind: '$x'}
                        deps = depsFromStage;
                    }
                    declareField(scopeId, parsedPath, std::move(deps), std::move(metadata));
                },
                [&](const RenamePath& p) {
                    maybeDeclareInheritedScope();
                    auto parsedOldPath = internPath(p.getOldPath());
                    auto parsedNewPath = internPath(p.getNewPath());

                    FieldMetadata metadata;
                    FieldDependencies deps;
                    bool isBaseDocumentField = false;
                    ParsedPath aliasCollectionPath;

                    if (parentScope) {
                        // The found field could be either:
                        // - exact field from parent scope
                        // - a shadowing field
                        // - the <missing> field from an exhaustive scope
                        // Example 1: [{$replaceWith: {}}, {$set: {a: "$b"}}]
                        // 'a' depends on the $replaceWith stage's <missing> field.
                        // Example 2: [{$set: {'b': 1}}, {$set: {a: '$b.c'}}]
                        // 'a' depends on 'b'
                        FieldList prefix;
                        auto [oldPathField, oldPathFieldType] =
                            lookupField(parentScope, parsedOldPath, &prefix);

                        deps.insert(oldPathField);

                        switch (oldPathFieldType) {
                            case FieldMatchType::kExact: {
                                if (canPrefixContainArrays(prefix)) {
                                    break;
                                }
                                metadata.canFieldBeArray =
                                    _fields[oldPathField].metadata.canFieldBeArray;
                                // Track transitive alias. If the prefix contains arrays
                                // we skip this: canFieldBeArray is already true and any
                                // downstream alias resolution would also yield true.
                                if (auto* alias = getAlias(oldPathField)) {
                                    aliasCollectionPath = *alias;
                                }
                                break;
                            }
                            case FieldMatchType::kMissing: {
                                // A truly missing field cannot be an array. An unknown field
                                // might be.
                                metadata.canFieldBeArray =
                                    !_fields[oldPathField].metadata.knownToBeMissing;
                                break;
                            }
                            case FieldMatchType::kShadowed: {
                                // The old path is shadowed: e.g., b.z where b has no
                                // embedded scope. Check if the shadowing field has an alias
                                // so we can resolve through it.
                                if (auto* alias = getAlias(oldPathField)) {
                                    aliasCollectionPath = *alias;
                                    size_t consumed = prefix.size() + 1;
                                    for (size_t i = consumed; i < parsedOldPath.size(); ++i) {
                                        aliasCollectionPath.push_back(parsedOldPath[i]);
                                    }
                                    if (!canPrefixContainArrays(prefix) &&
                                        !_fields[oldPathField].metadata.canFieldBeArray) {
                                        metadata.canFieldBeArray = _canMainCollPathBeArray(
                                            buildDottedPath(aliasCollectionPath));
                                    }
                                }
                                break;
                            }
                            case FieldMatchType::kBaseDocument: {
                                isBaseDocumentField = true;
                                break;
                            }
                        }
                    } else {
                        isBaseDocumentField = true;
                        deps.insert(FieldId::none());
                    }

                    if (isBaseDocumentField) {
                        // We represent all collection field references as FieldId::none(), without
                        // distinguishing between them.
                        metadata.canFieldBeArray = _canMainCollPathBeArray(p.getOldPath());
                        aliasCollectionPath.assign(parsedOldPath.begin(), parsedOldPath.end());
                    }

                    // Each rename modifies the new field and depends on the previous field.
                    declareField(scopeId, parsedNewPath, std::move(deps), std::move(metadata));

                    // Store alias for the leaf field of the new path.
                    if (!aliasCollectionPath.empty()) {
                        auto [leafFieldId, _] = lookupField(scopeId, parsedNewPath);
                        tassert(12193201, "Missing leafFieldId", leafFieldId);
                        _aliases[leafFieldId] = std::move(aliasCollectionPath);
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
        _dsToStageId[documentSource.get()] = stageId;

        auto parentScopeId = _stages.empty() ? ScopeId::none() : _stages.back().scope;
        FieldDependencies dependencies = processStageDependencies(dsInfo, parentScopeId);

        const auto nextNewScopeId = _scopes.getNextId();
        auto scopeId = processScope(dsInfo, stageId, parentScopeId, dependencies);

        _stages.append(Stage{scopeId,
                             std::move(documentSource),
                             std::move(dependencies),
                             dsInfo.isSingleDocumentTransformation(),
                             nextNewScopeId});
    }

    /**
     * Creates dependencies for a stage.
     */
    FieldDependencies processStageDependencies(const DocumentSourceInfo& dsInfo,
                                               ScopeId parentScope) {
        return processPathDependencies(
            dsInfo.getPathDependencies(), dsInfo.dependsOnWholeDocument(), parentScope);
    }

    /**
     * Creates dependencies for an expression.
     */
    FieldDependencies processExpressionDependencies(const Expression& expr, ScopeId parentScope) {
        DepsTracker depsTracker = expression::getDependencies(&expr);
        return processPathDependencies(
            depsTracker.fields, depsTracker.needWholeDocument, parentScope);
    }

    /**
     * Creates dependencies from a set of paths.
     */
    FieldDependencies processPathDependencies(const OrderedPathSet& paths,
                                              bool dependsOnWholeDocument,
                                              ScopeId parentScope) {
        if (dependsOnWholeDocument) {
            return FieldDependencies::wholeDocument();
        }
        if (paths.empty()) {
            return FieldDependencies{};
        }
        if (parentScope) {
            FieldDependencies dependencies;
            for (auto&& path : paths) {
                auto parsedPath = internPath(path);
                auto fieldId = lookupField(parentScope, parsedPath).fieldId;
                dependencies.insert(fieldId);
            }
            return dependencies;
        }
        // Any dependency in the first stage is always a collection field.
        return FieldDependencies{FieldId::none()};
    }

    /**
     * Find the lowest ID stage, scope and field nodes corresponding to the given stage. These IDs
     * can be used to determine the valid portion of a graph. If the range [container.begin(),
     * stageIt) is valid, then so are [0, minId) for all node types.
     */
    std::tuple<StageId, ScopeId, FieldId> earliestDescendants(
        const DocumentSourceContainer& container,
        DocumentSourceContainer::const_iterator stageIt) const {
        // Compute the StageId of the first stage to invalidate. Stages before stageIt are
        // unchanged, so their map entries and StageIds remain valid.
        if (stageIt == container.begin()) {
            return {StageId{0}, ScopeId{0}, FieldId{0}};
        }

        StageId stageId = {getStageId(std::prev(stageIt)->get()).value + 1};
        ScopeId scopeId = _stages[stageId].nextNewScope;
        FieldId fieldId = scopeId == _scopes.getNextId()
            // No scope to invalidate so no fields to invalidate.
            ? _fields.getNextId()
            // Invalidate every field declared by or after the scope.
            : _scopes[scopeId].missingField;

        return {stageId, scopeId, fieldId};
    }

    /**
     * Invalidate the portion of the graph corresponding to [stageIt, container.end()).
     */
    void invalidate(const DocumentSourceContainer& container,
                    DocumentSourceContainer::const_iterator stageIt) {
        // Invalidate all nodes originating from the given stage.
        auto [invalidStage, invalidScope, invalidField] = earliestDescendants(container, stageIt);

        // Clean up aliases for invalidated fields.
        absl::erase_if(_aliases,
                       [invalidField](const auto& entry) { return entry.first >= invalidField; });

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

    // Mapping between DocumentSource and StageId. May contain dangling entries for DocumentSources
    // that have been removed from the pipeline. This is safe because this map is never iterated and
    // is only ever queried with valid DocumentSource pointers.
    absl::flat_hash_map<const DocumentSource*, StageId> _dsToStageId;

    // Maps a FieldId to the collection path it aliases (as interned StringPool IDs).
    // Only populated for fields created via RenamePath that reference collection fields
    // (directly or transitively through other aliases).
    absl::flat_hash_map<FieldId, ParsedPath> _aliases;

    // Callback to query the Path Arrayness API for the main collection.
    const CanPathBeArray _canMainCollPathBeArray;
};

DependencyGraph::DependencyGraph(const DocumentSourceContainer& container,
                                 CanPathBeArray canMainCollPathBeArray)
    : _impl(std::make_unique<Impl>(container, std::move(canMainCollPathBeArray))) {}

DependencyGraph::~DependencyGraph() = default;
DependencyGraph::DependencyGraph(DependencyGraph&&) noexcept = default;
DependencyGraph& DependencyGraph::operator=(DependencyGraph&&) noexcept = default;

boost::intrusive_ptr<mongo::DocumentSource> DependencyGraph::getDeclaringStage(DocumentSource* ds,
                                                                               PathRef path) const {
    return _impl->getDeclaringStage(ds, path);
}

bool DependencyGraph::canPathBeArray(DocumentSource* ds, PathRef path) const {
    return _impl->canPathBeArray(ds, path);
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
            BSONObjBuilder fieldsBob = scopeBob.subobjStart("fields");
            {
                BSONObjBuilder fieldObj = fieldsBob.subobjStart(
                    fmt::format("<missing>:{}", _orderedFieldIds.get(scope.missingField).value));
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

    void serializeField(FieldId fieldId, BSONObjBuilder& bob) {
        if (!fieldId) {
            bob.append("source", "<base document>");
            return;
        }
        const auto& field = _graph._fields[fieldId];
        auto fieldScopeName = formatScope(field.declaringScope);
        bob.append("declaringScope", fieldScopeName);
        if (field.embeddedScope) {
            serializeScope(field.embeddedScope, bob);
        }
        if (!field.metadata.isDefault()) {
            serializeMetadata(field.metadata, bob);
        }
        if (auto* alias = _graph.getAlias(fieldId)) {
            bob.append("collectionAlias", _graph.buildDottedPath(*alias));
        }
        serializeDependencies(field.dependencies, bob);
    }

    void serializeMetadata(const FieldMetadata& metadata, BSONObjBuilder& bob) {
        BSONObjBuilder metaBuilder = bob.subobjStart("metadata");
        if (!metadata.canFieldBeArray) {
            metaBuilder.append("array", false);
        }
        if (metadata.knownToBeMissing) {
            metaBuilder.append("missing", true);
        }
    }

    void serializeDependencies(const FieldDependencies& deps, BSONObjBuilder& bob) {
        if (deps.dependsOnWholeDocument()) {
            bob.append("dependencies", "<all>");
            return;
        }
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
        if (!fieldId) {
            return "<base document>";
        }
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

DependencyGraphContext::DependencyGraphContext(ExpressionContext& expCtx,
                                               DocumentSourceContainer& container)
    : _expCtx(expCtx), _container(container) {}

const DependencyGraph& DependencyGraphContext::getGraph(
    boost::optional<DocumentSourceContainer::const_iterator> maxStageIt) const {
    CanPathBeArray canPathBeArray = [this](StringData path) -> bool {
        const auto& pathArrayness = _expCtx.getMainCollPathArrayness();
        return pathArrayness.canPathBeArray(FieldRef(path), &_expCtx);
    };
    _graph.emplace(_container, std::move(canPathBeArray));
    return *_graph;
}

}  // namespace mongo::pipeline::dependency_graph
