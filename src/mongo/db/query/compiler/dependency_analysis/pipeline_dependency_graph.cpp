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
#include "mongo/db/pipeline/pipeline.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pipeline::dependency_graph {
namespace detail {
StringPool::Id StringPool::intern(StringData str) {
    auto it = _stringToId.find(str);
    if (it != _stringToId.end()) {
        return it->second;
    }
    Id id(_strings.size());
    _strings.emplace_back(str);
    _stringToId.emplace_hint(it, _strings.back(), id);
    return id;
}

StringPool::Id StringPool::lookup(StringData str) const {
    if (auto it = _stringToId.find(str); it != _stringToId.end()) {
        return it->second;
    }
    return Id::none();
}
}  // namespace detail

DependencyGraph::DependencyGraph(const DocumentSourceContainer& container) {
    recomputeFromStage(container.begin(), container);
}

boost::intrusive_ptr<mongo::DocumentSource> DependencyGraph::getDeclaringStage(DocumentSource* ds,
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

DependencyGraph::StageId DependencyGraph::getStageId(DocumentSource* ds) const {
    if (!ds) {
        return _stages.getLastId();
    }
    if (auto it = _dsToStageId.find(ds); it != _dsToStageId.end()) {
        return it->second;
    }
    tasserted(11937307, "Unknown DocumentSource");
}

DependencyGraph::StageId DependencyGraph::clearAndRebuildMapping(
    const DocumentSourceContainer& container, DocumentSourceContainer::const_iterator stageIt) {
    _dsToStageId.clear();
    // Rebuild mapping from begin to stageIt.
    StageId index{0};
    for (auto it = container.begin(); it != stageIt; it++) {
        _dsToStageId.emplace(it->get(), index);
        ++index.value;
    }
    return index;
}

DependencyGraph::FieldId DependencyGraph::lookupFullPath(ScopeId scopeId,
                                                         ParsedPathView path) const {
    auto [fieldId, shadowed] = lookupField(scopeId, path);
    return shadowed ? FieldId::none() : fieldId;
}

DependencyGraph::FieldLookupResult DependencyGraph::lookupField(ScopeId scopeId,
                                                                ParsedPathView path) const {
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

void DependencyGraph::recomputeFromStage(DocumentSourceContainer::const_iterator stageIt,
                                         const DocumentSourceContainer& container) {
    invalidate(container, stageIt);
    for (auto it = stageIt; it != container.end(); it++) {
        detail::DocumentSourceInfo dsInfo{**it};
        processStage(*it, dsInfo);
    }
}

std::tuple<DependencyGraph::ScopeId, DependencyGraph::FieldId> DependencyGraph::earliestDescendants(
    StageId stageId) const {
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

void DependencyGraph::invalidate(const DocumentSourceContainer& container,
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

void DependencyGraph::processStage(boost::intrusive_ptr<DocumentSource> documentSource,
                                   const detail::DocumentSourceInfo& dsInfo) {
    StageId stageId = _stages.getNextId();
    _dsToStageId.emplace(documentSource.get(), stageId);

    auto parentScopeId = _stages.empty() ? ScopeId::none() : _stages.back().scope;

    const auto nextNewScopeId = _scopes.getNextId();
    auto scopeId = processScope(dsInfo, stageId, parentScopeId);

    _stages.append(Stage{scopeId,
                         std::move(documentSource),
                         dsInfo.isSingleDocumentTransformation(),
                         nextNewScopeId});
}

DependencyGraph::ScopeId DependencyGraph::processScope(const detail::DocumentSourceInfo& ds,
                                                       StageId stage,
                                                       ScopeId parentScope) {
    const ScopeId scopeId = _scopes.getNextId();

    bool preservePaths = false;
    switch (ds.getModPathsType()) {
        using Type = DocumentSource::GetModPathsReturn::Type;
        // TODO(SERVER-119374): Revisit how to handle kNotSupported and kAllPaths.
        case Type::kNotSupported:
        case Type::kAllPaths: {
            // All paths might be modified. This scope does not inherit the parent scope fields.
            // It also doesn't know all fields - all lookups will fail and we should say we have no
            // information about the field (as opposed to saying the field is definitely missing
            // like for kAllExcept).
            declareScope(stage, scopeId /*exhaustiveScope*/, ScopeId::none());
            break;
        }
        case Type::kFiniteSet: {
            // A specific set of paths are modified, which means lookups should find them in this
            // scope. This scope inherits all fields from the parent scope too.
            if (parentScope && !ds.modifiesAnyPaths()) {
                return parentScope;
            }
            ScopeId exhaustiveScope =
                parentScope ? _scopes[parentScope].exhaustiveScope : ScopeId::none();
            // Parent scope names are valid.
            declareScope(stage, exhaustiveScope, parentScope);
            break;
        }
        case Type::kAllExcept: {
            // Parent scope names other than the ones in paths are not valid.
            // TODO(SERVER-119374): Handle this properly.
            declareScope(stage, _scopes.getNextId(), ScopeId::none());
            preservePaths = true;
            break;
        }
    }

    for (auto&& path : ds.getModifiedPaths()) {
        auto parsedPath = internPath(path);
        if (preservePaths) {
            includeField(scopeId, parentScope, parsedPath);
        } else {
            declareField(scopeId, parsedPath);
        }
    }

    if (parentScope) {
        for (auto&& [newPath, oldPath] : ds.getRenames()) {
            auto parsedOldPath = internPath(oldPath);
            auto parsedNewPath = internPath(newPath);
            declareField(scopeId, parsedNewPath);
        }
        tassert(11937305, "Unimplemented complex renames", ds.getComplexRenames().empty());
    } else {
        // Assume no renames or complex renames.
        tassert(11937303, "Unimplemented renames when no parent scope", ds.getRenames().empty());
        tassert(11937304,
                "Unimplemented complex renames when no parent scope",
                ds.getComplexRenames().empty());
    }

    // Scope with scopeId was added.
    return scopeId;
}

void DependencyGraph::declareScope(StageId stage, ScopeId exhaustiveScope, ScopeId parentScope) {
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

void DependencyGraph::includeField(ScopeId scope, ScopeId parentScope, ParsedPathView path) {
    // Including 'a' should reference field 'a' and exit.
    if (path.size() == 1) {
        auto existingField = lookupFullPath(parentScope, path);
        if (existingField && existingField != _scopes[parentScope].missingField) {
            _scopes[scope].fields[path.front()] = existingField;
            return;
        }
        declareField(scope, path);
        return;
    }

    // Including a.b:
    // - if 'a' already included in the current scope (maybe 'a.c' was included), call includeField
    // for 'b'.
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
        declareField(_fields[existingBaseField].embeddedScope, subPath);
        return;
    }

    // 'a' is not included in the current scope, so we need to declare it then include 'b'.
    auto newBaseField = declareField(scope, basePath);
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
    declareField(scope, path);
}

DependencyGraph::FieldId DependencyGraph::declareField(ScopeId scope, ParsedPathView path) {
    // Declaring 'a' should create field 'a' and exit.
    if (path.size() == 1) {
        auto field = _fields.append(Field{scope, ScopeId::none()});
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
        // FieldId::none(), it comes from the document. If baseField == <missing>, it is modified by
        // the scope definining it.
        auto exhaustiveEmbeddedScope = existingBaseField
            ? _scopes[_fields[existingBaseField].declaringScope].exhaustiveScope
            : ScopeId::none();
        declareScope(_scopes[scope].stage, exhaustiveEmbeddedScope, parentEmbeddedScope);
        _scopes[scope].fields[basePath.front()] = newBaseField;
    }
    // Finally, declare the subPath in the embeddedScope we found or created.
    (void)declareField(_fields[newBaseField].embeddedScope, subPath);
    return newBaseField;
}

DependencyGraph::ParsedPath DependencyGraph::internPath(PathRef path) {
    ParsedPath vec;
    for (auto s : std::views::split(path, '.')) {
        vec.push_back(_strings.intern({s.begin(), s.end()}));
    }
    return vec;
}

DependencyGraph::ParsedPath DependencyGraph::parsePath(PathRef path) const {
    ParsedPath vec;
    for (auto s : std::views::split(path, '.')) {
        auto id = _strings.lookup({s.begin(), s.end()});
        vec.push_back(id);
    }
    return vec;
}

std::string DependencyGraph::toDebugString() const {
    auto bson = toBSON();
    return tojson(bson, ExtendedRelaxedV2_0_0, true /*pretty*/);
}

class DependencyGraph::Serializer {
public:
    Serializer(const DependencyGraph& graph) : _graph(graph) {}

    BSONObj serializeToBson() {
        BSONObjBuilder stagesBob;
        for (StageId stageId{0}; stageId < _graph._stages.getNextId(); stageId.value++) {
            const auto& stage = _graph._stages[stageId];
            BSONObjBuilder stageBob = stagesBob.subobjStart(formatStage(stageId));
            serializeScope(stage.scope, stageBob);
        }
        return stagesBob.obj();
    }

private:
    using OrderedFieldId = int32_t;
    using OrderedFields = std::vector<std::pair<detail::StringPool::Id, FieldId>>;

    class OrderedFieldIdMap {
    public:
        OrderedFieldId get(FieldId fieldId) const {
            auto [it, inserted] = _orderedFieldIds.emplace(fieldId, _nextOrderedFieldId);
            if (inserted) {
                ++_nextOrderedFieldId;
            }
            return it->second;
        }

    private:
        // 'getModifiedPaths()' reports renames in an non-deterministic order so we assign each
        // field a "normalized" ID after by sorting the fields within a scope.
        mutable OrderedFieldId _nextOrderedFieldId{0};
        mutable absl::flat_hash_map<FieldId, OrderedFieldId> _orderedFieldIds;
    };

    static OrderedFields sortedFields(const detail::FieldMap& fields) {
        OrderedFields result(fields.begin(), fields.end());
        std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return a.first < b.first; });
        return result;
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
                    BSONObjBuilder fieldObj = fieldsBob.subobjStart(
                        fmt::format("<missing>:{}", _orderedFieldIds.get(scope.missingField)));
                    serializeField(scope.missingField, fieldObj);
                }

                // FieldMap doesn't guarantee any order. We need a stable order for golden testing.
                for (const auto& [name, fieldId] : sortedFields(scope.fields)) {
                    auto scopeFieldName = formatField(_graph._strings.get(name), fieldId);
                    BSONObjBuilder fieldObj = fieldsBob.subobjStart(scopeFieldName);
                    serializeField(fieldId, fieldObj);
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
    }

    std::string formatStage(StageId stageId) const {
        return fmt::format(
            "{}:{}", _graph._stages[stageId].documentSource->getSourceName(), stageId.value);
    }

    std::string formatScope(ScopeId scopeId) const {
        return fmt::format("scope:{}", scopeId.value);
    }

    std::string formatField(PathRef name, FieldId fieldId) const {
        return fmt::format("{}:{}", name, _orderedFieldIds.get(fieldId));
    }

    std::string formatField(FieldId fieldId) const {
        const auto& field = _graph._fields[fieldId];
        const auto& fieldMap = _graph._scopes[field.declaringScope].fields;
        auto fieldIt = std::find_if(fieldMap.begin(), fieldMap.end(), [fieldId](const auto& p) {
            return p.second == fieldId;
        });
        if (fieldIt == fieldMap.end() &&
            _graph._scopes[field.declaringScope].missingField == fieldId) {
            return fmt::format("<missing>:{}", _orderedFieldIds.get(fieldId));
        }
        tassert(11937309, "Cannot find field in parent scope", fieldIt != fieldMap.end());
        return formatField(_graph._strings.get(fieldIt->first), fieldIt->second);
    }


    const DependencyGraph& _graph;
    absl::flat_hash_set<ScopeId> _visitedScopes;
    OrderedFieldIdMap _orderedFieldIds;
};

BSONObj DependencyGraph::toBSON() const {
    Serializer serializer{*this};
    return serializer.serializeToBson();
}
}  // namespace mongo::pipeline::dependency_graph
