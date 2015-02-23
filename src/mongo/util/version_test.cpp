/*    Copyright 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version.h"

namespace {

    using mongo::toVersionArray;

    TEST(VersionTest, NormalCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3"), BSON_ARRAY(1 << 2 << 3 << 0) );
        ASSERT_EQUALS( toVersionArray("1.2.0"), BSON_ARRAY(1 << 2 << 0 << 0) );
        ASSERT_EQUALS( toVersionArray("2.0.0"), BSON_ARRAY(2 << 0 << 0 << 0) );
    }

    TEST(VersionTest, PreCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3-pre-"), BSON_ARRAY(1 << 2 << 3 << -100) );
        ASSERT_EQUALS( toVersionArray("1.2.0-pre-"), BSON_ARRAY(1 << 2 << 0 << -100) );
        ASSERT_EQUALS( toVersionArray("2.0.0-pre-"), BSON_ARRAY(2 << 0 << 0 << -100) );
    }

    TEST(VersionTest, RcCase) {
        ASSERT_EQUALS( toVersionArray("1.2.3-rc0"), BSON_ARRAY(1 << 2 << 3 << -50) );
        ASSERT_EQUALS( toVersionArray("1.2.0-rc1"), BSON_ARRAY(1 << 2 << 0 << -49) );
        ASSERT_EQUALS( toVersionArray("2.0.0-rc2"), BSON_ARRAY(2 << 0 << 0 << -48) );
    }

    TEST(VersionTest, RcPreCase) {
        // Note that the pre of an rc is the same as the rc itself
        ASSERT_EQUALS( toVersionArray("1.2.3-rc3-pre-"), BSON_ARRAY(1 << 2 << 3 << -47) );
        ASSERT_EQUALS( toVersionArray("1.2.0-rc4-pre-"), BSON_ARRAY(1 << 2 << 0 << -46) );
        ASSERT_EQUALS( toVersionArray("2.0.0-rc5-pre-"), BSON_ARRAY(2 << 0 << 0 << -45) );
    }

} // namespace
