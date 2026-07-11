// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * Finds an object-typed field named "fieldName" in "object" that represents an OpTime.
 *
 * The OpTime objects have two fields, a Timestamp ts and numeric term.
 *
 * Returns Status::OK() and sets *out to the found element's OpTime value on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName" or either subobject field is
 * missing, and ErrorCodes::TypeMismatch if the type of the matching element is not Object, the ts
 * subfield is not Timestamp, or the term subfield is not numeric.  For return values other than
 * Status::OK(), the resulting value of "*out" is undefined.
 */
Status bsonExtractOpTimeField(const BSONObj& object, std::string_view fieldName, repl::OpTime* out);

}  // namespace mongo
