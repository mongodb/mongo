// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/schema_type_info.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/compiler/type_system/matcher_typing.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

Type SchemaTypeInfo::getPathType(const FieldRef& path) const {
    return pipeline::type_system::getPathType(_rootType, path);
}

void CollectionSchemaChecker::uassertIfInvalidatedAndSyncSchema(const SchemaTypeInfo& current,
                                                                const NamespaceString& nss) {
    const auto& currentRootType = current.getRootType();
    if (_prevRootType && *_prevRootType == currentRootType) {
        return;
    }

    const bool hadPrevRootType = _prevRootType.has_value();
    _prevRootType = currentRootType;

    if (hadPrevRootType) {
        uasserted(ErrorCodes::QueryPlanKilled,
                  str::stream()
                      << "query plan killed :: collection schema metadata changed during yield: "
                         "namespace="
                      << nss.toStringForErrorMsg());
    }
}

}  // namespace mongo
