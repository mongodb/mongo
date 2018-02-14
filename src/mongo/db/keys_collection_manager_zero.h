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

#include <memory>
#include <string>

#include "mongo/db/keys_collection_document.h"
#include "mongo/db/keys_collection_manager.h"

namespace mongo {

class OperationContext;
class LogicalTime;
class ServiceContext;

/**
 * This implementation of the KeysCollectionManager always returns a zeroed key.  This is a
 * transitional type to bridge the gap until we decide how to store and rotate keys in standalones
 * and non-clustered replica sets.
 */
class KeysCollectionManagerZero : public KeysCollectionManager {
public:
    KeysCollectionManagerZero(std::string purpose);

    /**
     * Return a key that is valid for the given time and also matches the keyId.
     */
    StatusWith<KeysCollectionDocument> getKeyForValidation(OperationContext* opCtx,
                                                           long long keyId,
                                                           const LogicalTime& forThisTime) override;

    /**
     * Returns a key that is valid for the given time.
     */
    StatusWith<KeysCollectionDocument> getKeyForSigning(OperationContext* opCtx,
                                                        const LogicalTime& forThisTime) override;

    void clearCache() override;

private:
    std::string _purpose;
};

}  // namespace mongo
