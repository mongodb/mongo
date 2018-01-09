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

#include "mongo/base/data_type_terminated.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stringutils.h"
#include <string>

namespace mongo {
namespace {

// For testing purposes, a type that has a fixed load and store size, and some
// arbitrary serialization format of 'd' repeated N times.
template <size_t N>
struct Dummy {
    static constexpr size_t extent = N;
};
}  // namespace
// Pop the anonymous namespace to specialize mongo::DataType::Handler.
// Template specialization is a drag.

template <size_t N>
struct DataType::Handler<Dummy<N>> {
    using handledType = Dummy<N>;
    static constexpr size_t extent = handledType::extent;

    static Status load(handledType* sdata,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        if (length < extent) {
            return Status(ErrorCodes::Overflow, "too short for Dummy");
        }
        for (size_t i = 0; i < extent; ++i) {
            if (*ptr++ != 'd') {
                return Status(ErrorCodes::Overflow, "load of invalid Dummy object");
            }
        }
        *advanced = extent;
        return Status::OK();
    }

    static Status store(const handledType& sdata,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        if (length < extent) {
            return Status(ErrorCodes::Overflow, "insufficient space for Dummy");
        }
        for (size_t i = 0; i < extent; ++i) {
            *ptr++ = 'd';
        }
        *advanced = extent;
        return Status::OK();
    }

    static handledType defaultConstruct() {
        return {};
    }
};

// Re-push the anonymous namespace.
namespace {

/**
 * Tests specifically for Terminated, unrelated to the DataRange
 * or DataRangeCursor classes that call it.
 */

TEST(DataTypeTerminated, StringDataNormalStore) {
    const StringData writes[] = {StringData("a"), StringData("bb"), StringData("ccc")};
    std::string buf(100, '\xff');
    char* const bufBegin = &*buf.begin();
    char* ptr = bufBegin;
    size_t avail = buf.size();
    std::string expected;
    for (const auto& w : writes) {
        size_t adv;
        ASSERT_OK(
            DataType::store(Terminated<'\0', StringData>(w), ptr, avail, &adv, ptr - bufBegin));
        ASSERT_EQ(adv, w.size() + 1);
        ptr += adv;
        avail -= adv;
        expected += w.toString();
        expected += '\0';
    }
    ASSERT_EQUALS(expected, buf.substr(0, buf.size() - avail));
}

TEST(DataTypeTerminated, StringDataNormalLoad) {
    const StringData writes[] = {StringData("a"), StringData("bb"), StringData("ccc")};
    std::string buf;
    for (const auto& w : writes) {
        buf += w.toString();
        buf += '\0';
    }
    const char* const bufBegin = &*buf.begin();
    const char* ptr = bufBegin;
    size_t avail = buf.size();

    for (const auto& w : writes) {
        size_t adv;
        auto term = Terminated<'\0', StringData>{};
        ASSERT_OK(DataType::load(&term, ptr, avail, &adv, ptr - bufBegin));
        ASSERT_EQ(adv, term.value.size() + 1);
        ptr += adv;
        avail -= adv;
        ASSERT_EQUALS(term.value, w);
    }
}

TEST(DataTypeTerminated, LoadStatusOkPropagation) {
    // Test that the nested type's .load complaints are surfaced.
    const char buf[] = {'d', 'd', 'd', '\0'};
    size_t advanced = 123;
    auto x = Terminated<'\0', Dummy<3>>();
    Status s = DataType::load(&x, buf, sizeof(buf), &advanced, 0);
    ASSERT_OK(s);
    ASSERT_EQUALS(advanced, 4u);  // OK must overwrite advanced
}

TEST(DataTypeTerminated, StoreStatusOkAdvanced) {
    // Test that an OK .store sets proper 'advanced'.
    char buf[4] = {};
    size_t advanced = 123;  // should be overwritten
    Status s = DataType::store(Terminated<'\0', Dummy<3>>(), buf, sizeof(buf), &advanced, 0);
    ASSERT_OK(s);
    ASSERT_EQ(StringData(buf, 4), StringData(std::string{'d', 'd', 'd', '\0'}));
    ASSERT_EQUALS(advanced, 4u);  // OK must overwrite advanced
}

TEST(DataTypeTerminated, ErrorUnterminatedRead) {
    const char buf[] = {'h', 'e', 'l', 'l', 'o'};
    size_t advanced = 123;
    auto x = Terminated<'\0', StringData>();
    Status s = DataType::load(&x, buf, sizeof(buf), &advanced, 0);
    ASSERT_EQ(s.codeString(), "Overflow");
    ASSERT_STRING_CONTAINS(s.reason(), "couldn't locate");
    ASSERT_STRING_CONTAINS(s.reason(), "terminal char (\\u0000)");
    ASSERT_EQUALS(advanced, 123u);  // fails must not overwrite advanced
}

TEST(DataTypeTerminated, LoadStatusPropagation) {
    // Test that the nested type's .load complaints are surfaced.
    const char buf[] = {'d', 'd', '\0'};
    size_t advanced = 123;
    auto x = Terminated<'\0', Dummy<3>>();
    Status s = DataType::load(&x, buf, sizeof(buf), &advanced, 0);
    ASSERT_EQ(s.codeString(), "Overflow");
    ASSERT_STRING_CONTAINS(s.reason(), "too short for Dummy");
    // ASSERT_STRING_CONTAINS(s.reason(), "terminal char (\\u0000)");
    ASSERT_EQUALS(advanced, 123u);  // fails must not overwrite advanced
}

TEST(DataTypeTerminated, StoreStatusPropagation) {
    // Test that the nested type's .store complaints are surfaced.
    char in[2];  // Not big enough to hold a Dummy<3>.
    size_t advanced = 123;
    Status s = DataType::store(Terminated<'\0', Dummy<3>>(), in, sizeof(in), &advanced, 0);
    ASSERT_EQ(s.codeString(), "Overflow");
    ASSERT_STRING_CONTAINS(s.reason(), "insufficient space for Dummy");
    ASSERT_EQUALS(advanced, 123u);  // fails must not overwrite advanced
}

TEST(DataTypeTerminated, ErrorShortRead) {
    // The span before the '\0' is passed to Dummy<3>'s load.
    // This consumes only the first 3 bytes, so Terminated complains
    // about the unconsumed 'X'.
    const char buf[] = {'d', 'd', 'd', 'X', '\0'};
    size_t advanced = 123;
    auto x = Terminated<'\0', Dummy<3>>();
    Status s = DataType::load(&x, buf, sizeof(buf), &advanced, 0);
    ASSERT_EQ(s.codeString(), "Overflow");
    ASSERT_STRING_CONTAINS(s.reason(), "only read");
    ASSERT_STRING_CONTAINS(s.reason(), "terminal char (\\u0000)");
    ASSERT_EQUALS(advanced, 123u);  // fails must not overwrite advanced
}

TEST(DataTypeTerminated, ErrorShortWrite) {
    char in[3] = {};
    auto x = Terminated<'\0', Dummy<3>>();
    size_t advanced = 123;
    Status s = DataType::store(x, in, sizeof(in), &advanced, 0);
    ASSERT_EQ(s.codeString(), "Overflow");
    ASSERT_STRING_CONTAINS(s.reason(), "couldn't write");
    ASSERT_STRING_CONTAINS(s.reason(), "terminal char (\\u0000)");
    ASSERT_EQUALS(advanced, 123u);  // fails must not overwrite advanced
}

TEST(DataTypeTerminated, ThroughDataRangeCursor) {
    char buf[100];
    const std::string parts[] = {"a", "bb", "ccc"};
    std::string serialized;
    for (const std::string& s : parts) {
        serialized += s + '\0';
    }
    {
        auto buf_writer = DataRangeCursor(buf, buf + sizeof(buf));
        for (const std::string& s : parts) {
            Terminated<'\0', ConstDataRange> tcdr(ConstDataRange(s.data(), s.data() + s.size()));
            ASSERT_OK(buf_writer.writeAndAdvance(tcdr));
        }
        const auto written = std::string(static_cast<const char*>(buf), buf_writer.data());
        ASSERT_EQUALS(written, serialized);
    }
    {
        auto buf_source = ConstDataRangeCursor(buf, buf + sizeof(buf));
        for (const std::string& s : parts) {
            Terminated<'\0', ConstDataRange> tcdr;
            ASSERT_OK(buf_source.readAndAdvance(&tcdr));
            std::string read(tcdr.value.data(), tcdr.value.data() + tcdr.value.length());
            ASSERT_EQUALS(s, read);
        }
    }
}

}  // namespace
}  // namespace mongo
