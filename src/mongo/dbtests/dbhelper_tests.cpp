/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;
using std::set;

/**
 * Unit tests related to DBHelpers
 */

static const char* const ns = "unittests.removetests";

// TODO: Normalize with test framework
/** Simple test for Helpers::RemoveRange. */
class RemoveRange {
public:
    RemoveRange() : _min(4), _max(8) {}

    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        for (int i = 0; i < 10; ++i) {
            client.insert(ns, BSON("_id" << i));
        }

        {
            // Remove _id range [_min, _max).
            ScopedTransaction transaction(&txn, MODE_IX);
            Lock::DBLock lk(txn.lockState(), nsToDatabaseSubstring(ns), MODE_X);
            OldClientContext ctx(&txn, ns);

            KeyRange range(ns, BSON("_id" << _min), BSON("_id" << _max), BSON("_id" << 1));
            mongo::WriteConcernOptions dummyWriteConcern;
            Helpers::removeRange(&txn, range, false, dummyWriteConcern);
        }

        // Check that the expected documents remain.
        ASSERT_EQUALS(expected(), docs(&txn));
    }

private:
    BSONArray expected() const {
        BSONArrayBuilder bab;
        for (int i = 0; i < _min; ++i) {
            bab << BSON("_id" << i);
        }
        for (int i = _max; i < 10; ++i) {
            bab << BSON("_id" << i);
        }
        return bab.arr();
    }

    BSONArray docs(OperationContext* txn) const {
        DBDirectClient client(txn);
        unique_ptr<DBClientCursor> cursor = client.query(ns, Query().hint(BSON("_id" << 1)));
        BSONArrayBuilder bab;
        while (cursor->more()) {
            bab << cursor->next();
        }
        return bab.arr();
    }
    int _min;
    int _max;
};

class All : public Suite {
public:
    All() : Suite("remove") {}
    void setupTests() {
        add<RemoveRange>();
    }
} myall;

}  // namespace RemoveTests
