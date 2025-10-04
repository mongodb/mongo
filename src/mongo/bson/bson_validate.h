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

#include "mongo/base/status.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"

#include <cstdint>

namespace mongo {

enum ValidationVersion {
    /* Original validator */
    V1_Original = 1,
    /* Adds validation for the content of Column-typed BinData */
    V2_Column = 2
};

// When adding new versions of BSON validation, update both this and the range and the
// default for the server parameter in src/mongo/bson/bson_validate.idl
static constexpr ValidationVersion currentValidationVersion = V2_Column;

/**
 * Checks that the buf holds a BSON object as defined in http://bsonspec.org/spec.html.
 * Note that maxLength is the buffer size, NOT the BSON size.
 * Validation errors result in returning an InvalidBSON or Overflow status.
 * For the default validation mode, the checks are structural only, and include:
 *    - String, Object, Array, BinData, DBRef, Code, Symbol and CodeWScope lengths are correct.
 *    - Field names, String, Object, Array, DBRef, Code, Symbol, and CodeWScope end with NUL.
 *    - Bool values are false (0) or true (1).
 *    - Correct nesting, not exceeding maximum allowable nesting depth.
 * For the extended validation mode, the checks include everything above and:
 *    - Deprecated types are not used.
 *    - Contents of array indices are consecutively numbered from zero.
 *    - Correct UUID and MD5 lengths.
 *    - Structurally correct encrypted BSON values.
 *    - Valid regular expression options.
 * For the full validation mode, the checks include everything above and:
 *    - Field names are not duplicated in the same level.
 *    - Validity of UTF-8 strings.
 *    - Valid compressed BSON columns.
 * Length is only limited by the buffer's maxLength and the inherent 2GB - 1 format limitation.
 */
Status validateBSON(const char* buf,
                    uint64_t maxLength,
                    BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                    ValidationVersion validationVersion = currentValidationVersion) noexcept;

Status validateBSON(const BSONObj& obj,
                    BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                    ValidationVersion validationVersion = currentValidationVersion) noexcept;

Status validateBSONColumn(const char* buf,
                          int maxLength,
                          BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                          ValidationVersion validationVersion = currentValidationVersion) noexcept;

}  // namespace mongo
