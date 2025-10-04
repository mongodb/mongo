/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"

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
