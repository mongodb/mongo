// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

namespace mongo::sbe::value {
using namespace std::literals::string_view_literals;

/**
 * Helper function for converting mongo::Value to SBE Value. Caller owns the SBE Value returned.
 */
inline std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const ::mongo::Value& val) {
    // TODO SERVER-73443: Rationalize Document Value to SBE Value Conversion.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""sv);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + obj.objsize();
    return sbe::bson::convertToOwned(be + 4, end, 0).releaseToRaw();
}

}  // namespace mongo::sbe::value
