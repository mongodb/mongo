// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/dbcheck/deferred_writer.h"
#include "mongo/db/repl/dbcheck/health_log_gen.h"
#include "mongo/db/repl/dbcheck/health_log_interface.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class HealthLogEntry;

class HealthLog : public HealthLogInterface {
    HealthLog(const HealthLog&) = delete;
    HealthLog& operator=(const HealthLog&) = delete;

public:
    /**
     * Required to use HealthLog as a ServiceContext decorator.
     *
     * Should not be used anywhere else.
     */
    HealthLog();

    void startup() override;

    void shutdown() override;

    bool log(const HealthLogEntry& entry) override;

private:
    DeferredWriter _writer;
};
}  // namespace mongo
