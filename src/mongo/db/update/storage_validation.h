/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/mutable/element.h"

namespace mongo {

namespace storage_validation {

/**
 * Returns a status to indicate whether or not 'element' is a valid _id field for storage in a
 * collection.
 */
Status storageValidIdField(const mongo::BSONElement& element);

/**
 * Validates that the MutableBSON document 'doc' is acceptable for storage in a collection. The
 * check is performed recursively on subdocuments. Uasserts if the validation fails or if the depth
 * exceeds the maximum allowable depth.
 *
 * When the dots and dollars feature flag is off, always reject $-prefixed fields. Otherwise, reject
 * only $-prefixed fields at the top-level of a document. If 'allowTopLevelDollarPrefixes' is set to
 * true, do not reject $-prefixed fields at the top-level of a document.
 *
 * 'shouldValidate' is true if the caller wants to validate for storage, otherwise this helper will
 * only check top-level $-prefixed field names skipping all the validations.
 *
 * 'containsDotsAndDollarsField' is set to true if there exists any field name containing '.'/'$'
 * during validation.
 */
void storageValid(const mutablebson::Document& doc,
                  const bool allowTopLevelDollarPrefixes,
                  const bool shouldValidate,
                  bool* containsDotsAndDollarsField);

/**
 * Validates that the MutableBSON element 'elem' is acceptable for storage in a collection. If
 * 'deep' is true, the check is performed recursively on subdocuments. Uasserts if the validation
 * fails or if 'recursionLevel' exceeds the maximum allowable depth.
 *
 * When the dots and dollars feature flag is off, always reject $-prefixed fields. Otherwise, reject
 * only $-prefixed fields at the top-level of a document. If 'allowTopLevelDollarPrefixes' is set to
 * true, do not reject $-prefixed fields at the top-level of a document.
 *
 * 'shouldValidate' is true if the caller wants to validate for storage, otherwise this helper will
 * only check top-level $-prefixed field names skipping all the validations.
 *
 * 'containsDotsAndDollarsField' is set to true if there exists any field name containing '.'/'$'
 * during validation.
 */
void storageValid(mutablebson::ConstElement elem,
                  const bool deep,
                  std::uint32_t recursionLevel,
                  const bool allowTopLevelDollarPrefixes,
                  const bool shouldValidate,
                  bool* containsDotsAndDollarsField);

}  // namespace storage_validation

}  // namespace mongo
