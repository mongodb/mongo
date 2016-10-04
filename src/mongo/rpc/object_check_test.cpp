/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <iterator>

#include "mongo/base/data_range_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace {

using namespace mongo;

TEST(DataTypeValidated, BSONValidationEnabled) {
    using std::swap;

    bool wasEnabled = serverGlobalParams.objcheck;
    const auto setValidation = [&](bool enabled) { serverGlobalParams.objcheck = enabled; };
    ON_BLOCK_EXIT(setValidation, wasEnabled);

    using std::begin;
    using std::end;

    BSONObj valid = BSON("baz"
                         << "bar"
                         << "garply"
                         << BSON("foo"
                                 << "bar"));
    char buf[1024] = {0};
    std::copy(valid.objdata(), valid.objdata() + valid.objsize(), begin(buf));
    {
        Validated<BSONObj> v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_OK(cdrc.readAndAdvance(&v));
    }

    {
        // mess up the data
        DataRangeCursor drc(begin(buf), end(buf));
        // skip past size so we don't trip any sanity checks.
        drc.advance(4);  // skip size
        while (drc.writeAndAdvance(0xFF).isOK())
            ;
    }

    {
        Validated<BSONObj> v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_NOT_OK(cdrc.readAndAdvance(&v));
    }

    {
        // disable validation
        setValidation(false);
        Validated<BSONObj> v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_OK(cdrc.readAndAdvance(&v));
    }
}
}
