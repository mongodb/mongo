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

#include "mongo/db/namespace_string.h"

namespace mongo {

class OperationContext;
class Collection;
struct ValidateResults;
class BSONObjBuilder;
class Status;

namespace CollectionValidation {

enum class ValidateOptions {
    kNoFullValidation = 0,

    // If the FullRecordStoreValidation option is set, validate() will do a full validation of the
    // underlying record store using the storage engine's validation functionality. For WiredTiger
    // this results in a call to verify().
    kFullRecordStoreValidation = 1 << 0,
    // If set, validate() will validate the internal structure of each index, and checks consistency
    // of the number of keys in the index compared to the internal structure.
    kFullIndexValidation = 1 << 1,
    // Includes all of the full validations above.
    kFullValidation = kFullRecordStoreValidation | kFullIndexValidation,
};

inline bool operator&(ValidateOptions lhs, ValidateOptions rhs) {
    return (static_cast<int>(lhs) & static_cast<int>(rhs)) != 0;
}

/**
 * Expects the caller to hold no locks.
 *
 * Background validation does not support any type of full validation above.
 * The combination of background = true and options of anything other than kNoFullValidation is
 * prohibited.
 *
 * @return OK if the validate run successfully
 *         OK will be returned even if corruption is found
 *         details will be in 'results'.
 */
Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidateOptions options,
                bool background,
                ValidateResults* results,
                BSONObjBuilder* output,
                bool turnOnExtraLoggingForTest = false);

/**
 * Checks whether a failpoint has been hit in the above validate() code..
 */
bool getIsValidationPausedForTest();

}  // namespace CollectionValidation
}  // namespace mongo
