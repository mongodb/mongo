// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/deadline_monitor.h"

#include "mongo/platform/atomic.h"
#include "mongo/scripting/deadline_monitor_gen.h"


namespace mongo {

int getScriptingEngineInterruptInterval() {
    return gScriptingEngineInterruptIntervalMS.load();
}

}  // namespace mongo
