/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

/**
 * Test suite generator headers.
 */

#include "mongo/db/db_raii.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;
using namespace mongo::unittest;
using std::shared_ptr;

namespace mongo {

class BSONObj;
class OperationContext;
class Status;
class StringData;

namespace dbtests {

/**
 * Creates an index if it does not already exist.
 */
Status createIndex(OperationContext* opCtx,
                   StringData ns,
                   const BSONObj& keys,
                   bool unique = false);

/**
 * Creates an index from a BSON spec, if it does not already exist.
 */
Status createIndexFromSpec(OperationContext* opCtx, StringData ns, const BSONObj& spec);

/**
 * Combines AutoGetOrCreateDb and OldClientContext. If the requested 'ns' exists, the constructed
 * object will have both the database and the collection locked in MODE_IX. Otherwise, the database
 * will be locked in MODE_IX and will be created, while the collection will be locked in MODE_X, but
 * not created.
 */
class WriteContextForTests {
    WriteContextForTests(const WriteContextForTests&) = delete;
    WriteContextForTests& operator=(const WriteContextForTests&) = delete;

public:
    WriteContextForTests(OperationContext* opCtx, StringData ns);

    Database* db() const {
        return _clientContext->db();
    }

    Collection* getCollection() const {
        return CollectionCatalog::get(_opCtx).lookupCollectionByNamespace(_nss);
    }

private:
    OperationContext* const _opCtx;
    const NamespaceString _nss;

    boost::optional<AutoGetOrCreateDb> _autoCreateDb;
    boost::optional<Lock::CollectionLock> _collLock;
    boost::optional<OldClientContext> _clientContext;
};

}  // namespace dbtests
}  // namespace mongo
