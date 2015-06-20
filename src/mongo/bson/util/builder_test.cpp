// builder_test.h

/*    Copyright 2009 10gen Inc.
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

#include "mongo/bson/util/builder.h"

namespace mongo {
TEST(Builder, String1) {
    const char* big = "eliot was here";
    StringData small(big, 5);
    ASSERT_EQUALS(small, "eliot");

    BufBuilder bb;
    bb.appendStr(small);

    ASSERT_EQUALS(0, strcmp(bb.buf(), "eliot"));
    ASSERT_EQUALS(0, strcmp("eliot", bb.buf()));
}

TEST(Builder, StringBuilderAddress) {
    const void* longPtr = reinterpret_cast<const void*>(-1);
    const void* shortPtr = reinterpret_cast<const void*>(0xDEADBEEF);
    const void* nullPtr = NULL;

    StringBuilder sb;
    sb << longPtr;

    if (sizeof(longPtr) == 8) {
        ASSERT_EQUALS("0xFFFFFFFFFFFFFFFF", sb.str());
    } else {
        ASSERT_EQUALS("0xFFFFFFFF", sb.str());
    }

    sb.reset();
    sb << shortPtr;
    ASSERT_EQUALS("0xDEADBEEF", sb.str());

    sb.reset();
    sb << nullPtr;
    ASSERT_EQUALS("0x0", sb.str());
}
}
