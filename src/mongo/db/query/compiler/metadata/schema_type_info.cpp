// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/schema_type_info.h"

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

namespace mongo {

using namespace pipeline::type_system;

void SchemaTypeInfo::populateFromValidator(const MatchExpression* validator) {
    if (!validator) {
        _rootType = Type::anyObject();
        return;
    }
    _rootType = narrowType(Type::anyObject(), validator, /*assumeTrue=*/true);
}

bool SchemaTypeInfo::canPathBeArray(const FieldRef& path) const {
    return _rootType.canPathBeArray(path);
}

}  // namespace mongo
