// index_catalog_test.cpp : index_catalog.{h,cpp} unit tests.

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

#include "mongo/base/initializer.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/structure/collection.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST( IndexCatalog, IndexIteratorTest ) {
        // Set up.
        mongo::runGlobalInitializersOrDie(0, 0, 0);
        Client::initThread("indexiteratortest");
        const char* const _ns = "unittests.indexcatalog";
        Client::WriteContext ctx(_ns);
        Database* _db = ctx.ctx().db();
        Collection* _coll = _db->createCollection(_ns);
        IndexCatalog* _catalog = _coll->getIndexCatalog();

        int numFinishedIndexesStart = _catalog->numIndexesReady();

        BSONObjBuilder b1;
        b1.append("key", BSON("x" << 1));
        b1.append("ns", _ns);
        b1.append("name", "_x_0");
        _catalog->createIndex(b1.obj(), true);

        BSONObjBuilder b2;
        b2.append("key", BSON("y" << 1));
        b2.append("ns", _ns);
        b2.append("name", "_y_0");
        _catalog->createIndex(b2.obj(), true);

        ASSERT_TRUE(_catalog->numIndexesReady() == numFinishedIndexesStart+2);

        IndexCatalog::IndexIterator ii = _catalog->getIndexIterator(false);
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

        ASSERT_TRUE(indexesIterated == _catalog->numIndexesReady());
        ASSERT_TRUE(foundIndex);

        // Clean up.
        _db->dropCollection(_ns);
    }

}
