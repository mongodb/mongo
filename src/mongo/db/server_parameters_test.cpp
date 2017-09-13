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

namespace {
auto const getStr = [](ServerParameter& sp) {
    BSONObjBuilder b;
    sp.append(nullptr, b, "x");
    return b.obj().firstElement().String();
};
auto const getInt = [](ServerParameter& sp) {
    BSONObjBuilder b;
    sp.append(nullptr, b, "x");
    return b.obj().firstElement().Int();
};
auto const getBool = [](ServerParameter& sp) {
    BSONObjBuilder b;
    sp.append(nullptr, b, "x");
    return b.obj().firstElement().Bool();
};

using std::string;
using std::vector;

TEST(ServerParameters, boundInt) {
    int ival = 123;
    BoundServerParameter<int> bspi("bspl",
                                   [&ival](const int& v) {
                                       ival = v;
                                       return Status::OK();
                                   },
                                   [&ival] { return ival; },
                                   ServerParameterType::kStartupOnly);
    ASSERT_EQUALS(123, getInt(bspi));

    const struct {
        std::string setval;
        int expval;
        bool succeed;
    } setl[] = {
        {"234", 234, true},
        {"5.5", -1, false},
        {"345", 345, true},
        {"flowers", -1, false},
        {"  456", -1, false},
        {" 567 ", -1, false},
        {"678  ", -1, false},
        {"789-0", -1, false},
    };
    for (auto const& p : setl) {
        ASSERT_EQ(bspi.setFromString(p.setval).isOK(), p.succeed);
        if (p.succeed) {
            ASSERT_EQUALS(p.expval, ival);
            ASSERT_EQUALS(p.expval, getInt(bspi));
        }
    }
}

TEST(ServerParameter, boundBool) {
    bool bval = true;
    BoundServerParameter<bool> bspb("bspb",
                                    [&bval](const bool& v) {
                                        bval = v;
                                        return Status::OK();
                                    },
                                    [&bval] { return bval; },
                                    ServerParameterType::kStartupOnly);
    ASSERT_TRUE(getBool(bspb));

    struct {
        std::string setval;
        bool expval;
        bool succeed;
    } setb[] = {
        {"1", true, true},
        {"0", false, true},
        {"true", true, true},
        {"false", false, true},

        {"yes", false, false},
        {"no", false, false},
        {"", false, false},
        {"-1", false, false},
    };
    for (auto const& p : setb) {
        ASSERT_EQ(bspb.setFromString(p.setval).isOK(), p.succeed);
        if (p.succeed) {
            ASSERT_EQUALS(p.expval, bval);
            ASSERT_EQUALS(p.expval, getBool(bspb));
        }
    }
}

TEST(ServerParameters, boundStringExplicitLock) {
    stdx::mutex mut;
    std::string value("initial");
    BoundServerParameter<std::string> bspsel("bsp",
                                             [&value, &mut](const std::string& v) {
                                                 stdx::unique_lock<stdx::mutex> lk(mut);
                                                 value = v;
                                                 return Status::OK();
                                             },
                                             [&value, &mut] {
                                                 stdx::unique_lock<stdx::mutex> lk(mut);
                                                 return value;
                                             });

    ASSERT_EQUALS("initial", getStr(bspsel));

    const std::string sets[] = {"first-set", "second-set", "third-set"};
    for (auto const& p : sets) {
        ASSERT_TRUE(bspsel.set(BSON("x" << p).firstElement()).isOK());
        ASSERT_EQUALS(p, getStr(bspsel));
    }
}

TEST(ServerParameters, boundIntLock) {
    LockedServerParameter<int> bspi("lsp", 1234);
    ASSERT_EQUALS(1234, getInt(bspi));
    ASSERT_EQUALS(1234, bspi.getLocked());

    std::ostringstream maxint;
    maxint << std::numeric_limits<int>::max();
    std::ostringstream lowint;
    lowint << (std::numeric_limits<int>::lowest() + 1);

    std::ostringstream toobig;
    toobig << std::numeric_limits<int>::max() << "0";
    std::ostringstream toosmall;
    toosmall << std::numeric_limits<int>::lowest() << "0";

    const struct {
        std::string setstr;
        int setint;
        bool succeed;
    } sets[] = {
        {"5678", 5678, true},
        {"67", 67, true},
        {maxint.str(), std::numeric_limits<int>::max(), true},
        {lowint.str(), std::numeric_limits<int>::lowest() + 1, true},
        {toobig.str(), -1, false},
        {toosmall.str(), -1, false},
        {"flowers", -1, false},
        {"123.456", -1, false},
        {"123-456", -1, false},
        {"  123", -1, false},
        {" 123 ", -1, false},
        {"123  ", -1, false},
    };
    for (auto const& p : sets) {
        ASSERT_EQ(bspi.setFromString(p.setstr).isOK(), p.succeed);
        if (p.succeed) {
            ASSERT_EQUALS(p.setint, getInt(bspi));
            ASSERT_EQUALS(p.setint, bspi.getLocked());
        }
    }

    const int seti[] = {
        -1, 0, 1, std::numeric_limits<int>::lowest() + 1, std::numeric_limits<int>::max()};
    for (auto const& p : seti) {
        ASSERT_TRUE(bspi.setLocked(p).isOK());
        ASSERT_EQUALS(p, getInt(bspi));
        ASSERT_EQUALS(p, bspi.getLocked());
    }
}

TEST(ServerParameters, Simple1) {
    AtomicInt32 f(5);
    ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> ff(NULL, "ff", &f);
    ASSERT_EQUALS("ff", ff.name());

    ASSERT_TRUE(ff.set(6).isOK());
    ASSERT_EQUALS(6, f.load());

    ASSERT_TRUE(ff.set(BSON("x" << 7).firstElement()).isOK());
    ASSERT_EQUALS(7, f.load());

    ASSERT_TRUE(ff.setFromString("8").isOK());
    ASSERT_EQUALS(8, f.load());
}

TEST(ServerParameters, Vector1) {
    vector<string> v;

    ExportedServerParameter<vector<string>, ServerParameterType::kStartupOnly> vv(NULL, "vv", &v);

    BSONObj x = BSON("x" << BSON_ARRAY("a"
                                       << "b"
                                       << "c"));
    ASSERT_TRUE(vv.set(x.firstElement()).isOK());

    ASSERT_EQUALS(3U, v.size());
    ASSERT_EQUALS("a", v[0]);
    ASSERT_EQUALS("b", v[1]);
    ASSERT_EQUALS("c", v[2]);

    BSONObjBuilder b;

    OperationContextNoop opCtx;
    vv.append(&opCtx, b, vv.name());

    BSONObj y = b.obj();
    ASSERT(x.firstElement().woCompare(y.firstElement(), false) == 0);


    ASSERT_TRUE(vv.setFromString("d,e").isOK());
    ASSERT_EQUALS(2U, v.size());
    ASSERT_EQUALS("d", v[0]);
    ASSERT_EQUALS("e", v[1]);
}

}  // namespace
}  // namespace mongo
