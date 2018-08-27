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

#include <memory>

#include "mongo/db/logical_session_id.h"
#include "mongo/db/sessions_collection_sharded.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;

/**
 * Accesses the sessions collection for config servers.
 */
class SessionsCollectionConfigServer : public SessionsCollectionSharded {
public:
    /**
    * Ensures that the sessions collection has been set up for this cluster,
    * sharded, and with the proper indexes.
    *
    * This method may safely be called multiple times.
    *
    * If there are no shards in this cluster, this method will do nothing.
    */
    Status setupSessionsCollection(OperationContext* opCtx) override;

private:
    Status _shardCollectionIfNeeded(OperationContext* opCtx);
    Status _generateIndexesIfNeeded(OperationContext* opCtx);

    stdx::mutex _mutex;
    bool _collectionSetUp{false};
};

}  // namespace mongo
