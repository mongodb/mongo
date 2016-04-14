/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/balancer/balancer_configuration.h"

#include "mongo/base/status.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

BalancerConfiguration::BalancerConfiguration(uint64_t defaultMaxChunkSizeBytes)
    : _secondaryThrottle(
          MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault)),
      _defaultMaxChunkSizeBytes(defaultMaxChunkSizeBytes) {
    invariant(checkMaxChunkSizeValid(defaultMaxChunkSizeBytes));

    _useDefaultBalancerSettings();
    _useDefaultChunkSizeSettings();
}

BalancerConfiguration::~BalancerConfiguration() = default;

bool BalancerConfiguration::isBalancerActive() const {
    if (!_shouldBalance.loadRelaxed()) {
        return false;
    }

    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (_balancerSettings.isBalancerActiveWindowSet()) {
        return _balancerSettings.inBalancingWindow(boost::posix_time::second_clock::local_time());
    }

    return true;
}

MigrationSecondaryThrottleOptions BalancerConfiguration::getSecondaryThrottle() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _secondaryThrottle;
}

bool BalancerConfiguration::waitForDelete() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _waitForDelete;
}

Status BalancerConfiguration::refreshAndCheck(OperationContext* txn) {
    // Balancer configuration
    Status balancerSettingsStatus = _refreshBalancerSettings(txn);
    if (!balancerSettingsStatus.isOK()) {
        return {balancerSettingsStatus.code(),
                str::stream() << "Failed to refresh the balancer settings due to "
                              << balancerSettingsStatus.toString()};
    }

    // Chunk size settings
    Status chunkSizeStatus = _refreshChunkSizeSettings(txn);
    if (!chunkSizeStatus.isOK()) {
        return {chunkSizeStatus.code(),
                str::stream() << "Failed to refresh the chunk sizes settings due to "
                              << chunkSizeStatus.toString()};
    }

    return Status::OK();
}

bool BalancerConfiguration::checkMaxChunkSizeValid(uint64_t maxChunkSize) {
    if (maxChunkSize >= (1024 * 1024) && maxChunkSize <= (1024 * 1024 * 1024)) {
        return true;
    }

    return false;
}

Status BalancerConfiguration::_refreshBalancerSettings(OperationContext* txn) {
    SettingsType balancerSettings;

    auto balanceSettingsStatus =
        Grid::get(txn)->catalogManager(txn)->getGlobalSettings(txn, SettingsType::BalancerDocKey);
    if (balanceSettingsStatus.isOK()) {
        auto settingsTypeStatus =
            SettingsType::fromBSON(std::move(balanceSettingsStatus.getValue()));
        if (!settingsTypeStatus.isOK()) {
            return settingsTypeStatus.getStatus();
        }
        balancerSettings = std::move(settingsTypeStatus.getValue());
    } else if (balanceSettingsStatus.getStatus() != ErrorCodes::NoMatchingDocument) {
        return balanceSettingsStatus.getStatus();
    } else {
        _useDefaultBalancerSettings();
        return Status::OK();
    }

    if (balancerSettings.isBalancerStoppedSet() && balancerSettings.getBalancerStopped()) {
        _shouldBalance.store(false);
    } else {
        _shouldBalance.store(true);
    }

    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    _balancerSettings = std::move(balancerSettings);

    if (_balancerSettings.isKeySet()) {
        _secondaryThrottle =
            uassertStatusOK(MigrationSecondaryThrottleOptions::createFromBalancerConfig(
                _balancerSettings.toBSON()));
    } else {
        _secondaryThrottle =
            MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);
    }

    if (_balancerSettings.isWaitForDeleteSet() && _balancerSettings.getWaitForDelete()) {
        _waitForDelete = true;
    } else {
        _waitForDelete = false;
    }

    return Status::OK();
}

void BalancerConfiguration::_useDefaultBalancerSettings() {
    _shouldBalance.store(true);
    _balancerSettings = SettingsType{};
    _waitForDelete = false;
    _secondaryThrottle =
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);
}

Status BalancerConfiguration::_refreshChunkSizeSettings(OperationContext* txn) {
    SettingsType chunkSizeSettings;

    auto chunkSizeSettingsStatus =
        grid.catalogManager(txn)->getGlobalSettings(txn, SettingsType::ChunkSizeDocKey);
    if (chunkSizeSettingsStatus.isOK()) {
        auto settingsTypeStatus =
            SettingsType::fromBSON(std::move(chunkSizeSettingsStatus.getValue()));
        if (!settingsTypeStatus.isOK()) {
            return settingsTypeStatus.getStatus();
        }
        chunkSizeSettings = std::move(settingsTypeStatus.getValue());
    } else if (chunkSizeSettingsStatus.getStatus() != ErrorCodes::NoMatchingDocument) {
        return chunkSizeSettingsStatus.getStatus();
    } else {
        _useDefaultChunkSizeSettings();
        return Status::OK();
    }

    const uint64_t newMaxChunkSizeBytes = chunkSizeSettings.getChunkSizeMB() * 1024 * 1024;

    if (!checkMaxChunkSizeValid(newMaxChunkSizeBytes)) {
        return {ErrorCodes::BadValue,
                str::stream() << chunkSizeSettings.getChunkSizeMB()
                              << " is not a valid value for MaxChunkSize"};
    }

    if (newMaxChunkSizeBytes != getMaxChunkSizeBytes()) {
        log() << "MaxChunkSize changing from " << getMaxChunkSizeBytes() / (1024 * 1024) << "MB"
              << " to " << newMaxChunkSizeBytes / (1024 * 1024) << "MB";
    }

    return Status::OK();
}

void BalancerConfiguration::_useDefaultChunkSizeSettings() {
    _maxChunkSizeBytes.store(_defaultMaxChunkSizeBytes);
}

}  // namespace mongo
