// indexcatalogtests.cpp : index_catalog.{h,cpp} unit tests.

/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 */

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context_impl.h"

#include "mongo/dbtests/dbtests.h"

namespace IndexCatalogTests {

    static const char* const _ns = "unittests.indexcatalog";

    class IndexIteratorTests {
    public:
        IndexIteratorTests() {
            OperationContextImpl txn;
            Client::WriteContext ctx(&txn, _ns);

            _db = ctx.db();
            _coll = _db->createCollection(&txn, _ns);
            _catalog = _coll->getIndexCatalog();
            ctx.commit();
        }

        ~IndexIteratorTests() {
            OperationContextImpl txn;
            Client::WriteContext ctx(&txn, _ns);

            _db->dropCollection(&txn, _ns);
            ctx.commit();
        }

        void run() {
            OperationContextImpl txn;
            Client::WriteContext ctx(&txn, _ns);

            int numFinishedIndexesStart = _catalog->numIndexesReady(&txn);

            Helpers::ensureIndex(&txn, _coll, BSON("x" << 1), false, "_x_0");
            Helpers::ensureIndex(&txn, _coll, BSON("y" << 1), false, "_y_0");

            ASSERT_TRUE(_catalog->numIndexesReady(&txn) == numFinishedIndexesStart+2);

            IndexCatalog::IndexIterator ii = _catalog->getIndexIterator(&txn,false);
            int indexesIterated = 0;
            bool foundIndex = false;
            while (ii.more()) {
                IndexDescriptor* indexDesc = ii.next();
                indexesIterated++;
                BSONObjIterator boit(indexDesc->infoObj());
                while (boit.more() && !foundIndex) {
                    BSONElement e = boit.next();
                    if (str::equals(e.fieldName(), "name") &&
                            str::equals(e.valuestrsafe(), "_y_0")) {
                        foundIndex = true;
                        break;
                    }
                }
            }

            ctx.commit();
            ASSERT_TRUE(indexesIterated == _catalog->numIndexesReady(&txn));
            ASSERT_TRUE(foundIndex);
        }

    private:
        IndexCatalog* _catalog;
        Collection* _coll;
        Database* _db;
    };

    class IndexCatalogTests : public Suite {
    public:
        IndexCatalogTests() : Suite( "indexcatalogtests" ) {
        }
        void setupTests() {
            add<IndexIteratorTests>();
        }
    } indexCatalogTests;
}
