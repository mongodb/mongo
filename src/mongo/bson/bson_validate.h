// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

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
 *
 * When 'outDescription' is non-null and validation fails, it may be populated with a short, fixed
 * description of the check that failed, naming the failing check but containing none of the
 * document's data (unlike the returned Status' reason, which is annotated with observed lengths,
 * field paths and the document's _id). Every distinct check has its own wording, so the set of
 * possible descriptions is small and callers can distinguish failures without parsing the
 * data-annotated message. It is left unchanged if validation succeeds, or if the failure came from
 * code outside bson_validate.cpp that carries no description.
 *
 * The description is threaded back purely through parameters; it is never attached to the returned
 * Status and so is never serialized to a client. It is intended for server-side diagnostics such
 * as collection validation.
 */
Status validateBSON(const char* buf,
                    uint64_t maxLength,
                    BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                    ValidationVersion validationVersion = currentValidationVersion,
                    boost::optional<std::string>* outDescription = nullptr) noexcept;

Status validateBSON(const BSONObj& obj,
                    BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                    ValidationVersion validationVersion = currentValidationVersion,
                    boost::optional<std::string>* outDescription = nullptr) noexcept;

Status validateBSONColumn(const char* buf,
                          int maxLength,
                          BSONValidateModeEnum mode = BSONValidateModeEnum::kDefault,
                          ValidationVersion validationVersion = currentValidationVersion) noexcept;

// Validates JS-produced BSON. Throws InvalidBSONFromJavaScript on failure, preserves
// ExceededMemoryLimit.
void uassertValidBSONFromJavaScript(const BSONObj& obj, std::string_view context);

/**
 * Validates the nesting depth of 'obj', returning ErrorCodes::Overflow if it exceeds the depth
 * limit getMaxDepthForUserStorage() sets. That limit is lower than the one validateBSON() enforces,
 * because a depth level buffer is needed to account for the nesting the server adds when it embeds
 * 'obj' in an oplog entry or a command reply.
 */
Status validateBSONDepthForUserStorage(const BSONObj& obj);

/**
 * Same as validateBSONDepthForUserStorage(const BSONObj&), but additionally invokes
 * 'topLevelVisitor' for every top-level element of 'obj' while the depth validation traversal is
 * already visiting those elements. This allows callers to perform top-level validation work in the
 * same pass as the depth check. If the visitor encounters an error, it should throw a
 * 'DBException'; this function will catch it and return the corresponding Status.
 */
Status validateBSONDepthForUserStorage(
    const BSONObj& obj, const std::function<void(const BSONElement&)>& topLevelVisitor);

}  // namespace mongo
