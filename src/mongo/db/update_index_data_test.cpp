// update_index_data_tests.cpp

/*    Copyright 2012 10gen Inc.
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

#include "mongo/unittest/unittest.h"

#include "mongo/db/update_index_data.h"

namespace mongo {

using std::string;

TEST(UpdateIndexDataTest, Simple1) {
    UpdateIndexData a;
    a.addPath("a.b");
    ASSERT_TRUE(a.mightBeIndexed("a.b"));
    ASSERT_TRUE(a.mightBeIndexed("a"));
    ASSERT_TRUE(a.mightBeIndexed("a.b.c"));
    ASSERT_TRUE(a.mightBeIndexed("a.$.b"));

    ASSERT_FALSE(a.mightBeIndexed("b"));
    ASSERT_FALSE(a.mightBeIndexed("a.c"));

    a.clear();
    ASSERT_FALSE(a.mightBeIndexed("a.b"));
}

TEST(UpdateIndexDataTest, Simple2) {
    UpdateIndexData a;
    a.addPath("ab");
    ASSERT_FALSE(a.mightBeIndexed("a"));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed("ab"));
}

TEST(UpdateIndexDataTest, Component1) {
    UpdateIndexData a;
    a.addPathComponent("a");
    ASSERT_FALSE(a.mightBeIndexed(""));
    ASSERT_TRUE(a.mightBeIndexed("a"));
    ASSERT_TRUE(a.mightBeIndexed("b.a"));
    ASSERT_TRUE(a.mightBeIndexed("a.b"));
    ASSERT_TRUE(a.mightBeIndexed("b.a.c"));
    ASSERT_FALSE(a.mightBeIndexed("b.c"));
    ASSERT_FALSE(a.mightBeIndexed("ab"));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed("a"));
}

TEST(UpdateIndexDataTest, AllPathsIndexed1) {
    UpdateIndexData a;
    a.allPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed("a"));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed("a"));
}

TEST(UpdateIndexDataTest, AllPathsIndexed2) {
    UpdateIndexData a;
    a.allPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed("a"));
    ASSERT_TRUE(a.mightBeIndexed(""));
    a.addPathComponent("a");
    ASSERT_TRUE(a.mightBeIndexed("a"));
    ASSERT_TRUE(a.mightBeIndexed("b"));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed("a"));
}

TEST(UpdateIndexDataTest, getCanonicalIndexField1) {
    string x;

    ASSERT_FALSE(getCanonicalIndexField("a", &x));
    ASSERT_FALSE(getCanonicalIndexField("aaa", &x));
    ASSERT_FALSE(getCanonicalIndexField("a.b", &x));

    ASSERT_TRUE(getCanonicalIndexField("a.$", &x));
    ASSERT_EQUALS(x, "a");
    ASSERT_TRUE(getCanonicalIndexField("a.0", &x));
    ASSERT_EQUALS(x, "a");
    ASSERT_TRUE(getCanonicalIndexField("a.123", &x));
    ASSERT_EQUALS(x, "a");

    ASSERT_TRUE(getCanonicalIndexField("a.$.b", &x));
    ASSERT_EQUALS(x, "a.b");
    ASSERT_TRUE(getCanonicalIndexField("a.0.b", &x));
    ASSERT_EQUALS(x, "a.b");
    ASSERT_TRUE(getCanonicalIndexField("a.123.b", &x));
    ASSERT_EQUALS(x, "a.b");

    ASSERT_FALSE(getCanonicalIndexField("a.$ref", &x));
    ASSERT_FALSE(getCanonicalIndexField("a.$ref.b", &x));


    ASSERT_FALSE(getCanonicalIndexField("a.c$d.b", &x));

    ASSERT_FALSE(getCanonicalIndexField("a.123a", &x));
    ASSERT_FALSE(getCanonicalIndexField("a.a123", &x));
    ASSERT_FALSE(getCanonicalIndexField("a.123a.b", &x));
    ASSERT_FALSE(getCanonicalIndexField("a.a123.b", &x));

    ASSERT_FALSE(getCanonicalIndexField("a.", &x));
}
}
