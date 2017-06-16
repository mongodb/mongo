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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/logical_session_cache_factory_mongod.h"

#include "mongo/db/service_liason_mock.h"
#include "mongo/db/sessions_collection_mock.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

std::unique_ptr<SessionsCollection> makeSessionsCollection(LogicalSessionCacheServer state) {
    switch (state) {
        case LogicalSessionCacheServer::kSharded:
            // TODO SERVER-29203, replace with SessionsCollectionSharded
            return stdx::make_unique<MockSessionsCollection>(
                std::make_shared<MockSessionsCollectionImpl>());
        case LogicalSessionCacheServer::kReplicaSet:
            // TODO SERVER-29202, replace with SessionsCollectionRS
            return stdx::make_unique<MockSessionsCollection>(
                std::make_shared<MockSessionsCollectionImpl>());
        case LogicalSessionCacheServer::kStandalone:
            // TODO SERVER-29201, replace with SessionsCollectionStandalone
            return stdx::make_unique<MockSessionsCollection>(
                std::make_shared<MockSessionsCollectionImpl>());
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

std::unique_ptr<LogicalSessionCache> makeLogicalSessionCacheD(LogicalSessionCacheServer state) {
    // TODO SERVER-29198 replace with ServiceLiasonMongod
    auto liason = stdx::make_unique<MockServiceLiason>(std::make_shared<MockServiceLiasonImpl>());

    // Set up the logical session cache
    auto sessionsColl = makeSessionsCollection(state);
    return stdx::make_unique<LogicalSessionCache>(
        std::move(liason), std::move(sessionsColl), LogicalSessionCache::Options{});
}

}  // namespace mongo
