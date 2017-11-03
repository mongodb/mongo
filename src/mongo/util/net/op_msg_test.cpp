/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <type_traits>

#include "mongo/base/static_assert.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {
namespace {

// Makes a SharedBuffer out of arguments passed to constructor.
class Bytes {
public:
    template <typename... T>
    explicit Bytes(T&&... args) {
        append(args...);
    }

protected:
    void append() {}  // no-op base case

    template <typename T, typename... Rest>
    std::enable_if_t<std::is_integral<T>::value> append(T arg, Rest&&... rest) {
        // Make sure BufBuilder has a real overload of this exact type and it isn't implicitly
        // converted.
        (void)static_cast<void (BufBuilder::*)(T)>(&BufBuilder::appendNum);

        buffer.appendNum(arg);  // automatically little endian.
        append(rest...);
    }

    template <typename... Rest>
    void append(const BSONObj& arg, Rest&&... rest) {
        arg.appendSelfToBufBuilder(buffer);
        append(rest...);
    }

    template <typename... Rest>
    void append(const Bytes& arg, Rest&&... rest) {
        buffer.appendBuf(arg.buffer.buf(), arg.buffer.len());
        append(rest...);
    }

    template <typename... Rest>
    void append(StringData arg, Rest&&... rest) {
        buffer.appendStr(arg, /* null terminate*/ true);
        append(rest...);
    }

    BufBuilder buffer;
};

// A Bytes that puts the size of the buffer at the front as a little-endian int32
class Sized : public Bytes {
public:
    template <typename... T>
    explicit Sized(T&&... args) {
        buffer.skip(sizeof(int32_t));
        append(args...);
        DataView(buffer.buf()).write<LittleEndian<int32_t>>(buffer.len());
    }

    // Adds extra to the stored size. Use this to produce illegal messages.
    Sized&& addToSize(int32_t extra) && {
        DataView(buffer.buf()).write<LittleEndian<int32_t>>(buffer.len() + extra);
        return std::move(*this);
    }
};

// A Bytes that puts the standard message header at the front.
class OpMsgBytes : public Sized {
public:
    template <typename... T>
    explicit OpMsgBytes(T&&... args)
        : Sized{int32_t{1},      // requestId
                int32_t{2},      // replyId
                int32_t{dbMsg},  // opCode
                args...} {}

    Message done() {
        const auto orig = Message(buffer.release());
        // Copy the message to an exact-sized allocation so ASAN can detect out-of-bounds accesses.
        auto copy = SharedBuffer::allocate(orig.size());
        memcpy(copy.get(), orig.buf(), orig.size());
        return Message(std::move(copy));
    }

    OpMsg parse() {
        return OpMsg::parseOwned(done());
    }

    OpMsgBytes&& addToSize(int32_t extra) && {
        DataView(buffer.buf()).write<LittleEndian<int32_t>>(buffer.len() + extra);
        return std::move(*this);
    }
};

// Fixture class to raise log verbosity so that invalid messages are printed by the parser.
class OpMsgParser : public unittest::Test {
public:
    void setUp() override {
        _original =
            logger::globalLogDomain()->getMinimumLogSeverity(logger::LogComponent::kNetwork);
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kNetwork,
                                                            logger::LogSeverity::Debug(1));
    }
    void tearDown() override {
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kNetwork,
                                                            _original);
    }

private:
    logger::LogSeverity _original = logger::LogSeverity::Debug(0);
};

// Section bytes
const char kBodySection = 0;
const char kDocSequenceSection = 1;

// Flags
const uint32_t kNoFlags = 0;
const uint32_t kHaveChecksum = 1;

// CRC filler value
const uint32_t kFakeCRC = 0;  // TODO will need to compute real crc when SERVER-28679 is done.

TEST_F(OpMsgParser, SucceedsWithJustBody) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 0u);
}

TEST_F(OpMsgParser, IgnoresCrcIfPresent) {  // Until SERVER-28679 is done.
    auto msg = OpMsgBytes{
        kHaveChecksum,  //
        kBodySection,
        fromjson("{ping: 1}"),
        kFakeCRC,  // If not ignored, this would be read as a second body.
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 0u);
}

TEST_F(OpMsgParser, SucceedsWithBodyThenSequence) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
            fromjson("{a: 2}"),
        },
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 1u);
    ASSERT_EQ(msg.sequences[0].name, "docs");
    ASSERT_EQ(msg.sequences[0].objs.size(), 2u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{a: 1}"));
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[1], fromjson("{a: 2}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenBody) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },

        kBodySection,
        fromjson("{ping: 1}"),
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 1u);
    ASSERT_EQ(msg.sequences[0].name, "docs");
    ASSERT_EQ(msg.sequences[0].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenBodyThenSequence) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kDocSequenceSection,
        Sized{
            "empty",  //
        },

        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 2u);
    ASSERT_EQ(msg.sequences[0].name, "empty");
    ASSERT_EQ(msg.sequences[0].objs.size(), 0u);
    ASSERT_EQ(msg.sequences[1].name, "docs");
    ASSERT_EQ(msg.sequences[1].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[1].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenSequenceThenBody) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kDocSequenceSection,
        Sized{
            "empty",  //
        },

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },

        kBodySection,
        fromjson("{ping: 1}"),
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 2u);
    ASSERT_EQ(msg.sequences[0].name, "empty");
    ASSERT_EQ(msg.sequences[0].objs.size(), 0u);
    ASSERT_EQ(msg.sequences[1].name, "docs");
    ASSERT_EQ(msg.sequences[1].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[1].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithBodyThenSequenceThenSequence) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },

        kDocSequenceSection,
        Sized{
            "empty",  //
        },
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 2u);
    ASSERT_EQ(msg.sequences[0].name, "docs");
    ASSERT_EQ(msg.sequences[0].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{a: 1}"));
    ASSERT_EQ(msg.sequences[1].name, "empty");
    ASSERT_EQ(msg.sequences[1].objs.size(), 0u);
}

TEST_F(OpMsgParser, FailsIfNoBody) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40587);
}

TEST_F(OpMsgParser, FailsIfNoBodyEvenWithSequence) {
    auto msg = OpMsgBytes{
        kNoFlags,  //

        kDocSequenceSection,
        Sized{"docs"},
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40587);
}

TEST_F(OpMsgParser, FailsIfTwoBodies) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kBodySection,
        fromjson("{pong: 1}"),
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40430);
}

TEST_F(OpMsgParser, FailsIfDuplicateSequences) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{"docs"},

        kDocSequenceSection,
        Sized{"docs"},
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40431);
}

TEST_F(OpMsgParser, FailsIfDuplicateSequenceWithBody) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1, 'docs': []}"),

        kDocSequenceSection,
        Sized{"docs"},
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40433);
}

TEST_F(OpMsgParser, FailsIfDuplicateSequenceWithBodyNested) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1, a: {b:[]}}"),

        kDocSequenceSection,
        Sized{"a.b"},
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40433);
}

TEST_F(OpMsgParser, SucceedsIfSequenceAndBodyHaveCommonPrefix) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{cursor: {ns: 'foo.bar', id: 1}}"),

        kDocSequenceSection,
        Sized{
            "cursor.firstBatch",  //
            fromjson("{_id: 1}"),
        },
    }.parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{cursor: {ns: 'foo.bar', id: 1}}"));
    ASSERT_EQ(msg.sequences.size(), 1u);
    ASSERT_EQ(msg.sequences[0].name, "cursor.firstBatch");
    ASSERT_EQ(msg.sequences[0].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{_id: 1}"));
}

TEST_F(OpMsgParser, FailsIfUnknownSectionKind) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        '\x99',    // This is where a section kind would go
        Sized{},
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 40432);
}

TEST_F(OpMsgParser, FailsIfBodyTooBig) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),
    }.addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::InvalidBSON);
}

TEST_F(OpMsgParser, FailsIfBodyTooBigIntoChecksum) {
    auto msg = OpMsgBytes{
        kHaveChecksum,  //
        kBodySection,
        fromjson("{ping: 1}"),
        kFakeCRC,
    }.addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::InvalidBSON);
}

TEST_F(OpMsgParser, FailsIfDocumentSequenceTooBig) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },
    }.addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FailsIfDocumentSequenceTooBigIntoChecksum) {
    auto msg = OpMsgBytes{
        kHaveChecksum,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        },

        kFakeCRC,
    }.addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FailsIfDocumentInSequenceTooBig) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
        }.addToSize(-1),  // Shrink sequence so document extends past end.
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::InvalidBSON);
}

TEST_F(OpMsgParser, FailsIfNameOfDocumentSequenceTooBig) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{
            "foo",
        }.addToSize(-1),  // Shrink sequence so document extends past end.
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FailsIfNameOfDocumentSequenceHasNoNulTerminator) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{'f', 'o', 'o'},
        // No '\0' at end of document. ASAN should complain if we keep looking for one.
    };

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FailsIfTooManyDocumentSequences) {
    auto msg = OpMsgBytes{
        kNoFlags,  //
        kBodySection,
        fromjson("{ping: 1}"),

        kDocSequenceSection,
        Sized{"foo"},

        kDocSequenceSection,
        Sized{"bar"},

        kDocSequenceSection,
        Sized{"baz"},
    };

    ASSERT_THROWS_WITH_CHECK(
        msg.parse(), ExceptionFor<ErrorCodes::TooManyDocumentSequences>, [](const DBException& ex) {
            ASSERT(ex.isA<ErrorCategory::ConnectionFatalMessageParseError>());
        });
}

TEST_F(OpMsgParser, FailsIfNoRoomForFlags) {
    // Flags are 4 bytes. Try 0-3 bytes.
    ASSERT_THROWS_CODE(OpMsgBytes{}.parse(), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE(OpMsgBytes{'\0'}.parse(), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE((OpMsgBytes{'\0', '\0'}.parse()), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE(
        (OpMsgBytes{'\0', '\0', '\0'}.parse()), AssertionException, ErrorCodes::Overflow);

    ASSERT_THROWS_CODE(OpMsg::flags(OpMsgBytes{}.done()), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE(
        OpMsg::flags(OpMsgBytes{'\0'}.done()), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE(
        OpMsg::flags(OpMsgBytes{'\0', '\0'}.done()), AssertionException, ErrorCodes::Overflow);
    ASSERT_THROWS_CODE(OpMsg::flags(OpMsgBytes{'\0', '\0', '\0'}.done()),
                       AssertionException,
                       ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FlagExtractionWorks) {
    ASSERT_EQ(OpMsg::flags(OpMsgBytes{0u}.done()), 0u);    // All clear.
    ASSERT_EQ(OpMsg::flags(OpMsgBytes{~0u}.done()), ~0u);  // All set.

    for (auto i = uint32_t(0); i < 32u; i++) {
        const auto flags = uint32_t(1) << i;
        ASSERT_EQ(OpMsg::flags(OpMsgBytes{flags}.done()), flags) << flags;
        ASSERT(OpMsg::isFlagSet(OpMsgBytes{flags}.done(), flags)) << flags;
        ASSERT(!OpMsg::isFlagSet(OpMsgBytes{~flags}.done(), flags)) << flags;
        ASSERT(!OpMsg::isFlagSet(OpMsgBytes{0u}.done(), flags)) << flags;
        ASSERT(OpMsg::isFlagSet(OpMsgBytes{~0u}.done(), flags)) << flags;
    }
}

TEST_F(OpMsgParser, FailsWithUnknownRequiredFlags) {
    // Bits 0 and 1 are known, and bits >= 16 are optional.
    for (auto i = uint32_t(2); i < 16u; i++) {
        auto flags = uint32_t(1) << i;
        auto msg = OpMsgBytes{
            flags,  //
            kBodySection,
            fromjson("{ping: 1}"),
        };

        ASSERT_THROWS_WITH_CHECK(msg.parse(), AssertionException, [](const DBException& ex) {
            ASSERT_EQ(ex.toStatus().code(), ErrorCodes::IllegalOpMsgFlag);
            ASSERT(ErrorCodes::isConnectionFatalMessageParseError(ex.toStatus().code()));
        });
    }
}

TEST_F(OpMsgParser, SucceedsWithUnknownOptionalFlags) {
    // bits >= 16 are optional.
    for (auto i = uint32_t(16); i < 32u; i++) {
        auto flags = uint32_t(1) << i;
        OpMsgBytes{
            flags,  //
            kBodySection,
            fromjson("{ping: 1}"),
        }.parse();
    }
}

void testSerializer(const Message& fromSerializer, OpMsgBytes&& expected) {
    const auto expectedMsg = expected.done();
    ASSERT_EQ(fromSerializer.operation(), dbMsg);
    // Ignoring request and reply ids since they aren't handled by OP_MSG code.

    auto gotSD = StringData(fromSerializer.singleData().data(), fromSerializer.dataSize());
    auto expectedSD = StringData(expectedMsg.singleData().data(), expectedMsg.dataSize());
    if (gotSD == expectedSD)
        return;

    size_t commonLength =
        std::mismatch(gotSD.begin(), gotSD.end(), expectedSD.begin(), expectedSD.end()).first -
        gotSD.begin();

    log() << "Mismatch after " << commonLength << " bytes.";
    log() << "Common prefix: " << hexdump(gotSD.rawData(), commonLength);
    log() << "Got suffix     : "
          << hexdump(gotSD.rawData() + commonLength, gotSD.size() - commonLength);
    log() << "Expected suffix: "
          << hexdump(expectedSD.rawData() + commonLength, expectedSD.size() - commonLength);
    FAIL("Serialization didn't match expected data. See above for details.");
}

TEST(OpMsgSerializer, JustBody) {
    OpMsg msg;
    msg.body = fromjson("{ping: 1}");

    testSerializer(msg.serialize(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kBodySection,
                       fromjson("{ping: 1}"),
                   });
}

TEST(OpMsgSerializer, BodyAndSequence) {
    OpMsg msg;
    msg.body = fromjson("{ping: 1}");
    msg.sequences = {{"docs", {fromjson("{a:1}"), fromjson("{a:2}")}}};

    testSerializer(msg.serialize(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kDocSequenceSection,
                       Sized{
                           "docs",  //
                           fromjson("{a: 1}"),
                           fromjson("{a: 2}"),
                       },

                       kBodySection,
                       fromjson("{ping: 1}"),
                   });
}

TEST(OpMsgSerializer, BodyAndEmptySequence) {
    OpMsg msg;
    msg.body = fromjson("{ping: 1}");
    msg.sequences = {{"docs", {}}};

    testSerializer(msg.serialize(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kDocSequenceSection,
                       Sized{
                           "docs",  //
                       },

                       kBodySection,
                       fromjson("{ping: 1}"),
                   });
}

TEST(OpMsgSerializer, BodyAndTwoSequences) {
    OpMsg msg;
    msg.body = fromjson("{ping: 1}");
    msg.sequences = {
        {"a", {fromjson("{a: 1}")}},  //
        {"b", {fromjson("{b: 1}")}},
    };

    testSerializer(msg.serialize(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kDocSequenceSection,
                       Sized{
                           "a",  //
                           fromjson("{a: 1}"),
                       },

                       kDocSequenceSection,
                       Sized{
                           "b",  //
                           fromjson("{b: 1}"),
                       },

                       kBodySection,
                       fromjson("{ping: 1}"),
                   });
}

TEST(OpMsgSerializer, BodyAndSequenceInPlace) {
    OpMsgBuilder builder;

    auto emptySeq = builder.beginDocSequence("empty");
    emptySeq.done();

    {
        auto seq = builder.beginDocSequence("docs");
        seq.append(fromjson("{a: 1}"));
        seq.appendBuilder().append("a", 2);
    }

    builder.beginBody().append("ping", 1);
    builder.resumeBody().append("$db", "foo");

    testSerializer(builder.finish(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kDocSequenceSection,
                       Sized{
                           "empty",
                       },

                       kDocSequenceSection,
                       Sized{
                           "docs",  //
                           fromjson("{a: 1}"),
                           fromjson("{a: 2}"),
                       },

                       kBodySection,
                       fromjson("{ping: 1, $db: 'foo'}"),
                   });
}

TEST(OpMsgSerializer, BodyAndInPlaceSequenceInPlaceWithReset) {
    OpMsgBuilder builder;

    auto emptySeq = builder.beginDocSequence("empty");
    emptySeq.done();

    {
        auto seq = builder.beginDocSequence("docs");
        seq.append(fromjson("{a: 1}"));
        seq.appendBuilder().append("a", 2);
    }

    builder.beginBody().append("ping", 1);
    builder.resumeBody().append("$db", "foo");

    builder.reset();

    // Everything above shouldn't matter.

    {
        auto seq = builder.beginDocSequence("docs2");
        seq.append(fromjson("{b: 1}"));
        seq.appendBuilder().append("b", 2);
    }

    builder.beginBody().append("pong", 1);

    testSerializer(builder.finish(),
                   OpMsgBytes{
                       kNoFlags,  //
                       kDocSequenceSection,
                       Sized{
                           "docs2",  //
                           fromjson("{b: 1}"),
                           fromjson("{b: 2}"),
                       },

                       kBodySection,
                       fromjson("{pong: 1}"),
                   });
}

TEST(OpMsgSerializer, ReplaceFlagsWorks) {
    {
        auto msg = OpMsgBytes{~0u}.done();
        OpMsg::replaceFlags(&msg, 0u);
        ASSERT_EQ(OpMsg::flags(msg), 0u);
    }
    {
        auto msg = OpMsgBytes{0u}.done();
        OpMsg::replaceFlags(&msg, ~0u);
        ASSERT_EQ(OpMsg::flags(msg), ~0u);
    }

    for (auto i = uint32_t(0); i < 32u; i++) {
        auto flags = uint32_t(1) << i;
        {
            auto msg = OpMsgBytes{0u}.done();
            OpMsg::replaceFlags(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), flags) << flags;
        }
        {
            auto msg = OpMsgBytes{~0u}.done();
            OpMsg::replaceFlags(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), flags) << flags;
        }
        {
            auto msg = OpMsgBytes{~flags}.done();
            OpMsg::replaceFlags(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), flags) << flags;
        }
    }
}

TEST(OpMsgSerializer, SetFlagWorks) {
    for (auto i = uint32_t(0); i < 32u; i++) {
        auto flags = uint32_t(1) << i;
        {
            auto msg = OpMsgBytes{0u}.done();
            OpMsg::setFlag(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), flags) << flags;
        }
        {
            auto msg = OpMsgBytes{~0u}.done();
            OpMsg::setFlag(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), ~0u) << flags;
        }
        {
            auto msg = OpMsgBytes{~flags}.done();
            OpMsg::setFlag(&msg, flags);
            ASSERT_EQ(OpMsg::flags(msg), ~0u) << flags;
        }
    }
}

TEST(OpMsgRequest, GetDatabaseWorks) {
    OpMsgRequest msg;
    msg.body = fromjson("{$db: 'foo'}");
    ASSERT_EQ(msg.getDatabase(), "foo");

    msg.body = fromjson("{before: 1, $db: 'foo'}");
    ASSERT_EQ(msg.getDatabase(), "foo");

    msg.body = fromjson("{before: 1, $db: 'foo', after: 1}");
    ASSERT_EQ(msg.getDatabase(), "foo");
}

TEST(OpMsgRequest, GetDatabaseThrowsWrongType) {
    OpMsgRequest msg;
    msg.body = fromjson("{$db: 1}");
    ASSERT_THROWS(msg.getDatabase(), DBException);
}

TEST(OpMsgRequest, GetDatabaseThrowsMissing) {
    OpMsgRequest msg;
    msg.body = fromjson("{}");
    ASSERT_THROWS(msg.getDatabase(), AssertionException);

    msg.body = fromjson("{$notdb: 'foo'}");
    ASSERT_THROWS(msg.getDatabase(), AssertionException);
}

TEST(OpMsgRequest, FromDbAndBodyDoesNotCopy) {
    auto body = fromjson("{ping: 1}");
    const void* const bodyPtr = body.objdata();
    auto msg = OpMsgRequest::fromDBAndBody("db", std::move(body));

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1, $db: 'db'}"));
    ASSERT_EQ(static_cast<const void*>(msg.body.objdata()), bodyPtr);
}
}  // namespace
}  // namespace mongo
