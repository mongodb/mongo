/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/assert_util.h"

using mongo::MsgAssertionException;

/**
 * Test getLastError client handling
 */
namespace {

using std::string;

static const char* const _ns = "unittests.gle";

/**
 * Verify that when the command fails we get back an error message.
 */
class GetLastErrorCommandFailure {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.insert(_ns,
                      BSON("test"
                           << "test"));

        // Cannot mix fsync + j, will make command fail
        string gleString = client.getLastError(true, true, 10, 10);
        ASSERT_NOT_EQUALS(gleString, "");
    }
};

/**
 * Verify that the write succeeds
 */
class GetLastErrorClean {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.insert(_ns,
                      BSON("test"
                           << "test"));

        // Make sure there was no error
        string gleString = client.getLastError();
        ASSERT_EQUALS(gleString, "");
    }
};

/**
 * Verify that the write succeed first, then error on dup
 */
class GetLastErrorFromDup {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.insert(_ns, BSON("_id" << 1));

        // Make sure there was no error
        string gleString = client.getLastError();
        ASSERT_EQUALS(gleString, "");

        // insert dup
        client.insert(_ns, BSON("_id" << 1));
        // Make sure there was an error
        gleString = client.getLastError();
        ASSERT_NOT_EQUALS(gleString, "");
    }
};

class All : public Suite {
public:
    All() : Suite("gle") {}

    void setupTests() {
        add<GetLastErrorClean>();
        add<GetLastErrorCommandFailure>();
        add<GetLastErrorFromDup>();
    }
};

SuiteInstance<All> myall;
}
