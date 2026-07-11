// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"


namespace mongo {
[[MONGO_MOD_PRIVATE]] Status checkValidationOptionsCanBeUsed(
    const CollectionOptions& opts,
    boost::optional<ValidationLevelEnum> newLevel,
    boost::optional<ValidationActionEnum> newAction,
    boost::optional<Collection::Validator> newValidator,
    boost::optional<bool> newPrepareConstraintValidationLevel);

[[MONGO_MOD_PRIVATE]] std::pair<bool, MatchExpressionParser::AllowedFeatureSet>
mustReparseValidator(boost::optional<ValidationLevelEnum> newLevel,
                     boost::optional<ValidationActionEnum> newAction);

}  // namespace mongo
