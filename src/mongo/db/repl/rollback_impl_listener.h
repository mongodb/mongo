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

#pragma once

#include "mongo/db/repl/rollback_common_point_resolver.h"
#include "mongo/db/repl/rollback_impl.h"

namespace mongo {
namespace repl {

/**
 * This class implements the RollbackCommonPointResolver::Listener interface. It is owned by
 * RollbackImpl and used by RollbackCommonPointResolver to notify RollbackImpl of oplog entries
 * on the local and remote oplogs as we look for the common point between both oplogs.
 *
 * As we scan the local oplog backwards, each oplog entry is validated (to see if it can be rolled
 * back by RollbackImpl) and processed by RollbackFixUpInfo (to eventually revert the operation
 * described in the oplog entry).
 *
 * For oplog entries on the remote oplog, we validate each oplog entry to ensure it can be handled
 * by RollbackImpl.
 *
 * When we encounter the common point between the local and remote oplogs, the oplog entry
 * corresponding to the common point is recorded by RollbackFixUpInfo.
 *
 * If a scanned operation cannot be rolled back (for local oplog) or rolled forward (for remote
 * oplog), there are two possible outcomes:
 *
 * 1)  If the operation is not recognized (for example, we downgraded and this operation is from a
 *     more recent version of the server), we immediately shut down with a fatal error and inform
 *     the user that a full resync of this node is required.
 *
 * 2)  For certain 3.4 operations that RollbackFixUpInfo is not designed to handle, we return a
 *     IncompatibleRollbackAlgorithm error code to indicate that we have to call rollback() (in
 *     rs_rollback.cpp) to complete the rollback procedure.
 *     TODO: This fallback behaviour can be removed in 3.8.
 */
class RollbackImpl::Listener : public RollbackCommonPointResolver::Listener {
public:
    Status onLocalOplogEntry(const BSONObj& oplogEntryObj) override;
    Status onRemoteOplogEntry(const BSONObj& oplogEntryObj) override;
    Status onCommonPoint(
        const RollbackCommonPointResolver::RollbackCommonPoint& oplogEntryObj) override;
};

}  // namespace repl
}  // namespace mongo
