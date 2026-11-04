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

/**
 * Checks whether a collMod that sets validationLevel to 'constraint' could be accepted for a
 * collection with options 'opts', given the other validation fields in the same request. This runs
 * the same rules as 'checkValidationOptionsCanBeUsed', so it only needs the request metadata and
 * can be used to reject an invalid upgrade before paying for a scan of the collection.
 */
[[MONGO_MOD_PUBLIC]] Status checkConstraintUpgradePreconditions(
    const CollectionOptions& opts,
    boost::optional<ValidationActionEnum> newAction,
    bool hasNewValidator);

[[MONGO_MOD_PRIVATE]] std::pair<bool, MatchExpressionParser::AllowedFeatureSet>
mustReparseValidator(boost::optional<ValidationLevelEnum> newLevel,
                     boost::optional<ValidationActionEnum> newAction);

}  // namespace mongo
