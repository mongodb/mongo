/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
