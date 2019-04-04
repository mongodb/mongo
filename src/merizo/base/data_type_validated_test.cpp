/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "merizo/base/data_type_validated.h"

#include <algorithm>
#include <iterator>

#include "merizo/base/data_range.h"
#include "merizo/base/data_range_cursor.h"
#include "merizo/base/data_type_endian.h"
#include "merizo/base/status.h"
#include "merizo/db/jsobj.h"
#include "merizo/unittest/unittest.h"

namespace merizo {
template <>
struct Validator<char> {
    static Status validateLoad(const char* ptr, size_t length) {
        if ((length >= sizeof(char)) && (ptr[0] == 0xFU)) {
            return Status::OK();
        }
        return Status(ErrorCodes::BadValue, "bad");
    }

    static Status validateStore(const char& toStore) {
        if (toStore == 0xFU) {
            return Status::OK();
        }
        return Status(ErrorCodes::BadValue, "bad");
    }
};
}  // namespace merizo

namespace {

using namespace merizo;
using std::end;
using std::begin;

TEST(DataTypeValidated, SuccessfulValidation) {
    char buf[1];

    {
        DataRangeCursor drc(begin(buf), end(buf));
        ASSERT_OK(drc.writeAndAdvance(Validated<char>(0xFU)));
    }

    {
        Validated<char> valid;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_OK(cdrc.readAndAdvance(&valid));
        ASSERT_EQUALS(valid.val, char{0xFU});
    }
}

TEST(DataTypeValidated, FailedValidation) {
    char buf[1];

    {
        DataRangeCursor drc(begin(buf), end(buf));
        ASSERT_NOT_OK(drc.writeAndAdvance(Validated<char>(0x01)));
    }

    buf[0] = char{0x01};

    {
        Validated<char> valid;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_NOT_OK(cdrc.readAndAdvance(&valid));
    }
}

}  // namespace
