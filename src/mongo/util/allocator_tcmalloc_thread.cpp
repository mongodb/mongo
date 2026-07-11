// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"
#include "mongo/util/allocator_thread.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif

#include "mongo/db/client.h"
#include "mongo/stdx/thread.h"

namespace mongo {

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
namespace {

// Thread to process tcmalloc background work
stdx::thread tcmallocThread;


void tcmallocThreadRun() {

    Client::initThread("tcmallocBg", getGlobalServiceContext()->getService());

    // This function never returns. It just sleeps and loops
    tcmalloc::MallocExtension::ProcessBackgroundActions();
}

}  // namespace

void startAllocatorThread() {

    if (!TCMallocEnableBackgroundThread) {
        return;
    }

    if (tcmalloc::MallocExtension::NeedsProcessBackgroundActions()) {
        // Start background thread
        tcmallocThread = stdx::thread(tcmallocThreadRun);
    }
}
#else


void startAllocatorThread() {
    // Do nothing
}

#endif

}  // namespace mongo
