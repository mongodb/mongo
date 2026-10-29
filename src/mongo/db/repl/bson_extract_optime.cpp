// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/bson_extract_optime.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/optime.h"

#include <string_view>

namespace mongo {

Status bsonExtractOpTimeField(const BSONObj& object,
                              std::string_view fieldName,
                              repl::OpTime* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, BSONType::object, &element);
    if (!status.isOK())
        return status;

    BSONObj opTimeObj = element.Obj();
    Timestamp ts;
    status = bsonExtractTimestampField(opTimeObj, repl::OpTime::kTimestampFieldName, &ts);
    if (!status.isOK())
        return status;
    long long term;
    status = bsonExtractIntegerField(opTimeObj, repl::OpTime::kTermFieldName, &term);
    if (!status.isOK())
        return status;
    *out = repl::OpTime(ts, term);
    return Status::OK();
}

}  // namespace mongo
