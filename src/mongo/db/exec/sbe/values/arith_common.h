// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

namespace mongo::sbe::value {

value::TagValueMaybeOwned genericAdd(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue);
value::TagValueMaybeOwned genericSub(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue);
value::TagValueMaybeOwned genericMul(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue);
value::TagValueMaybeOwned genericNumConvert(value::TypeTags lhsTag,
                                            value::Value lhsValue,
                                            value::TypeTags targetTag);
}  // namespace mongo::sbe::value
