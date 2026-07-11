// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class NamespaceString;
class OperationContext;
class Collection;
class CollectionPtr;
class BSONObjBuilder;
class Status;
class ValidateResults;

namespace collection_validation {

/**
 * Checks if 'hashPrefixes' contains valid hash strings. Throws if any is invalid.
 * When 'equalLength' is true, also checks all hash strings have the same length.
 */
[[MONGO_MOD_FILE_PRIVATE]] void validateHashes(const std::vector<std::string>& hashPrefixes,
                                               bool equalLength);

/**
 * Parses and checks the command object and returns a 'ValidationOptions' object used for collection
 * validation.
 * Optionally skips parsing 'atClusterTime' for unreplicated collections, which is desired with
 * modal validation usage.
 */
ValidationOptions parseValidateOptions(OperationContext* opCtx,
                                       NamespaceString nss,
                                       const BSONObj& cmdObj,
                                       bool skipAtClusterTime = false);

/**
 * Expects the caller to hold no locks.
 *
 * @return OK if the validate run successfully
 *         OK will be returned even if corruption is found
 *         details will be in 'results'.
 */
Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidationOptions options,
                ValidateResults* results);

/**
 * Checks whether a failpoint has been hit in the above validate() code..
 */
[[MONGO_MOD_FILE_PRIVATE]] bool getIsValidationPausedForTest();

}  // namespace collection_validation
}  // namespace mongo
