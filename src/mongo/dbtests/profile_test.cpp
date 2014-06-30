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

/**
 * This file contains tests for the profile command
 */

#include "mongo/db/instance.h"
#include "mongo/unittest/unittest.h"

using mongo::BSONObj;
using mongo::DBDirectClient;

using std::string;

namespace mongo_test {
    class Profiler: public mongo::unittest::Test {
    public:
        static const string PROFILER_TEST_DB;
        static const string PROFILER_TEST_NS;
        static const string PROFILE_NS;

    protected:
        void setUp() {
            BSONObj ret;
            db.runCommand(PROFILER_TEST_DB, BSON("dropDatabase" << 1), ret);
            ASSERT(ret["ok"].trueValue());
        }

        DBDirectClient db;
    };

    const string Profiler::PROFILER_TEST_DB = "profilerTestDB";
    const string Profiler::PROFILER_TEST_NS = Profiler::PROFILER_TEST_DB + ".test";
    const string Profiler::PROFILE_NS = Profiler::PROFILER_TEST_DB + ".system.profile";

    TEST_F(Profiler, BigDoc) {
        // Test that update with large document with a long string can be
        // be profiled in a shortened version
        const string bigStr(16 * (1 << 20) - 200, 'a');

        {
            BSONObj replyObj;
            db.runCommand(PROFILER_TEST_DB, BSON("profile" << 2), replyObj);
            ASSERT(replyObj["ok"].trueValue());
        }

        const BSONObj doc(BSON("field" << bigStr));
        db.update(PROFILER_TEST_NS, doc, doc);

        std::auto_ptr<mongo::DBClientCursor> cursor = db.query(PROFILE_NS, BSONObj());
        ASSERT(cursor->more());

        BSONObj profileDoc(cursor->next());
        ASSERT(profileDoc.hasField("abbreviated"));
        const string abbreviatedField(profileDoc["abbreviated"].str());

        // Make sure that the abbreviated field contains the query and the updateobj info
        ASSERT(abbreviatedField.find("query:") != string::npos);
        ASSERT(abbreviatedField.find("updateobj:") != string::npos);
    }

    TEST_F(Profiler, BigDocWithManyFields) {
        // Test that update with large document with lots of fields can be
        // be profiled in a shortened version
        mongo::BSONObjBuilder builder;

        for (int x = 0; x < (1 << 20); x++) {
            const string fieldName(mongo::str::stream() << "x" << x);
            builder.append(fieldName, x);
        }

        DBDirectClient db;

        {
            BSONObj replyObj;
            db.runCommand(PROFILER_TEST_DB, BSON("profile" << 2), replyObj);
            ASSERT(replyObj["ok"].trueValue());
        }

        const BSONObj doc(builder.done());
        db.update(PROFILER_TEST_NS, doc, doc);

        std::auto_ptr<mongo::DBClientCursor> cursor = db.query(PROFILE_NS, BSONObj());
        ASSERT(cursor->more());

        BSONObj profileDoc(cursor->next());
        ASSERT(profileDoc.hasField("abbreviated"));
        const string abbreviatedField(profileDoc["abbreviated"].str());

        // Make sure that the abbreviated field contains the query and the updateobj info
        ASSERT(abbreviatedField.find("query:") != string::npos);
        ASSERT(abbreviatedField.find("updateobj:") != string::npos);
    }
}

