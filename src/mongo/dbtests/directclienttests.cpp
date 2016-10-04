/** @file directclienttests.cpp
*/

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include <iostream>

#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace DirectClientTests {

using std::unique_ptr;
using std::vector;

class ClientBase {
public:
    ClientBase() {
        mongo::LastError::get(cc()).reset();
    }
    virtual ~ClientBase() {
        mongo::LastError::get(cc()).reset();
    }
};

const char* ns = "a.b";

class Capped : public ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);
        for (int pass = 0; pass < 3; pass++) {
            client.createCollection(ns, 1024 * 1024, true, 999);
            for (int j = 0; j < pass * 3; j++)
                client.insert(ns, BSON("x" << j));

            // test truncation of a capped collection
            if (pass) {
                BSONObj info;
                BSONObj cmd = BSON("captrunc"
                                   << "b"
                                   << "n"
                                   << 1
                                   << "inc"
                                   << true);
                // cout << cmd.toString() << endl;
                bool ok = client.runCommand("a", cmd, info);
                // cout << info.toString() << endl;
                verify(ok);
            }

            verify(client.dropCollection(ns));
        }
    }
};

class InsertMany : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        vector<BSONObj> objs;
        objs.push_back(BSON("_id" << 1));
        objs.push_back(BSON("_id" << 1));
        objs.push_back(BSON("_id" << 2));


        client.dropCollection(ns);
        client.insert(ns, objs);
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(), 11000);
        ASSERT_EQUALS((int)client.count(ns), 1);

        client.dropCollection(ns);
        client.insert(ns, objs, InsertOption_ContinueOnError);
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(), 11000);
        ASSERT_EQUALS((int)client.count(ns), 2);
    }
};

class BadNSCmd : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        BSONObj result;
        BSONObj cmdObj = BSON("count"
                              << "");
        ASSERT_THROWS(client.runCommand("", cmdObj, result), UserException);
    }
};

class BadNSQuery : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        unique_ptr<DBClientCursor> cursor = client.query("", Query(), 1);
        ASSERT(cursor->more());
        BSONObj result = cursor->next().getOwned();
        ASSERT(result.hasField("$err"));
        ASSERT_EQUALS(result["code"].Int(), ErrorCodes::InvalidNamespace);
    }
};

class BadNSGetMore : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        unique_ptr<DBClientCursor> cursor = client.getMore("", 1, 1);
        ASSERT(cursor->more());
        BSONObj result = cursor->next().getOwned();
        ASSERT(result.hasField("$err"));
        ASSERT_EQUALS(result["code"].Int(), ErrorCodes::InvalidNamespace);
    }
};

class BadNSInsert : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.insert("", BSONObj(), 0);
        ASSERT(!client.getLastError().empty());
    }
};

class BadNSUpdate : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.update("", Query(), BSON("$set" << BSON("x" << 1)));
        ASSERT(!client.getLastError().empty());
    }
};

class BadNSRemove : ClientBase {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.remove("", Query());
        ASSERT(!client.getLastError().empty());
    }
};

class All : public Suite {
public:
    All() : Suite("directclient") {}
    void setupTests() {
        add<Capped>();
        add<InsertMany>();
        add<BadNSCmd>();
        add<BadNSQuery>();
        add<BadNSGetMore>();
        add<BadNSInsert>();
        add<BadNSUpdate>();
        add<BadNSRemove>();
    }
};

SuiteInstance<All> myall;
}
