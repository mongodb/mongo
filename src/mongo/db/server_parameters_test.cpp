// server_parameters_test.cpp

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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/server_parameters.h"

namespace mongo {

using std::string;
using std::vector;

TEST(ServerParameters, Simple1) {
    std::atomic<int> f(5);  // NOLINT
    ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> ff(NULL, "ff", &f);
    ASSERT_EQUALS("ff", ff.name());

    ff.set(6);
    ASSERT_EQUALS(6, f);

    ff.set(BSON("x" << 7).firstElement());
    ASSERT_EQUALS(7, f);

    ff.setFromString("8");
    ASSERT_EQUALS(8, f);
}

TEST(ServerParameters, Vector1) {
    vector<string> v;

    ExportedServerParameter<vector<string>, ServerParameterType::kStartupOnly> vv(NULL, "vv", &v);

    BSONObj x = BSON("x" << BSON_ARRAY("a"
                                       << "b"
                                       << "c"));
    vv.set(x.firstElement());

    ASSERT_EQUALS(3U, v.size());
    ASSERT_EQUALS("a", v[0]);
    ASSERT_EQUALS("b", v[1]);
    ASSERT_EQUALS("c", v[2]);

    BSONObjBuilder b;

    OperationContextNoop txn;
    vv.append(&txn, b, vv.name());

    BSONObj y = b.obj();
    ASSERT(x.firstElement().woCompare(y.firstElement(), false) == 0);


    vv.setFromString("d,e");
    ASSERT_EQUALS(2U, v.size());
    ASSERT_EQUALS("d", v[0]);
    ASSERT_EQUALS("e", v[1]);
}
}
