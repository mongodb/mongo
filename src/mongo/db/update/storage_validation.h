// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

namespace storage_validation {

/**
 * Validates that the MutableBSON document 'doc' is acceptable for storage in a collection and
 * checks if there exists any field name containing '.'/'$'. The check is performed recursively on
 * subdocuments. Uasserts if the validation fails or if the depth exceeds the maximum allowable
 * depth. If 'allowTopLevelDollarPrefixes' is set to false, reject $-prefixed fields at the
 * top-level of a document.

 * 'shouldValidate' is true if the caller wants to validate for storage, otherwise this helper will
 * only check top-level $-prefixed field names skipping all the validations.
 *
 * 'containsDotsAndDollarsField' is set to true if there exists any field name containing '.'/'$'
 * during validation.
 */
[[MONGO_MOD_PUBLIC]] void scanDocument(const mutablebson::Document& doc,
                                       bool allowTopLevelDollarPrefixes,
                                       bool shouldValidate,
                                       bool* containsDotsAndDollarsField,
                                       bool fromOplogApplication);

/**
 * Validates that the MutableBSON element 'elem' is acceptable for storage in a collection and
 * checks if there exists any field name containing '.'/'$'. If 'deep' is true, the check is
 * performed recursively on subdocuments. Uasserts if the validation fails or if 'recursionLevel'
 * exceeds the maximum allowable depth.
 *
 * If 'allowTopLevelDollarPrefixes' is set to false, reject $-prefixed fields at the top-level of a
 * document.
 *
 * 'shouldValidate' is true if the caller wants to validate for storage, otherwise this helper will
 * only check top-level $-prefixed field names skipping all the validations.
 *
 * 'containsDotsAndDollarsField' is set to true if there exists any field name containing '.'/'$'
 * during validation.
 *
 * 'isEmbeddedInIdField' is set to true if the element is embedded inside an _id field. This allows
 * to reject $-prefixed fields at all levels under an _id field.
 *
 * 'fromOplogApplication' is true if the caller is applying an oplog update from an external
 * source, which enables validation of BSONColumn binary data.
 */
void scanDocument(mutablebson::ConstElement elem,
                  bool deep,
                  std::uint32_t recursionLevel,
                  bool allowTopLevelDollarPrefixes,
                  bool shouldValidate,
                  bool isEmbeddedInIdField,
                  bool* containsDotsAndDollarsField,
                  bool fromOplogApplication);

}  // namespace storage_validation

}  // namespace mongo
