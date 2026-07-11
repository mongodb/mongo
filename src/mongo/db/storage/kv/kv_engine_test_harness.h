// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>

namespace mongo {

class ClockSource;

/**
 * Creates a harness for generic KVEngine testing of all KVEngine implementations.
 *
 * A particular KVHarnessHelper implementation (with a particular KVEngine implementation) will
 * implement registerFactory() and create() such that generic unit tests can create() and test the
 * particular KVHarnessHelper implementation. This library can be pulled into a particular
 * implementation's CppUnitTest to exercise the generic test coverage on that implementation.
 */
class KVHarnessHelper {
public:
    virtual ~KVHarnessHelper() {}

    // returns same thing for entire life
    virtual KVEngine* getEngine() = 0;

    virtual KVEngine* restartEngine() = 0;

    static std::unique_ptr<KVHarnessHelper> create(ServiceContext* svcCtx);
    static void registerFactory(
        std::function<std::unique_ptr<KVHarnessHelper>(ServiceContext*)> factory);
};

}  // namespace mongo
