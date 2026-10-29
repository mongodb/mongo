// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>


namespace mongo::abt {

enum class ExplainVersion { V2, V3, Vmax };

class ExplainGenerator {
public:
    static std::string explainV2(ABT::reference_type node);
    static std::pair<sbe::value::TypeTags, sbe::value::Value> explainBSON(ABT::reference_type node);
    static std::string explainBSONStr(ABT::reference_type node);
};

}  // namespace mongo::abt
