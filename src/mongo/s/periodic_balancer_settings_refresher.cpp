/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/periodic_balancer_settings_refresher.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const Milliseconds kRefreshInterval(10 * 1000);

const auto getPeriodicBalancerSettingsRefresher =
    ServiceContext::declareDecoration<std::unique_ptr<PeriodicBalancerSettingsRefresher>>();

}  // namespace

PeriodicBalancerSettingsRefresher::PeriodicBalancerSettingsRefresher(bool isPrimary)
    : _isPrimary(isPrimary) {
    _thread = stdx::thread([this] { _periodicRefresh(); });
}

PeriodicBalancerSettingsRefresher::~PeriodicBalancerSettingsRefresher() {
    invariant(!_thread.joinable());
}

void PeriodicBalancerSettingsRefresher::create(ServiceContext* serviceContext, bool isPrimary) {
    invariant(!getPeriodicBalancerSettingsRefresher(serviceContext));
    getPeriodicBalancerSettingsRefresher(serviceContext) =
        stdx::make_unique<PeriodicBalancerSettingsRefresher>(isPrimary);

    // Register a shutdown task to terminate the refresher thread.
    registerShutdownTask(
        [serviceContext] { PeriodicBalancerSettingsRefresher::get(serviceContext)->shutdown(); });
}

PeriodicBalancerSettingsRefresher* PeriodicBalancerSettingsRefresher::get(
    ServiceContext* serviceContext) {
    return getPeriodicBalancerSettingsRefresher(serviceContext).get();
}

void PeriodicBalancerSettingsRefresher::start() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(!_isPrimary);
    _isPrimary = true;
}

void PeriodicBalancerSettingsRefresher::stop() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (!_isPrimary) {
        return;
    }
    _isPrimary = false;
}

void PeriodicBalancerSettingsRefresher::shutdown() {
    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        if (_isShutdown) {
            return;
        }
        _isShutdown = true;
    }

    _thread.join();
    _thread = {};
}

void PeriodicBalancerSettingsRefresher::_periodicRefresh() {
    Client::initThread("Periodic Balancer Settings Refresher");
    auto opCtx = cc().makeOperationContext();

    while (!_shutDownRequested()) {
        if (_isPrimary) {
            auto status =
                Grid::get(opCtx.get())->getBalancerConfiguration()->refreshAndCheck(opCtx.get());
            if (!status.isOK()) {
                log() << "failed to refresh balancer settings" << causedBy(status);
            }
        }

        try {
            MONGO_IDLE_THREAD_BLOCK;
            opCtx->sleepFor(kRefreshInterval);
        } catch (DBException e) {
            log() << "Periodic Balancer Settings Refresher interrupted ::" << e.what();
        }
    }
}

bool PeriodicBalancerSettingsRefresher::_shutDownRequested() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    return _isShutdown;
}

}  // namespace mongo
