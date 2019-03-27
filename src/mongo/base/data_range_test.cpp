/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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

#include "mongo/base/data_range.h"

#include <cstring>

#include "mongo/base/data_type_endian.h"
#include "mongo/platform/endian.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(DataRange, ConstDataRange) {
    unsigned char buf[sizeof(uint32_t) * 3];
    uint32_t native = 1234;
    uint32_t le = endian::nativeToLittle(native);
    uint32_t be = endian::nativeToBig(native);

    std::memcpy(buf, &native, sizeof(uint32_t));
    std::memcpy(buf + sizeof(uint32_t), &le, sizeof(uint32_t));
    std::memcpy(buf + sizeof(uint32_t) * 2, &be, sizeof(uint32_t));

    ConstDataRange cdv(buf, buf + sizeof(buf));

    ASSERT_EQUALS(native, cdv.read<uint32_t>());
    ASSERT_EQUALS(native, cdv.read<LittleEndian<uint32_t>>(sizeof(uint32_t)));
    ASSERT_EQUALS(native, cdv.read<BigEndian<uint32_t>>(sizeof(uint32_t) * 2));

    auto result = cdv.readNoThrow<uint32_t>(sizeof(uint32_t) * 3);
    ASSERT_EQUALS(false, result.isOK());
    ASSERT_EQUALS(ErrorCodes::Overflow, result.getStatus().code());
}

TEST(DataRange, ConstDataRangeType) {
    char buf[] = "foo";

    ConstDataRange cdr(buf, buf + sizeof(buf));

    ConstDataRange out(nullptr, nullptr);

    auto inner = cdr.readIntoNoThrow(&out);

    ASSERT_OK(inner);
    ASSERT_EQUALS(buf, out.data());
}

TEST(DataRange, DataRange) {
    uint8_t buf[sizeof(uint32_t) * 3];
    uint32_t native = 1234;

    DataRange dv(buf, buf + sizeof(buf));

    ASSERT_OK(dv.writeNoThrow(native));
    ASSERT_OK(dv.writeNoThrow(LittleEndian<uint32_t>(native), sizeof(uint32_t)));
    ASSERT_OK(dv.writeNoThrow(BigEndian<uint32_t>(native), sizeof(uint32_t) * 2));

    auto result = dv.writeNoThrow(native, sizeof(uint32_t) * 3);
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::Overflow, result.code());

    ASSERT_EQUALS(native, dv.read<uint32_t>());
    ASSERT_EQUALS(native, dv.read<LittleEndian<uint32_t>>(sizeof(uint32_t)));
    ASSERT_EQUALS(native, dv.read<BigEndian<uint32_t>>(sizeof(uint32_t) * 2));

    ASSERT_NOT_OK(dv.readNoThrow<uint32_t>(sizeof(uint32_t) * 3));
}

TEST(DataRange, DataRangeType) {
    char buf[] = "foo";
    char buf2[] = "barZ";

    DataRange dr(buf, buf + sizeof(buf) + -1);

    DataRange out(nullptr, nullptr);

    Status status = dr.readIntoNoThrow(&out);

    ASSERT_OK(status);
    ASSERT_EQUALS(buf, out.data());

    dr = DataRange(buf2, buf2 + sizeof(buf2) + -1);
    status = dr.writeNoThrow(out);

    ASSERT_OK(status);
    ASSERT_EQUALS(std::string("fooZ"), buf2);
}

TEST(DataRange, InitFromContainer) {
    std::vector<uint8_t> vec(20);
    ConstDataRange dr(vec);
    DataRange out(nullptr, nullptr);

    ASSERT_OK(dr.readIntoNoThrow(&out));

    DataRange mutableDr(vec);
    ASSERT_OK(mutableDr.writeNoThrow<int>(6));

    std::array<char, 5> array;
    DataRange arrDR(array);
    auto status = arrDR.writeNoThrow<uint64_t>(std::numeric_limits<uint64_t>::max());
    ASSERT_EQUALS(status, ErrorCodes::Overflow);
}

}  // namespace mongo
