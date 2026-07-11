// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_context_test_util.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_context.h"

#include <utility>

namespace mongo::query_settings {

QuerySettingsGuardForTest::QuerySettingsGuardForTest(OperationContext* opCtx, BSONObj settingsObj)
    : QuerySettingsGuardForTest(
          opCtx, QuerySettings::parse(settingsObj, IDLParserContext("QuerySettingsGuardForTest"))) {
}

QuerySettingsGuardForTest::QuerySettingsGuardForTest(OperationContext* opCtx,
                                                     const QuerySettings& settings)
    // Bind the operation's query settings state (deriving eligibility on first access). '_previous'
    // starts holding the resolved 'settings'; the swaps below install them and, on destruction,
    // restore whatever state the operation had before.
    : _state(&query_settings_details::getQuerySettingsStateForOp(opCtx)), _previous(settings) {
    using std::swap;
    swap(*_state, _previous);
}

QuerySettingsGuardForTest::~QuerySettingsGuardForTest() {
    if (_state) {
        using std::swap;
        swap(*_state, _previous);
    }
}

}  // namespace mongo::query_settings
