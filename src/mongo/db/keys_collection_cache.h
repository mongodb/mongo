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

#include "mongo/base/status_with.h"
#include "mongo/db/keys_collection_document.h"

namespace mongo {

class LogicalTime;
class OperationContext;

class KeysCollectionCache {
public:
    virtual ~KeysCollectionCache() = default;

    /**
     * Refreshes cache and returns the latest key seen.
     */
    virtual StatusWith<KeysCollectionDocument> refresh(OperationContext* opCtx) = 0;

    /**
     * Returns the key in the cache that has the smallest expiresAt value that is also greater than
     * the forThisTime argument.
     */
    virtual StatusWith<KeysCollectionDocument> getKey(const LogicalTime& forThisTime) = 0;

    /**
     * Returns the key in the cache that matches the keyId and expiresAt value to be no less than
     * the forThisTime argument.
     */
    virtual StatusWith<KeysCollectionDocument> getKeyById(long long keyId,
                                                          const LogicalTime& forThisTime) = 0;
};

}  // namespace mongo
