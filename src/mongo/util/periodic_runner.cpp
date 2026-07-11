// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/periodic_runner.h"

#include "mongo/util/assert_util.h"

namespace mongo {

PeriodicRunner::~PeriodicRunner() = default;

PeriodicJobAnchor::PeriodicJobAnchor(std::shared_ptr<Job> handle) : _handle{std::move(handle)} {}

PeriodicJobAnchor::~PeriodicJobAnchor() {
    if (!_handle) {
        return;
    }

    _handle->stop();
}

void PeriodicJobAnchor::start() {
    invariant(_handle);
    _handle->start();
}

void PeriodicJobAnchor::pause() {
    invariant(_handle);
    _handle->pause();
}

void PeriodicJobAnchor::resume() {
    invariant(_handle);
    _handle->resume();
}

void PeriodicJobAnchor::stop() {
    invariant(_handle);
    _handle->stop();
}

void PeriodicJobAnchor::setPeriod(Milliseconds ms) {
    invariant(_handle);
    _handle->setPeriod(ms);
}

Milliseconds PeriodicJobAnchor::getPeriod() const {
    invariant(_handle);
    return _handle->getPeriod();
}

void PeriodicJobAnchor::detach() {
    _handle.reset();
}

bool PeriodicJobAnchor::isValid() const noexcept {
    return static_cast<bool>(_handle);
}

}  // namespace mongo
