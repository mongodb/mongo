// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo {
/**
 * This class represents a path which can be taken through a specific document. In addition to the
 * path components, which are stored as a FieldRef, RuntimeUpdatePath also stores the "type" of
 * each component, which can be either kFieldName or kArrayIndex. Non-numeric components are always
 * of type kFieldName. Numeric components may either be field names (in the case of an object like
 * {"123": "foo"}) or array indexes.
 *
 * Unlike other path representations in our codebase, RuntimeUpdatePath is tied to a particular
 * document. It does not represent a path that a user provides as part of an update.
 *
 * Example:
 * Document: {a: {0: {b: ["foo", "bar"]}}}.
 *
 * The path to "bar" would be represented as "a.0.b.1" where "a" "0" and "b" are of type kFieldName
 * while "1" is of type kArrayIndex.
 */
class RuntimeUpdatePath {
public:
    enum ComponentType { kFieldName, kArrayIndex };
    using ComponentTypeVector = std::vector<ComponentType>;

    RuntimeUpdatePath() = default;
    RuntimeUpdatePath(FieldRef fr, ComponentTypeVector types)
        : _fieldRef(std::move(fr)), _types(std::move(types)) {
        validate();
    }

    // We don't allow move construction/assignment since we don't want to worry about a moved-from
    // RuntimeUpdatePath passing validate().
    RuntimeUpdatePath(RuntimeUpdatePath&&) = delete;
    RuntimeUpdatePath& operator=(RuntimeUpdatePath&&) = delete;

    RuntimeUpdatePath(const RuntimeUpdatePath&) = default;
    RuntimeUpdatePath& operator=(const RuntimeUpdatePath&) = default;

    /**
     * Returns the underlying FieldRef.
     */
    const FieldRef& fieldRef() const {
        validate();
        return _fieldRef;
    }

    /**
     * Returns the type array, which is guaranteed to be the same length as the backing FieldRef.
     */
    const ComponentTypeVector& types() const {
        validate();
        return _types;
    }

    bool empty() const {
        validate();
        return _fieldRef.empty();
    }

    size_t size() const {
        validate();
        return _fieldRef.numParts();
    }

    void append(std::string_view fieldName, ComponentType type) {
        validate();
        _fieldRef.appendPart(fieldName);
        _types.push_back(type);
        // AF-419 fails in fieldRef() with unmatched _fieldRef and _types size, so we'd like to
        // validate to twice here to find where unmatching happens.
        validate();
    }

    void popBack() {
        validate();
        invariant(_fieldRef.numParts() > 0);
        _fieldRef.removeLastPart();
        _types.pop_back();
        // AF-419 fails in fieldRef() with unmatched _fieldRef and _types size, so we'd like to
        // validate to twice here to find where unmatching happens.
        validate();
    }

private:
    // Returns whether the RuntimeUpdatePath is in a valid state.
    bool good() const {
        return _fieldRef.numParts() == _types.size();
    }

    void reportError() const;

    // If RuntimeUpdatePath is not in a valid state, report the error with detail info in the log
    // without exposing PPID.
    void validate() const {
        if (MONGO_unlikely(!good())) {
            reportError();
        }
    }

    // There is an invariant that the number of components in field ref is equal to the number of
    // types in _types.
    FieldRef _fieldRef;
    ComponentTypeVector _types;
};

/**
 * RAII class for temporarily appending to a RuntimeUpdatePath.
 */
class RuntimeUpdatePathTempAppend {
public:
    RuntimeUpdatePathTempAppend(RuntimeUpdatePath& typedFieldRef,
                                std::string_view part,
                                RuntimeUpdatePath::ComponentType type)
        : _typedFieldRef(typedFieldRef) {
        _typedFieldRef.append(part, type);
    }

    ~RuntimeUpdatePathTempAppend() {
        _typedFieldRef.popBack();
    }

private:
    RuntimeUpdatePath& _typedFieldRef;
};
}  // namespace mongo
