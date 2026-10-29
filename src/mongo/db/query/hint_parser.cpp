// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/hint_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
BSONObj parseHint(const BSONElement& element) {
    if (element.type() == BSONType::string) {
        return BSON("$hint" << element.valueStringData());
    } else if (element.type() == BSONType::object) {
        return element.Obj().getOwned();
    } else {
        uasserted(ErrorCodes::FailedToParse, "Hint must be a string or an object");
    }
    MONGO_UNREACHABLE;
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeHintToBSON(const BSONObj& hint, std::string_view fieldName, BSONObjBuilder* builder) {
    if (hint.isEmpty())
        return;
    builder->append(fieldName, hint);
}

}  // namespace mongo
