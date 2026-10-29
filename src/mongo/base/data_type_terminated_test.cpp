// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_type_terminated.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>
#include <utility>

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
    const std::string_view writes[] = {
        std::string_view("a"), std::string_view("bb"), std::string_view("ccc")};
    std::string buf(100, '\xff');
    char* const bufBegin = &*buf.begin();
    char* ptr = bufBegin;
    size_t avail = buf.size();
    std::string expected;
    for (const auto& w : writes) {
        size_t adv;
        ASSERT_OK(DataType::store(
            Terminated<'\0', std::string_view>(w), ptr, avail, &adv, ptr - bufBegin));
        ASSERT_EQ(adv, w.size() + 1);
        ptr += adv;
        avail -= adv;
        expected += std::string{w};
        expected += '\0';
    }
    ASSERT_EQUALS(expected, buf.substr(0, buf.size() - avail));
}

TEST(DataTypeTerminated, StringDataNormalLoad) {
    const std::string_view writes[] = {
        std::string_view("a"), std::string_view("bb"), std::string_view("ccc")};
    std::string buf;
    for (const auto& w : writes) {
        buf += std::string{w};
        buf += '\0';
    }
    const char* const bufBegin = &*buf.begin();
    const char* ptr = bufBegin;
    size_t avail = buf.size();

    for (const auto& w : writes) {
        size_t adv;
        auto term = Terminated<'\0', std::string_view>{};
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
    ASSERT_EQ(std::string_view(buf, 4), std::string_view(std::string{'d', 'd', 'd', '\0'}));
    ASSERT_EQUALS(advanced, 4u);  // OK must overwrite advanced
}

TEST(DataTypeTerminated, ErrorUnterminatedRead) {
    const char buf[] = {'h', 'e', 'l', 'l', 'o'};
    size_t advanced = 123;
    auto x = Terminated<'\0', std::string_view>();
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
            ASSERT_OK(buf_writer.writeAndAdvanceNoThrow(tcdr));
        }
        const auto written = std::string(static_cast<char*>(buf), buf_writer.data());
        ASSERT_EQUALS(written, serialized);
    }
    {
        auto buf_source = ConstDataRangeCursor(buf, buf + sizeof(buf));
        for (const std::string& s : parts) {
            Terminated<'\0', ConstDataRange> tcdr;
            ASSERT_OK(buf_source.readAndAdvanceNoThrow(&tcdr));
            std::string read(tcdr.value.data(), tcdr.value.data() + tcdr.value.length());
            ASSERT_EQUALS(s, read);
        }
    }
}

}  // namespace
}  // namespace mongo
