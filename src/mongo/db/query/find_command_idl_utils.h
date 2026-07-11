// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::find_command_idl_utils {

/** Should not be modified, as API version I/O guarantees could break. */
void noOpSerializer(bool, std::string_view fieldName, BSONObjBuilder* bob);

/** Should not be modified, as API version I/O guarantees could break. */
void serializeBSONWhenNotEmpty(BSONObj obj, std::string_view fieldName, BSONObjBuilder* bob);

/** Should not be modified, as API version I/O guarantees could break. */
BSONObj parseOwnedBSON(BSONElement element);

/** Should not be modified, as API version I/O guarantees could break. */
bool parseBoolean(BSONElement element);

}  // namespace mongo::find_command_idl_utils
