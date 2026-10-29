// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/encrypt_schema_types.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

void EncryptSchemaKeyId::serializeToBSON(std::string_view fieldName,
                                         BSONObjBuilder* builder) const {
    if (_type == Type::kUUIDs) {
        BSONArrayBuilder arrBuilder(builder->subarrayStart(fieldName));
        for (auto uuid : _uuids) {
            uuid.appendToArrayBuilder(&arrBuilder);
        }
        arrBuilder.doneFast();
    } else {
        builder->append(fieldName, _pointer.toString());
    }
}

void BSONTypeSet::serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
    builder->appendArray(fieldName, _typeSet.toBSONArray());
}

}  // namespace mongo
