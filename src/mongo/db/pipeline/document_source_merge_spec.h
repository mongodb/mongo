// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_merge_modes_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class BSONObjBuilder;
class BSONElement;

// Defines a policy strategy describing what to do when there is a matching document in the target
// collection. Can hold a value from the MergeWhenMatchedModeEnum, or a custom pipeline definition.
struct MergeWhenMatchedPolicy {
    MergeWhenMatchedModeEnum mode;
    boost::optional<std::vector<BSONObj>> pipeline;
};

/**
 * Serialize and deserialize functions for the $merge stage 'into' field which can be a single
 * string value, or an object.
 */
void mergeTargetNssSerializeToBSON(const NamespaceString& targetNss,
                                   std::string_view fieldName,
                                   BSONObjBuilder* bob,
                                   const SerializationContext& sc,
                                   const query_shape::SerializationOptions& opts);
NamespaceString mergeTargetNssParseFromBSON(boost::optional<TenantId> tenantId,
                                            const BSONElement& elem,
                                            const SerializationContext& sc);

/**
 * Serialize and deserialize functions for the $merge stage 'on' field which can be a single string
 * value, or array of strings.
 */
void mergeOnFieldsSerializeToBSON(const std::vector<std::string>& fields,
                                  std::string_view fieldName,
                                  BSONObjBuilder* bob,
                                  const query_shape::SerializationOptions& opts = {});
std::vector<std::string> mergeOnFieldsParseFromBSON(const BSONElement& elem);

/**
 * Serialize and deserialize functions for the $merge stage 'whenMatched' field which can be either
 * a string value, or an array of objects defining a custom pipeline.
 */
void mergeWhenMatchedSerializeToBSON(const MergeWhenMatchedPolicy& policy,
                                     std::string_view fieldName,
                                     BSONObjBuilder* bob);
MergeWhenMatchedPolicy mergeWhenMatchedParseFromBSON(const BSONElement& elem);
}  // namespace mongo
