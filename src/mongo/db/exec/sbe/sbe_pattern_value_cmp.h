// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo::sbe {

class SbePatternValueCmp {
public:
    SbePatternValueCmp();

    /*
    This constructor does not take ownership over 'specTag' and 'specVal', so it is the
    responsibility of the caller to make sure that a given 'SbePatternValueCmp' does not outlive
    the 'specTag' and the 'specVal'.
    */
    SbePatternValueCmp(value::TypeTags specTag,
                       value::Value specVal,
                       const CollatorInterface* collator);

    bool operator()(const std::pair<value::TypeTags, value::Value>& lhs,
                    const std::pair<value::TypeTags, value::Value>& rhs) const;

    BSONObj sortPattern;
    bool useWholeValue = true;
    const CollatorInterface* collator = nullptr;
    bool reversed = false;
};

}  // namespace mongo::sbe
