// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

/**
 * Helpers to facilitate unit test debugging in the case of test failure.
 */
namespace mongo::StorageDebugUtil {

/**
 * Prints all the document entries in the collection table and index tables associated with
 * 'coll'.
 *
 * This is useful to facilitate debugging validate unit test failures: to more easily
 * distinguish between validate code bugs and test or data persistence bugs.
 */
void printCollectionAndIndexTableEntries(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Prints the parsed contents of 'results'.
 */
void printValidateResults(const ValidateResults& results);

}  // namespace mongo::StorageDebugUtil
