// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/db/query/compiler/type_system/type.h"

namespace mongo {

class MatchExpression;
class CollectionQueryInfo;

/**
 * Represents the type and arrayness of each field of a collection's documents, as inferred from
 * the collection's document validator.
 */
class SchemaTypeInfo {
public:
    explicit SchemaTypeInfo() : _rootType(pipeline::type_system::Type::anyObject()) {}

    SchemaTypeInfo(pipeline::type_system::Type rootType) : _rootType(std::move(rootType)) {}

    /**
     * Empty schema metadata representing the conservative case where nothing is known about the
     * shape of the collection's documents. This may be encountered when there is no validator or
     * no Collection acquisition is possible.
     */
    static SchemaTypeInfo empty() {
        return SchemaTypeInfo();
    }

    /**
     * Populates '_rootType' by narrowing the "any object" type using 'validator', the parsed
     * document validator for a collection. Leaves '_rootType' as "any object" if 'validator' is
     * null.
     */
    void populateFromValidator(const MatchExpression* validator);

    /**
     * Returns the inferred type of the collection's documents.
     */
    const pipeline::type_system::Type& getRootType() const {
        return _rootType;
    }

    /**
     * Returns whether any component of 'path' could be an array, according to '_rootType'.
     * A path whose traversal is not modelled by '_rootType' is conservatively treated as an array.
     */
    bool canPathBeArray(const FieldRef& path) const;

    /**
     * Returns the inferred type of the value at 'path'. A path that traverses a possible array is
     * not modelled and yields 'any'; a path through a non-object yields 'missing'.
     */
    pipeline::type_system::Type getPathType(const FieldRef& path) const;

private:
    friend struct CollectionSchemaChecker;
    friend class CollectionQueryInfo;

    // Inferred type of the collection's root document.
    pipeline::type_system::Type _rootType;
};

/**
 * Tracks schema metadata version for a collection and kills the plan if that metadata changes while
 * the plan is yielded.
 */
struct CollectionSchemaChecker {
public:
    CollectionSchemaChecker() = default;

    void uassertIfInvalidatedAndSyncSchema(const SchemaTypeInfo& current);

private:
    boost::optional<pipeline::type_system::Type> _prevRootType;
};

}  // namespace mongo
