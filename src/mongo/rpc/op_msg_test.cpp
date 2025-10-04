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


#include "mongo/rpc/op_msg_test.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/serialization_context.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

class AuthorizationSessionImplTestHelper {
public:
    /**
     * Synthesize a user with the useTenant privilege and add them to the authorization session.
     */
    static void grantUseTenant(Client& client) {
        User user(
            std::make_unique<UserRequestGeneral>(UserName("useTenant", "admin"), boost::none));
        user.setPrivileges(
            {Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useTenant)});
        auto* as = dynamic_cast<AuthorizationSessionImpl*>(AuthorizationSession::get(client));
        if (as->_authenticatedUser != boost::none) {
            as->logoutAllDatabases("AuthorizationSessionImplTestHelper"_sd);
        }
        as->_authenticatedUser = std::move(user);
        as->_authenticationMode = AuthorizationSession::AuthenticationMode::kConnection;
        as->_updateInternalAuthorizationState();
    }
};

namespace rpc {
namespace test {
namespace {

// Fixture class to raise log verbosity so that invalid messages are printed by the parser.
class OpMsgParser : public unittest::Test {
    unittest::MinimumLoggedSeverityGuard _severityGuard{logv2::LogComponent::kNetwork,
                                                        logv2::LogSeverity::Debug(1)};
};

// Flags
constexpr uint32_t kNoFlags = 0;
constexpr uint32_t kHaveChecksum = OpMsg::kChecksumPresent;

TEST_F(OpMsgParser, SucceedsWithJustBody) {
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),
        }
            .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 0u);
}

TEST_F(OpMsgParser, SucceedsWithChecksum) {
    auto msg = OpMsgBytes{kHaveChecksum,  //
                          kBodySection,
                          fromjson("{ping: 1}")}
                   .appendChecksum()
                   .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 0u);
}

TEST_F(OpMsgParser, SucceedsWithBodyThenSequence) {
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),

            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
                fromjson("{a: 2}"),
            },
        }
            .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 1u);
    ASSERT_EQ(msg.sequences[0].name, "docs");
    ASSERT_EQ(msg.sequences[0].objs.size(), 2u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{a: 1}"));
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[1], fromjson("{a: 2}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenBody) {
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
            },

            kBodySection,
            fromjson("{ping: 1}"),
        }
            .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 1u);
    ASSERT_EQ(msg.sequences[0].name, "docs");
    ASSERT_EQ(msg.sequences[0].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[0].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenBodyThenSequence) {
    auto msg =
        OpMsgBytes{
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
        }
            .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 2u);
    ASSERT_EQ(msg.sequences[0].name, "empty");
    ASSERT_EQ(msg.sequences[0].objs.size(), 0u);
    ASSERT_EQ(msg.sequences[1].name, "docs");
    ASSERT_EQ(msg.sequences[1].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[1].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithSequenceThenSequenceThenBody) {
    auto msg =
        OpMsgBytes{
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
        }
            .parse();

    ASSERT_BSONOBJ_EQ(msg.body, fromjson("{ping: 1}"));
    ASSERT_EQ(msg.sequences.size(), 2u);
    ASSERT_EQ(msg.sequences[0].name, "empty");
    ASSERT_EQ(msg.sequences[0].objs.size(), 0u);
    ASSERT_EQ(msg.sequences[1].name, "docs");
    ASSERT_EQ(msg.sequences[1].objs.size(), 1u);
    ASSERT_BSONOBJ_EQ(msg.sequences[1].objs[0], fromjson("{a: 1}"));
}

TEST_F(OpMsgParser, SucceedsWithBodyThenSequenceThenSequence) {
    auto msg =
        OpMsgBytes{
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
        }
            .parse();

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
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{cursor: {ns: 'foo.bar', id: 1}}"),

            kDocSequenceSection,
            Sized{
                "cursor.firstBatch",  //
                fromjson("{_id: 1}"),
            },
        }
            .parse();

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
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),
        }
            .addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::InvalidBSON);
}

TEST_F(OpMsgParser, FailsIfBodyTooBigIntoChecksum) {
    auto msg =
        OpMsgBytes{
            kHaveChecksum,  //
            kBodySection,
            fromjson("{ping: 1}"),
        }
            .appendChecksum()
            .addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::InvalidBSON);
}

TEST_F(OpMsgParser, FailsIfDocumentSequenceTooBig) {
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),

            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
            },
        }
            .addToSize(-1);  // Shrink message so body extends past end.

    ASSERT_THROWS_CODE(msg.parse(), AssertionException, ErrorCodes::Overflow);
}

TEST_F(OpMsgParser, FailsIfDocumentSequenceTooBigIntoChecksum) {
    auto msg =
        OpMsgBytes{
            kHaveChecksum,  //
            kBodySection,
            fromjson("{ping: 1}"),

            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
            },
        }
            .appendChecksum()
            .addToSize(-1);  // Shrink message so body extends past end.

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
        }
            .addToSize(-1),  // Shrink sequence so document extends past end.
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
        }
            .addToSize(-1),  // Shrink sequence so document extends past end.
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
            ASSERT(ErrorCodes::isConnectionFatalMessageParseError(ex));
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
        }
            .parse();
    }
}

TEST_F(OpMsgParser, FailsWithChecksumMismatch) {
    auto msg = OpMsgBytes{kHaveChecksum,  //
                          kBodySection,
                          fromjson("{ping: 1}")}
                   .appendChecksum(123);

    ASSERT_THROWS_WITH_CHECK(msg.parse(), AssertionException, [](const DBException& ex) {
        ASSERT_EQ(ex.toStatus().code(), ErrorCodes::ChecksumMismatch);
        ASSERT(ErrorCodes::isConnectionFatalMessageParseError(ex.toStatus().code()));
    });
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

    LOGV2(22636, "Mismatch after {commonLength} bytes.", "commonLength"_attr = commonLength);
    LOGV2(22637,
          "Common prefix: {hexdump_gotSD_rawData_commonLength}",
          "hexdump_gotSD_rawData_commonLength"_attr = hexdump(gotSD.substr(0, commonLength)));
    LOGV2(22638,
          "Got suffix     : {hexdump_gotSD_rawData_commonLength_gotSD_size_commonLength}",
          "hexdump_gotSD_rawData_commonLength_gotSD_size_commonLength"_attr =
              hexdump(gotSD.substr(commonLength)));
    LOGV2(22639,
          "Expected suffix: {hexdump_expectedSD_rawData_commonLength_expectedSD_size_commonLength}",
          "hexdump_expectedSD_rawData_commonLength_expectedSD_size_commonLength"_attr =
              hexdump(expectedSD.substr(commonLength)));
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

class OpMsgWithAuth : public mongo::ScopedGlobalServiceContextForTest, public unittest::Test {
protected:
    void setUp() final {
        auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
        AuthorizationManager::set(getService(),
                                  globalAuthzManagerFactory->createShard(getService()));
        AuthorizationManager::get(getService())->setAuthEnabled(true);

        auth::AuthorizationBackendInterface::set(
            getService(), globalAuthzManagerFactory->createBackendInterface(getService()));

        client = getServiceContext()->getService()->makeClient("test");
    }

    ServiceContext::UniqueClient client;
};

TEST_F(OpMsgWithAuth, ParseValidatedTenancyScopeFromSecurityToken) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const auto kTenantId = TenantId(OID::gen());
    const auto token = std::string{auth::ValidatedTenancyScopeFactory::create(
                                       UserName("user", "admin", kTenantId),
                                       "secret"_sd,
                                       auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                                       auth::ValidatedTenancyScopeFactory::TokenForTestingTag{})
                                       .getOriginalToken()};
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),

            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
                fromjson("{a: 2}"),
            },

            kSecurityTokenSection,
            token,
        }
            .parse(client.get());

    auto body = BSON("ping" << 1);

    ASSERT(msg.validatedTenancyScope);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), kTenantId);
}

TEST_F(OpMsgWithAuth, ValidatedTenancyScopeShouldNotBeSerialized) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));

    const auto kTenantId = TenantId(OID::gen());

    const auto token = std::string{auth::ValidatedTenancyScopeFactory::create(
                                       kTenantId,
                                       auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy,
                                       auth::ValidatedTenancyScopeFactory::TenantForTestingTag{})
                                       .getOriginalToken()};

    const auto body = BSON("ping" << 1);
    auto msgBytes = OpMsgBytes{
        kNoFlags,  //
        kSecurityTokenSection,
        token,

        kDocSequenceSection,
        Sized{
            "docs",  //
            fromjson("{a: 1}"),
            fromjson("{a: 2}"),
        },
        kBodySection,
        body,
    };
    auto msg = msgBytes.parse(client.get());
    ASSERT(msg.validatedTenancyScope);

    auto serializedMsg = msg.serialize();
    testSerializer(serializedMsg,
                   OpMsgBytes{
                       kNoFlags,  //
                       kSecurityTokenSection,
                       token,

                       kDocSequenceSection,
                       Sized{
                           "docs",  //
                           fromjson("{a: 1}"),
                           fromjson("{a: 2}"),
                       },
                       kBodySection,
                       body,
                   });
}

TEST(OpMsgRequest, GetDatabaseWorks) {
    OpMsgRequest msg;
    msg.body = fromjson("{$db: 'foo'}");
    ASSERT_EQ(msg.parseDbName().toString_forTest(), "foo");

    msg.body = fromjson("{before: 1, $db: 'foo'}");
    ASSERT_EQ(msg.parseDbName().toString_forTest(), "foo");

    msg.body = fromjson("{before: 1, $db: 'foo', after: 1}");
    ASSERT_EQ(msg.parseDbName().toString_forTest(), "foo");
}

TEST(OpMsgRequest, GetDatabaseThrowsWrongType) {
    OpMsgRequest msg;
    msg.body = fromjson("{$db: 1}");
    ASSERT_THROWS(msg.parseDbName().toString_forTest(), DBException);
}

TEST(OpMsgRequest, GetDatabaseThrowsMissing) {
    OpMsgRequest msg;
    msg.body = fromjson("{}");
    ASSERT_THROWS(msg.parseDbName().toString_forTest(), AssertionException);

    msg.body = fromjson("{$notdb: 'foo'}");
    ASSERT_THROWS(msg.parseDbName().toString_forTest(), AssertionException);
}

TEST(OpMsgRequestBuilder, WithVTS) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const TenantId tenantId(OID::gen());
    const auto vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret"_sd,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

    const StringData dbString = "testDb";
    auto const body = fromjson("{ping: 1}");

    OpMsgRequest msg = OpMsgRequestBuilder::create(
        vts, DatabaseName::createDatabaseName_forTest(tenantId, dbString), body);
    ASSERT(msg.validatedTenancyScope);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), tenantId);
    ASSERT_EQ(msg.parseDbName().toString_forTest(), dbString);
}

TEST(OpMsgRequestBuilder, WithVTSAndSerializationContextExpPrefixDefault) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const TenantId tenantId(OID::gen());
    const StringData dbString = "testDb";
    const std::string dbStringWithTid = str::stream() << tenantId.toString() << "_" << dbString;
    auto const body = fromjson("{ping: 1}");

    using Prefix = SerializationContext::Prefix;

    auth::ValidatedTenancyScope vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret"_sd,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

    OpMsgRequest msg = OpMsgRequestBuilder::create(
        vts, DatabaseName::createDatabaseName_forTest(tenantId, dbString), body);
    ASSERT(msg.validatedTenancyScope);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), tenantId);
    ASSERT_EQ(msg.parseDbName().toString_forTest(), dbString);
}

void CheckVtsSetsPrefix(Client* client, bool simulateAtlasProxyTenantProtocol) {
    const auto kTenantId = TenantId(OID::gen());
    const auto token =
        std::string{auth::ValidatedTenancyScopeFactory::create(
                        UserName("user", "admin", kTenantId),
                        "secret"_sd,
                        simulateAtlasProxyTenantProtocol
                            ? auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy
                            : auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{})
                        .getOriginalToken()};
    auto msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            fromjson("{ping: 1}"),

            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
                fromjson("{a: 2}"),
            },

            kSecurityTokenSection,
            token,
        }
            .parse(client);
    auth::ValidatedTenancyScope vts = msg.validatedTenancyScope.value();
    ASSERT_TRUE(vts.isFromAtlasProxy() == simulateAtlasProxyTenantProtocol);

    auto serializedMsg = msg.serialize();
    auto request = OpMsgRequest::parse(serializedMsg, client);
    ASSERT(request.validatedTenancyScope);
    ASSERT_EQ(request.getValidatedTenantId().value(), vts.tenantId());
    ASSERT_TRUE(request.validatedTenancyScope->isFromAtlasProxy() ==
                simulateAtlasProxyTenantProtocol);
    ASSERT_EQ(request.getSerializationContext().getPrefix(),
              simulateAtlasProxyTenantProtocol ? SerializationContext::Prefix::IncludePrefix
                                               : SerializationContext::Prefix::ExcludePrefix);
}

TEST_F(OpMsgWithAuth, TestVTSSetsPrefixStateFalse) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    CheckVtsSetsPrefix(client.get(), false);
}

TEST_F(OpMsgWithAuth, TestVTSSetsPrefixStateTrue) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    CheckVtsSetsPrefix(client.get(), true);
}

void CheckCommandMsgIdlParsingForOpMsgRequest(bool simulateAtlasProxyTenantProtocol) {
    const TenantId tenantId(OID::gen());
    const std::string dbString =
        simulateAtlasProxyTenantProtocol ? (tenantId.toString() + "_testDb") : "testDb";
    auto cmd = BSON("insert" << "bar"
                             << "$db" << dbString << "documents" << BSON_ARRAY(BSONObj()));
    OpMsgRequest msg;
    msg.body = cmd;
    auth::ValidatedTenancyScope vts = auth::ValidatedTenancyScopeFactory::create(
        tenantId,
        simulateAtlasProxyTenantProtocol ? auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy
                                         : auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
    msg.validatedTenancyScope = vts;
    auto op = InsertOp::parse(msg);
    ASSERT_EQ(op.getSerializationContext().getPrefix(),
              simulateAtlasProxyTenantProtocol ? SerializationContext::Prefix::IncludePrefix
                                               : SerializationContext::Prefix::ExcludePrefix);
}

TEST_F(OpMsgWithAuth, TestExpectPrefixTrueParsedInMsg) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    CheckCommandMsgIdlParsingForOpMsgRequest(true);
}

TEST_F(OpMsgWithAuth, TestExpectPrefixFalseParsedInMsg) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    CheckCommandMsgIdlParsingForOpMsgRequest(false);
}

TEST(OpMsgRequestBuilder, WithVTSAndSerializationContextExpPrefixFalse) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const TenantId tenantId(OID::gen());
    const StringData dbString = "testDb";
    const std::string dbStringWithTid = str::stream() << tenantId.toString() << "_" << dbString;
    auto const body = fromjson("{ping: 1}");

    using Prefix = SerializationContext::Prefix;

    auth::ValidatedTenancyScope vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret"_sd,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

    OpMsgRequest msg = OpMsgRequestBuilder::create(
        vts, DatabaseName::createDatabaseName_forTest(tenantId, dbString), body);
    ASSERT(msg.validatedTenancyScope);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), tenantId);
    ASSERT_EQ(msg.parseDbName().toString_forTest(), dbString);
}

TEST(OpMsgRequestBuilder, WithVTSAndSerializationContextExpPrefixTrue) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const TenantId tenantId(OID::gen());
    const StringData dbString = "testDb";
    const std::string dbStringWithTid = str::stream() << tenantId.toString() << "_" << dbString;
    auto const body = fromjson("{ping: 1}");

    using Prefix = SerializationContext::Prefix;

    auth::ValidatedTenancyScope vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret"_sd,
        auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

    OpMsgRequest msg = OpMsgRequestBuilder::create(
        vts, DatabaseName::createDatabaseName_forTest(tenantId, dbString), body);
    ASSERT(msg.validatedTenancyScope);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), tenantId);
    ASSERT_EQ(msg.parseDbName().toStringWithTenantId_forTest(), dbStringWithTid);
}

TEST(OpMsgRequestBuilder, CreateDoesNotCopy) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest requireTenantIdController("featureFlagRequireTenantID",
                                                                   true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");


    const TenantId tenantId(OID::gen());

    auth::ValidatedTenancyScope vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret"_sd,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

    auto body = fromjson("{ping: 1}");
    const void* const bodyPtr = body.objdata();
    auto msg = OpMsgRequestBuilder::create(
        vts, DatabaseName::createDatabaseName_forTest(tenantId, "db"), std::move(body));

    auto const newBody = BSON("ping" << 1 << "$db"
                                     << "db");
    ASSERT_BSONOBJ_EQ(msg.body, newBody);
    ASSERT_EQ(static_cast<const void*>(msg.body.objdata()), bodyPtr);
}

TEST(OpMsgTest, ChecksumResizesMessage) {
    auto msg = OpMsgBytes{kNoFlags,  //
                          kBodySection,
                          fromjson("{ping: 1}")}
                   .done();

    // Test that appendChecksum() resizes the buffer if necessary.
    const auto capacity = msg.sharedBuffer().capacity();
    OpMsg::appendChecksum(&msg);
    ASSERT_EQ(msg.sharedBuffer().capacity(), capacity + 4);
    // The checksum is correct.
    OpMsg::parse(msg);
}

TEST(OpMsgTest, EmptyMessageWithChecksumFlag) {
    // Checks that an empty message that would normally be invalid because it's
    // missing a body, is invalid because a checksum was specified in the flag
    // but no checksum was included.
    auto msg = OpMsgBytes{OpMsg::kChecksumPresent};
    ASSERT_THROWS_CODE(msg.parse(), AssertionException, 51252);
}

TEST_F(OpMsgWithAuth, GetDbNameWithVTS) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));

    const auto kTenantId = TenantId(OID::gen());
    std::string db = "myDb";
    BSONObjBuilder builder;
    builder.append("ping", 1).append("$db", db);

    const auto token = std::string{auth::ValidatedTenancyScopeFactory::create(
                                       kTenantId,
                                       auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                                       auth::ValidatedTenancyScopeFactory::TenantForTestingTag{})
                                       .getOriginalToken()};

    const auto body = builder.obj();
    OpMsg msg =
        OpMsgBytes{
            kNoFlags,
            kBodySection,
            body,
            kDocSequenceSection,
            Sized{
                "docs",
                fromjson("{a: 1}"),
                fromjson("{a: 2}"),
            },
            kSecurityTokenSection,
            token,
        }
            .parse(client.get());


    const DatabaseName expectedTenantDbName =
        DatabaseName::createDatabaseName_forTest(kTenantId, "myDb");
    using SC = SerializationContext;

    // Test the request which has tenant prefix.
    OpMsgRequest request = OpMsgRequest(std::move(msg));
    ASSERT_EQ(request.getSerializationContext(),
              SerializationContext(
                  SC::Source::Command, SC::CallerType::Request, SC::Prefix::ExcludePrefix));
    ASSERT_EQ(request.parseDbName(), expectedTenantDbName);
}

TEST_F(OpMsgWithAuth, GetDbNameWithVTSIncludePrefix) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));

    const auto kTenantId = TenantId(OID::gen());
    std::string db = kTenantId.toString() + "_myDb";
    BSONObjBuilder builder;
    builder.append("ping", 1).append("$db", db);

    const auto token = std::string{auth::ValidatedTenancyScopeFactory::create(
                                       kTenantId,
                                       auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy,
                                       auth::ValidatedTenancyScopeFactory::TenantForTestingTag{})
                                       .getOriginalToken()};

    const auto body = builder.obj();
    OpMsg msg =
        OpMsgBytes{
            kNoFlags,  //
            kBodySection,
            body,
            kDocSequenceSection,
            Sized{
                "docs",  //
                fromjson("{a: 1}"),
                fromjson("{a: 2}"),
            },
            kSecurityTokenSection,
            token,
        }
            .parse(client.get());

    const DatabaseName expectedTenantDbName =
        DatabaseName::createDatabaseName_forTest(kTenantId, "myDb");
    using SC = SerializationContext;

    // Test the request which does not have tenant prefix.
    const DatabaseName expectedDbName = DatabaseName::createDatabaseName_forTest(kTenantId, "myDb");
    OpMsgRequest request{std::move(msg)};
    ASSERT_EQ(request.getSerializationContext(),
              SerializationContext(
                  SC::Source::Command, SC::CallerType::Request, SC::Prefix::IncludePrefix));
    ASSERT_EQ(request.parseDbName(), expectedDbName);
}

}  // namespace
}  // namespace test
}  // namespace rpc
}  // namespace mongo
