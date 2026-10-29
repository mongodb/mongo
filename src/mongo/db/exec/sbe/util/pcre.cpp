// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/pcre.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"

#include <string_view>

namespace mongo::sbe {
value::TagValueOwned makeNewPcreRegex(std::string_view pattern, std::string_view options) {
    auto regex = std::make_unique<pcre::Regex>(
        std::string{pattern},
        pcre_util::flagsToOptions(options),
        pcre::Limits{
            .heapLimitKB = static_cast<uint32_t>(internalQueryRegexHeapLimitKB.loadRelaxed()),
            .matchLimit = static_cast<uint32_t>(internalQueryRegexMatchLimit.loadRelaxed())});

    uassert(5073402, str::stream() << "Invalid Regex: " << errorMessage(regex->error()), *regex);
    return value::TagValueOwned::fromRaw(value::TypeTags::pcreRegex,
                                         value::bitcastFrom<pcre::Regex*>(regex.release()));
}
}  // namespace mongo::sbe
