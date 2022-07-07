/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class OperationContext;
class Collection;
class CollectionPtr;
class BSONObjBuilder;
class Status;

namespace CollectionValidation {

enum class ValidateMode {
    // Only performs validation on the collection metadata.
    kMetadata,
    // Does the above, plus checks a collection's data and indexes for correctness in a non-blocking
    // manner using an intent collection lock.
    kBackground,
    // Does the above, plus checks BSON documents more thoroughly.
    kBackgroundCheckBSON,
    // Does the above, but in a blocking manner using an exclusive collection lock.
    kForeground,

    // The standard foreground validation above, plus a full validation of the underlying
    // SortedDataInterface using the storage engine's validation functionality. For WiredTiger,
    // this results in a call to verify() for each index.
    //
    // This mode is only used by repair to avoid revalidating the record store.
    kForegroundFullIndexOnly,

    // The standard foreground validation above, plus a more thorough BSON document validation.
    kForegroundCheckBSON,

    // The full index validation and more thorough BSON document validation above, plus a full
    // validation of the underlying record store using the storage engine's validation
    // functionality. For WiredTiger, this results in a call to
    // verify().
    kForegroundFull,

    // The full index, BSON document, and record store validation above, plus enforce that the fast
    // count is equal to the number of records (as opposed to correcting the fast count if it is
    // incorrect).
    kForegroundFullEnforceFastCount,
};

/**
 * RepairMode indicates whether validate should repair the data inconsistencies it detects.
 *
 * When set to kFixErrors, if any validation errors are detected, repairs are attempted and the
 * 'repaired' flag in ValidateResults will be set to true. If all errors are fixed, then 'valid'
 * will also be set to true. kFixErrors is incompatible with the ValidateModes kBackground and
 * kForegroundFullEnforceFastCount. This implies kAdjustMultikey.
 *
 * When set to kAdjustMultikey, if any permissible, yet undesirable multikey inconsistencies are
 * detected, then the multikey metadata will be adjusted. The 'repaired' flag will be set if any
 * adjustments have been made. This is incompatible with background validation.
 */
enum class RepairMode {
    kNone,
    kFixErrors,
    kAdjustMultikey,
};

/**
 * Expects the caller to hold no locks.
 *
 * @return OK if the validate run successfully
 *         OK will be returned even if corruption is found
 *         details will be in 'results'.
 */
Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidateMode mode,
                RepairMode repairMode,
                ValidateResults* results,
                BSONObjBuilder* output,
                bool turnOnExtraLoggingForTest = false);

/**
 * Checks whether a failpoint has been hit in the above validate() code..
 */
bool getIsValidationPausedForTest();

}  // namespace CollectionValidation
}  // namespace mongo
