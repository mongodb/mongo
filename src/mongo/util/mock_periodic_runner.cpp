// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/mock_periodic_runner.h"

namespace mongo {

namespace periodic_runner_detail {

MockPeriodicJob::MockPeriodicJob(PeriodicRunner::PeriodicJob job) : _job(std::move(job)) {}

void MockPeriodicJob::start() {}

void MockPeriodicJob::pause() {}

void MockPeriodicJob::resume() {}

void MockPeriodicJob::stop() {}

Milliseconds MockPeriodicJob::getPeriod() const {
    return _job.interval;
}

void MockPeriodicJob::setPeriod(Milliseconds period) {
    _job.interval = period;
}

void MockPeriodicJob::run(Client* client) {
    _job.job(client);
}

}  // namespace periodic_runner_detail

PeriodicRunner::JobAnchor MockPeriodicRunner::makeJob(PeriodicJob job) {
    invariant(!_job);
    auto mockJob = std::make_shared<periodic_runner_detail::MockPeriodicJob>(std::move(job));
    _job = mockJob;
    return JobAnchor{std::move(mockJob)};
}

void MockPeriodicRunner::run(Client* client) {
    invariant(_job);
    _job->run(client);
}


}  // namespace mongo
