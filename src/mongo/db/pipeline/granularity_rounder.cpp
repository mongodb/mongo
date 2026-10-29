// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/granularity_rounder.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using Rounder = GranularityRounder::Rounder;

namespace {
// Used to keep track of which GranularityRounders are registered under which name.
StringMap<Rounder> rounderMap;
}  // namespace

void GranularityRounder::registerGranularityRounder(std::string_view name, Rounder rounder) {
    auto it = rounderMap.find(name);
    massert(40256,
            str::stream() << "Duplicate granularity rounder (" << name << ") registered.",
            it == rounderMap.end());
    rounderMap[name] = rounder;
}

boost::intrusive_ptr<GranularityRounder> GranularityRounder::getGranularityRounder(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::string_view granularity) {
    auto it = rounderMap.find(granularity);
    uassert(40257,
            str::stream() << "Unknown rounding granularity '" << granularity << "'",
            it != rounderMap.end());
    return it->second(expCtx);
}
}  // namespace mongo
