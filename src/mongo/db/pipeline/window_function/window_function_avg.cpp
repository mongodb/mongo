// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_avg.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/window_function/window_function_state_serialization.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

namespace {
constexpr std::string_view kSumField = "sum";
constexpr std::string_view kCountField = "count";
constexpr int kStateVersion = 1;
}  // namespace

Value WindowFunctionAvg::serializeState() const {
    return Value(Document{{window_function_state::kVersionField, kStateVersion},
                          {kSumField, RemovableSum::serializeState()},
                          {kCountField, _count}});
}

void WindowFunctionAvg::restoreState(const Value& state) {
    auto doc =
        window_function_state::validateVersionedStateDoc(state, kStateVersion, "WindowFunctionAvg");
    RemovableSum::restoreState(doc[kSumField]);
    _count = window_function_state::getLongField(doc, kCountField);
}

}  // namespace mongo
