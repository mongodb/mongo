/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include <string>

#include "mongo/db/keys_collection_cache_reader.h"
#include "mongo/util/duration.h"

namespace mongo {

class ShardingCatalogClient;

/**
 * Keeps a local cache of the keys with the ability to refresh. The refresh method also makes sure
 * that there will be valid keys available to sign the current logical time and there will be
 * another key ready after the current key expires.
 *
 * Assumptions and limitations:
 * - assumes that user does not manually update the keys collection.
 * - assumes that current process is the config primary.
 */
class KeysCollectionCacheReaderAndUpdater : public KeysCollectionCacheReader {
public:
    KeysCollectionCacheReaderAndUpdater(std::string purpose,
                                        ShardingCatalogClient* client,
                                        Seconds keyValidForInterval);
    ~KeysCollectionCacheReaderAndUpdater() = default;

    /**
     * Check if there are new documents expiresAt > latestKeyDoc.expiresAt.
     */
    StatusWith<KeysCollectionDocument> refresh(OperationContext* opCtx) override;

    StatusWith<KeysCollectionDocument> getKey(const LogicalTime& forThisTime) override;

private:
    const std::string _purpose;
    const Seconds _keyValidForInterval;

    ShardingCatalogClient* const _catalogClient;
};

}  // namespace mongo
