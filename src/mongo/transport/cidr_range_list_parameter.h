// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/versioned_value.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] transport {

/**
 * Appends a CIDR range list to a bson object. The array will be appended as a subobject using a key
 * with the value of the 'name' parameter.
 */
void appendCIDRRangeListParameter(VersionedValue<CIDRList>& value,
                                  BSONObjBuilder* bob,
                                  std::string_view name);

/**
 * Parses the 'obj' bson object and sets 'value' if parsing the CIDR range was sucessful. If parsing
 * was not sucessful, an error status from the thrown exception is returned.
 */
Status setCIDRRangeListParameter(VersionedValue<CIDRList>& value, BSONObj obj);

// TODO: SERVER-106468 Define CIDRRangeListParameter here instead of generating the code.

}  // namespace transport
}  // namespace mongo
