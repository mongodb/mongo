// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/util/modules.h"

#include <string_view>

// This file has utilities for usage by the generated IDL parsers which are
// considered part of the module with the .idl file, rather than the core.idl
// module. These utilities are not for general consumption outside of this module.
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]];

namespace mongo::idl {

// Serializers and deserializers for basic_types.idl

// OK statuses will throw.
void serializeErrorStatus(const Status& status,
                          std::string_view fieldName,
                          BSONObjBuilder* builder);

// OK status codes will throw. The BSON object may include fields "code" and "errmsg", which
// are parsed into the returned Status.
Status deserializeErrorStatus(const BSONElement& bsonElem);

Timestamp deserializeTimestamp(const BSONElement& bsonElem);

// Asserts that the BSONElement has bson type Timestamp.
LogicalTime deserializeLogicalTime(const BSONElement& e);

}  // namespace mongo::idl
