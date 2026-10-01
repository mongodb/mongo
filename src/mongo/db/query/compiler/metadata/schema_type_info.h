// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/db/query/compiler/type_system/type.h"

#include <cstdint>

namespace mongo {

class MatchExpression;

/**
 * Represents the type and arrayness of each field of a collection's documents, as inferred from
 * the collection's document validator.
 */
class SchemaTypeInfo {
public:
    explicit SchemaTypeInfo(uint64_t epoch = 0)
        : _rootType(pipeline::type_system::Type::anyObject()), _epoch(epoch) {}

    SchemaTypeInfo(pipeline::type_system::Type rootType, uint64_t epoch = 0)
        : _rootType(std::move(rootType)), _epoch(epoch) {}

    uint64_t epoch() const {
        return _epoch;
    }

    void incrementEpoch() {
        ++_epoch;
    }

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

private:
    // Inferred type of the collection's root document.
    pipeline::type_system::Type _rootType;

    // Monotonically increasing version of the validator-derived metadata.
    uint64_t _epoch;
};

}  // namespace mongo
