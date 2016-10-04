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

/** Unit tests for RecordId. */

#include "mongo/db/record_id.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(RecordId, HashEqual) {
    RecordId locA(1, 2);
    RecordId locB;
    locB = locA;
    ASSERT_EQUALS(locA, locB);
    RecordId::Hasher hasher;
    ASSERT_EQUALS(hasher(locA), hasher(locB));
}

TEST(RecordId, HashNotEqual) {
    RecordId original(1, 2);
    RecordId diffFile(10, 2);
    RecordId diffOfs(1, 20);
    RecordId diffBoth(10, 20);
    RecordId reversed(2, 1);
    ASSERT_NOT_EQUALS(original, diffFile);
    ASSERT_NOT_EQUALS(original, diffOfs);
    ASSERT_NOT_EQUALS(original, diffBoth);
    ASSERT_NOT_EQUALS(original, reversed);

    // Unequal DiskLocs need not produce unequal hashes.  But unequal hashes are likely, and
    // assumed here for sanity checking of the custom hash implementation.
    RecordId::Hasher hasher;
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffFile));
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffOfs));
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffBoth));
    ASSERT_NOT_EQUALS(hasher(original), hasher(reversed));
}

}  // namespace
}  // namespace mongo
