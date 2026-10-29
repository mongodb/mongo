// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/modules.h"

namespace mongo {
class FLE2EncryptionPlaceholder;
class FLE2RangeFindSpec;
class FLE2RangeInsertSpec;
class FLE2TextSearchInsertSpec;
class FLE2FindTextPayload;
/**
 * Extra validation for the placeholder struct to verify that range placeholders have min/max
 * endpoints. Will throw a uassert if the placeholder does not pass validation.
 */
void validateIDLFLE2EncryptionPlaceholder(const FLE2EncryptionPlaceholder* placeholder);
void validateIDLFLE2RangeFindSpec(const FLE2RangeFindSpec* placeholder);
void validateIDLFLE2RangeInsertSpec(const FLE2RangeInsertSpec* spec);
void validateIDLFLE2TextSearchInsertSpec(const FLE2TextSearchInsertSpec* spec);
void validateIDLFLE2FindTextPayload(const FLE2FindTextPayload* spec);
void validateQueryBounds(BSONType indexType, ImplicitValue lb, ImplicitValue ub);
bool isInfinite(ImplicitValue val);
}  // namespace mongo
