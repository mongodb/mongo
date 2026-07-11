// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {

class CollectionWriter;
class OperationContext;
class Status;

/**
 * Creates the appropriate indexes on _new_ system collections for authentication,
 * authorization, and sessions.
 */
[[MONGO_MOD_PUBLIC]] void createSystemIndexes(OperationContext* opCtx,
                                              CollectionWriter& collection,
                                              bool fromMigrate);

/**
 * Verifies that only the appropriate indexes to support authentication, authorization, and
 * sessions are present in the admin database. Will create new indexes, if they are missing.
 * The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
 * this function into one single builder that records the time elapsed during startup. Its default
 * value is nullptr because we only want to time this function when it is called during startup.
 */
[[MONGO_MOD_PUBLIC]] Status verifySystemIndexes(
    OperationContext* opCtx, BSONObjBuilder* startupTimeElapsedBuilder = nullptr);

}  // namespace mongo
