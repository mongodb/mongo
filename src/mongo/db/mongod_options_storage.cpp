// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/operation_context.h"

#include <string_view>

namespace mongo {

Status CollectionValidateOptionsServerParameter::setFromString(std::string_view value) {
    _data = CollectionValidateOptions::parse(fromjson(value),
                                             IDLParserContext("collection validate options"));
    return Status::OK();
}

Status CollectionValidateOptionsServerParameter::set(const BSONElement& newValueElement) {
    _data = CollectionValidateOptions::parse(newValueElement.Obj(),
                                             IDLParserContext("collection validate options"));
    return Status::OK();
}

void CollectionValidateOptionsServerParameter::append(OperationContext*,
                                                      BSONObjBuilder* bob,
                                                      std::string_view name) {
    auto subBob = BSONObjBuilder(bob->subobjStart(name));
    _data.serialize(&subBob);
}
}  // namespace mongo
