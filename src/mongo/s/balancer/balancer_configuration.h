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

#pragma once

#include <cstdint>

#include "mongo/base/disallow_copying.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class MigrationSecondaryThrottleOptions;
class OperationContext;
class Status;

/**
 * Contains settings, which control the behaviour of the balancer.
 */
class BalancerConfiguration {
    MONGO_DISALLOW_COPYING(BalancerConfiguration);

public:
    // Default value used for the max chunk size if one is not specified in the balancer
    // configuration
    static const uint64_t kDefaultMaxChunkSizeBytes{64 * 1024 * 1024};

    /**
     * Primes the balancer configuration with some default values. These settings may change at a
     * later time after a call to refresh().
     */
    BalancerConfiguration(uint64_t defaultMaxChunkSizeBytes);
    ~BalancerConfiguration();

    /**
     * Returns whether balancing is allowed based on both the enabled state of the balancer and the
     * balancing window.
     */
    bool isBalancerActive() const;

    /**
     * Returns the secondary throttle options for the balancer.
     */
    MigrationSecondaryThrottleOptions getSecondaryThrottle() const;

    /**
     * Returns whether the balancer should wait for deletion of orphaned chunk data at the end of
     * each migration.
     */
    bool waitForDelete() const;

    /**
     * Returns the max chunk size after which a chunk would be considered jumbo.
     */
    uint64_t getMaxChunkSizeBytes() const {
        return _maxChunkSizeBytes.loadRelaxed();
    }

    /**
     * Blocking method, which refreshes the balancer configuration from the settings in the
     * config.settings collection. It will stop at the first bad configuration value and return an
     * error indicating what failed.
     *
     * This method is thread-safe but it doesn't make sense to be called from more than one thread
     * at a time.
     */
    Status refreshAndCheck(OperationContext* txn);

    /**
     * Validates that the specified max chunk size value (in bytes) is allowed.
     */
    static bool checkMaxChunkSizeValid(uint64_t maxChunkSizeBytes);

private:
    /**
     * Reloads the balancer configuration from the settings document. Fails if the settings document
     * cannot be read, in which case the values will remain unchanged.
     */
    Status _refreshBalancerSettings(OperationContext* txn);

    /**
     * If the balancer settings document is missing, these are the defaults, which will be used.
     */
    void _useDefaultBalancerSettings();

    /**
     * Reloads the chunk sizes configuration from the settings document. Fails if the settings
     * document cannot be read or if any setting contains invalid value, in which case the offending
     * value will remain unchanged.
     */
    Status _refreshChunkSizeSettings(OperationContext* txn);

    /**
     * If the chunk size settings document is missing, these are the defaults, which will be used.
     */
    void _useDefaultChunkSizeSettings();

    // Whether auto-balancing of chunks should happen
    AtomicBool _shouldBalance{true};

    // The latest read balancer settings (used for the balancer window and secondary throttle) and a
    // mutex to protect its changes
    mutable stdx::mutex _balancerSettingsMutex;
    SettingsType _balancerSettings;
    bool _waitForDelete{false};
    MigrationSecondaryThrottleOptions _secondaryThrottle;

    // Default value for use for the max chunk size if the setting is not present in the balancer
    // configuration
    const uint64_t _defaultMaxChunkSizeBytes;

    // Max chunk size after which a chunk would be considered jumbo and won't be moved
    AtomicUInt64 _maxChunkSizeBytes;
};

}  // namespace mongo
