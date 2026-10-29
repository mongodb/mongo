// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/validate_id.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

namespace mongo {
/**
 * Returns a status to indicate whether or not 'element' is a valid _id field for storage in a
 * collection.
 */
Status validIdField(const mongo::BSONElement& element) {
    switch (element.type()) {
        case BSONType::regEx:
        case BSONType::array:
        case BSONType::undefined:
            return Status(ErrorCodes::InvalidIdField,
                          str::stream()
                              << "The '_id' value cannot be of type " << typeName(element.type()));
        case BSONType::object: {
            auto status = element.Obj().storageValidEmbedded();
            if (!status.isOK() && status.code() == ErrorCodes::DollarPrefixedFieldName) {
                return Status(status.code(),
                              str::stream() << "_id fields may not contain '$'-prefixed fields: "
                                            << status.reason());
            }
            return status;
        }
        default:
            break;
    }
    return Status::OK();
}
}  // namespace mongo
