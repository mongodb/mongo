/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <set>
#include <string>

#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
/**
 * This class is for use with non-MMAPv1 storage engines that track record store sizes in catalog
 * metadata.
 *
 * During normal server operation, we adjust the size metadata for all record stores. But when
 * performing replication recovery, we avoid doing so, as we trust that the size metadata on disk is
 * already correct with respect to the end state of recovery.
 *
 * However, there may be exceptions that require the server to adjust size metadata even during
 * recovery. One such case is the oplog: during rollback, the oplog is truncated, and then recovery
 * occurs using oplog entries after the common point from the sync source. The server will need to
 * adjust the size metadata for the oplog collection to ensure that the count of oplog entries is
 * correct after rollback recovery.
 *
 * This class is responsible for keeping track of idents that require this special
 * count adjustment.
 */
class SizeRecoveryState {
public:
    /**
     * If replication recovery is ongoing, returns false unless 'ident' has been specifically marked
     * as requiring adjustment even during recovery.
     *
     * If the system is not currently undergoing replication recovery, always returns true.
     */
    bool collectionNeedsSizeAdjustment(const std::string& ident) const;

    /**
     * Returns whether 'ident' has been specifically marked as requiring adjustment even during
     * recovery.
     */
    bool collectionAlwaysNeedsSizeAdjustment(const std::string& ident) const;

    /**
     * Mark 'ident' as always requiring size adjustment, even if replication recovery is ongoing.
     */
    void markCollectionAsAlwaysNeedsSizeAdjustment(const std::string& ident);

    /**
     * Clears all internal state. This method should be called before calling 'recover to a stable
     * timestamp'.
     */
    void clearStateBeforeRecovery();

private:
    mutable stdx::mutex _mutex;
    std::set<std::string> _collectionsAlwaysNeedingSizeAdjustment;
};

/**
 * Returns a mutable reference to the single SizeRecoveryState associated with 'serviceCtx'.
 */
SizeRecoveryState& sizeRecoveryState(ServiceContext* serviceCtx);

/**
 * Returns a mutable reference to a boolean decoration on 'serviceCtx', which indicates whether or
 * not the server is currently undergoing replication recovery.
 */
bool& inReplicationRecovery(ServiceContext* serviceCtx);
}  // namespace mongo
