/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/field_ref.h"

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
        invariant(good());
    }

    /**
     * Returns the underlying FieldRef.
     */
    const FieldRef& fieldRef() const {
        invariant(good());
        return _fieldRef;
    }

    /**
     * Returns the type array, which is guaranteed to be the same length as the backing FieldRef.
     */
    const ComponentTypeVector& types() const {
        invariant(good());
        return _types;
    }

    bool empty() const {
        invariant(good());
        return _fieldRef.empty();
    }

    size_t size() const {
        invariant(good());
        return _fieldRef.numParts();
    }

    void append(StringData fieldName, ComponentType type) {
        invariant(good());
        _fieldRef.appendPart(fieldName);
        _types.push_back(type);
    }

    void popBack() {
        invariant(good());
        invariant(_fieldRef.numParts() > 0);
        _fieldRef.removeLastPart();
        _types.pop_back();
    }

private:
    // Returns whether the RuntimeUpdatePath is in a valid state.
    bool good() const {
        return _fieldRef.numParts() == _types.size();
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
                                StringData part,
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
