// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

namespace [[MONGO_MOD_FILE_PRIVATE]] periodic_runner_detail {

class MockPeriodicJob : public PeriodicRunner::ControllableJob {
public:
    explicit MockPeriodicJob(PeriodicRunner::PeriodicJob job);

    void start() override;
    void pause() override;
    void resume() override;
    void stop() override;

    Milliseconds getPeriod() const override;
    void setPeriod(Milliseconds period) override;

    void run(Client* client);

private:
    PeriodicRunner::PeriodicJob _job;
};

}  // namespace periodic_runner_detail

class [[MONGO_MOD_PUBLIC]] MockPeriodicRunner : public PeriodicRunner {
public:
    JobAnchor makeJob(PeriodicJob job) override;

    void run(Client* client);

    // Escape hatch to allow creating multiple jobs with a single MockPeriodicRunner instance.
    void resetJob() {
        _job.reset();
    }

private:
    std::shared_ptr<periodic_runner_detail::MockPeriodicJob> _job;
};

}  // namespace mongo
