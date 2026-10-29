// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class FTDCController;

/**
 * Base class for system metrics collectors. Sets collector name to a common name all system metrics
 * collectors to use.
 */
class SystemMetricsCollector : public FTDCCollectorInterface {
public:
    std::string name() const final;

protected:
    /**
     * Convert any errors we see into BSON for the user to see in the final FTDC document. It is
     * acceptable for the collector to fail, but we do not want to shutdown the FTDC loop because
     * of it. We assume that the BSONBuilder is not corrupt on non-OK Status but nothing else with
     * regards to the final document output.
     */
    static void processStatusErrors(Status s, BSONObjBuilder* builder);
};


/**
 * Install system metrics collectors (if any exist).
 */
void installSystemMetricsCollector(FTDCController* controller);

}  // namespace mongo
