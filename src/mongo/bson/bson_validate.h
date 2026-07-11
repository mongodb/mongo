// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

enum ValidationVersion {
    /* Original validator */
    V1_Original = 1,
    /* Adds validation for the content of Column-typed BinData */
    V2_Column = 2
};

// When adding new versions of BSON validation, update both this and the range and the
// default for the server parameter in src/mongo/bson/bson_validate.idl
constexpr inline ValidationVersion currentValidationVersion = V2_Column;

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
