/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/s/dist_lock_manager.h"

/**
 * Helper methods for handling distributed locks that are backed by replica set config servers.
 */

namespace mongo {

    class CatalogManager;

namespace dist_lock_logic {

    /**
     * Tries to acquire the distributed lock with the given name and returns the
     * handle of the newly acquired lock on success.
     */
    StatusWith<DistLockHandle> lock(CatalogManager* lockCatalogue,
                                    const std::string& name,
                                    const std::string& whyMessage) BOOST_NOEXCEPT;

    /**
     * Unlocks the distributed lock with the given lock handle. Returns true on success.
     */
    bool unlock(CatalogManager* lockCatalogue,
                const DistLockHandle& lockHandle) BOOST_NOEXCEPT;

} // namespace dist_lock_logic
} // namespace mongo
