// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"

namespace mongo {

QueryKnobGuardForTest::~QueryKnobGuardForTest() {
    if (_opCtx) {
        _resetCachedConfiguration();
    }
}

void QueryKnobGuardForTest::_resetCachedConfiguration() {
    QueryKnobConfiguration::reset_forTest(_opCtx);
}

}  // namespace mongo
