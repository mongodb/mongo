/**
 * Tests how mongod and mongos validate the structure of incoming OP_MSG requests: flag bits,
 * checksums, the moreToCome and exhaustAllowed flags, and each section kind. Requests are built
 * byte by byte, so malformed messages that no driver would send can be covered.
 *
 * Most cases run before authentication, where the server applies preAuthMaximumMessageSizeBytes.
 * The body and document sequence size limits, and commands that accept document sequences, are
 * checked on a server without authentication, since unauthenticated clients cannot reach them.
 *
 * @tags: [
 *   # The requests are sent over a plain TCP socket.
 *   grpc_incompatible,
 *   requires_auth,
 *   requires_sharding,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertClosedWithoutReply,
    assertReplyError,
    assertReplyOk,
    body,
    docSequence,
    kChecksumPresent,
    kExhaustAllowed,
    kInvalidBSONHex,
    kMoreToCome,
    kValidTraceparent,
    rawSection,
    sendRawOpMsg,
    telemetry,
} from "jstests/noPassthrough/network/libs/raw_op_msg.js";

const kPreAuthMaxMessageSize = 16 * 1024;
const kMaxTelemetrySectionSize = 4 * 1024;
const kMaxUserBSONSize = 16 * 1024 * 1024;
// BSONObjMaxInternalSize: each document sequence document may be up to the user limit, plus 16KB.
const kMaxInternalBSONSize = kMaxUserBSONSize + 16 * 1024;
// kOpMsgReplyBSONBufferMaxSize: the body may be up to twice the user document limit, plus 64KB.
const kMaxBodySize = 2 * kMaxUserBSONSize + 64 * 1024;

const hello = () => body({hello: 1});
const ping = () => ({sections: [body({ping: 1})]});

/**
 * Sends `request` followed by a ping on the same connection. Returns the result for `request` and
 * asserts that the ping succeeds, which shows the connection is still usable.
 */
function sendThenPing(conn, request) {
    const [result, pingResult] = sendRawOpMsg(conn, [request, ping()]);
    assertReplyOk(pingResult);
    return result;
}

/** Sends a request that the server should reject by closing the connection. */
function assertRequestClosesConnection(conn, request) {
    const [result, pingResult] = sendRawOpMsg(conn, [request, ping()]);
    assertClosedWithoutReply(result);
    assert(pingResult.closed, "expected no further requests on the connection", {pingResult});
}

/**
 * Defines the cases that behave the same on mongod and mongos, run against `fixture.conn`.
 * `fixture.exhaustHelloWithoutMaxAwaitTimeCode` is the error each binary uses for an exhaust hello
 * without maxAwaitTimeMS.
 */
function definePreAuthCases(fixture) {
    describe("flag bits", function () {
        it("accepts a request with no flags", function () {
            assertReplyOk(sendThenPing(fixture.conn, {sections: [hello()]}));
        });

        it("closes the connection on unknown required flag bits (low 16 bits)", function () {
            for (const bit of [2, 3, 8, 15]) {
                jsTest.log.info("Sending unknown required flag bit", {bit});
                assertRequestClosesConnection(fixture.conn, {flags: 2 ** bit, sections: [hello()]});
            }
        });

        it("ignores unknown optional flag bits (high 16 bits)", function () {
            for (const bit of [17, 24, 31]) {
                jsTest.log.info("Sending unknown optional flag bit", {bit});
                assertReplyOk(sendThenPing(fixture.conn, {flags: 2 ** bit, sections: [hello()]}));
            }
        });

        it("accepts a valid checksum", function () {
            const request = {flags: kChecksumPresent, sections: [hello()], checksum: "valid"};
            assertReplyOk(sendThenPing(fixture.conn, request));
        });

        it("closes the connection on a checksum mismatch", function () {
            assertRequestClosesConnection(fixture.conn, {
                flags: kChecksumPresent,
                sections: [hello()],
                checksum: "invalid",
            });
        });

        it("closes the connection when the checksum flag is set on a message with only a checksum", function () {
            assertRequestClosesConnection(fixture.conn, {
                flags: kChecksumPresent,
                sections: [],
                checksum: "valid",
            });
        });

        it("closes the connection when the checksum flag is set but no checksum is sent", function () {
            // The last four bytes of the body are read as the checksum, which does not match.
            assertRequestClosesConnection(fixture.conn, {
                flags: kChecksumPresent,
                sections: [hello()],
            });
        });
    });

    describe("moreToCome flag", function () {
        it("runs the command without replying, for any command", function () {
            for (const cmd of [{hello: 1}, {isMaster: 1}, {ping: 1}, {buildInfo: 1}]) {
                jsTest.log.info("Sending fire-and-forget command", {cmd});
                const [result, pingResult] = sendRawOpMsg(fixture.conn, [
                    {flags: kMoreToCome, sections: [body(cmd)], replies: 0},
                    ping(),
                ]);
                assert(!result.closed, "connection was unexpectedly closed", {result});
                // The only reply on the connection answers the ping, not the fire-and-forget request.
                assertReplyOk(pingResult);
            }
        });

        it("does not reply when the command fails", function () {
            for (const cmd of [{notARealCommand: 1}, {find: "coll"}]) {
                jsTest.log.info("Sending failing fire-and-forget command", {cmd});
                const [result, pingResult] = sendRawOpMsg(fixture.conn, [
                    {flags: kMoreToCome, sections: [body(cmd)], replies: 0},
                    ping(),
                ]);
                assert(!result.closed, "connection was unexpectedly closed", {result});
                assertReplyOk(pingResult);
            }
        });

        it("does not reply when the request fails a non-fatal parse check", function () {
            const [result, pingResult] = sendRawOpMsg(fixture.conn, [
                {flags: kMoreToCome, sections: [hello(), telemetry(), telemetry()], replies: 0},
                ping(),
            ]);
            assert(!result.closed, "connection was unexpectedly closed", {result});
            assertReplyOk(pingResult);
        });

        it("still closes the connection on a fatal parse error", function () {
            const [result, pingResult] = sendRawOpMsg(fixture.conn, [
                {flags: kMoreToCome, sections: [hello(), hello()], replies: 0},
                ping(),
            ]);
            assert.eq(result.replies.length, 0, "fire-and-forget request got a reply", {result});
            // No reply is read for the fire-and-forget request, so the close shows up on the ping.
            assertClosedWithoutReply(pingResult);
        });
    });

    describe("exhaustAllowed flag", function () {
        it("is ignored by commands that do not support exhaust", function () {
            for (const {cmd, code} of [
                {cmd: {ping: 1}},
                {cmd: {connectionStatus: 1}},
                {cmd: {notARealCommand: 1}, code: ErrorCodes.CommandNotFound},
            ]) {
                jsTest.log.info("Sending exhaustAllowed with non-exhaust command", {cmd});
                // A following ping is answered next, so the server did not stream replies.
                const result = sendThenPing(fixture.conn, {
                    flags: kExhaustAllowed,
                    sections: [body(cmd)],
                });
                if (code === undefined) {
                    assertReplyOk(result);
                } else {
                    assertReplyError(result, code);
                }
                assert.eq(result.replies[0].flags & kMoreToCome, 0, "reply started a stream", {
                    result,
                });
            }
        });

        it("makes a hello that is not awaitable fail", function () {
            for (const cmd of [{hello: 1}, {isMaster: 1}, {ismaster: 1}]) {
                jsTest.log.info("Sending exhaustAllowed without maxAwaitTimeMS", {cmd});
                const result = sendThenPing(fixture.conn, {
                    flags: kExhaustAllowed,
                    sections: [body(cmd)],
                });
                assertReplyError(result, fixture.exhaustHelloWithoutMaxAwaitTimeCode);
            }
        });

        it("streams replies to an awaitable hello", function () {
            const [first, streaming] = sendRawOpMsg(fixture.conn, [
                {sections: [hello()]},
                {
                    flags: kExhaustAllowed,
                    sections: [body({hello: 1, maxAwaitTimeMS: 100})],
                    injectTopologyVersion: true,
                    replies: 2,
                },
            ]);
            assertReplyOk(first);

            assert(!streaming.closed, "connection was unexpectedly closed", {streaming});
            const [reply1, reply2] = streaming.replies;
            assert.eq(
                reply1.responseTo,
                streaming.requestId,
                "first reply is not for the request",
                {
                    streaming,
                },
            );
            assert.commandWorked(reply1.doc);
            assert.neq(reply1.flags & kMoreToCome, 0, "first reply did not start a stream", {
                streaming,
            });
            // Each streamed reply answers the previous one.
            assert.eq(reply2.responseTo, reply1.requestId, "second reply is not streamed", {
                streaming,
            });
            assert.commandWorked(reply2.doc);
        });
    });

    describe("body section (kind 0)", function () {
        it("closes the connection when there are no sections", function () {
            assertRequestClosesConnection(fixture.conn, {sections: []});
        });

        it("closes the connection when there is no body section", function () {
            assertRequestClosesConnection(fixture.conn, {sections: [telemetry()]});
            assertRequestClosesConnection(fixture.conn, {
                sections: [docSequence("docs", [{a: 1}])],
            });
        });

        it("closes the connection on repeated body sections", function () {
            assertRequestClosesConnection(fixture.conn, {sections: [hello(), hello()]});
            assertRequestClosesConnection(fixture.conn, {sections: [hello(), body({ping: 1})]});
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello(), telemetry(), hello()],
            });
        });

        it("closes the connection on invalid BSON", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [rawSection(0, kInvalidBSONHex)],
            });
        });

        it("closes the connection when the document length exceeds the message", function () {
            // The document claims 255 bytes but only 5 follow.
            assertRequestClosesConnection(fixture.conn, {sections: [rawSection(0, "ff00000000")]});
        });

        it("closes the connection when the body is followed by trailing bytes", function () {
            // The trailing bytes are read as another section, so their first byte is a section kind.
            assertRequestClosesConnection(fixture.conn, {sections: [hello(), {hex: "05"}]});
            assertRequestClosesConnection(fixture.conn, {sections: [hello(), {hex: "00"}]});
        });

        it("accepts a message at the pre-auth size limit", function () {
            const request = {sections: [hello()], padMessageTo: kPreAuthMaxMessageSize};
            assertReplyOk(sendThenPing(fixture.conn, request));
        });

        it("closes the connection on a message over the pre-auth size limit", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello()],
                padMessageTo: kPreAuthMaxMessageSize + 1,
            });
        });
    });

    describe("document sequence section (kind 1)", function () {
        // Parsing accepts up to two document sequences, but commands that do not declare document
        // sequence fields, such as hello, then fail with 40472 and keep the connection open.
        it("parses one or two document sequences, which hello then rejects", function () {
            for (const sections of [
                [hello(), docSequence("a", [{x: 1}])],
                [docSequence("a", [{x: 1}, {x: 2}]), hello(), docSequence("b", [{y: 1}])],
            ]) {
                assertReplyError(sendThenPing(fixture.conn, {sections}), 40472);
            }
        });

        it("parses a document sequence with no documents, which hello then rejects", function () {
            assertReplyError(
                sendThenPing(fixture.conn, {sections: [hello(), docSequence("a", [])]}),
                40472,
            );
        });

        it("closes the connection on more than two document sequences", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [
                    hello(),
                    docSequence("a", []),
                    docSequence("b", []),
                    docSequence("c", []),
                ],
            });
        });

        it("closes the connection on duplicate document sequence names", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello(), docSequence("a", [{x: 1}]), docSequence("a", [{x: 2}])],
            });
        });

        it("closes the connection when a document sequence name is also a body field", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello(), docSequence("hello", [])],
            });
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello(), docSequence("$db", [])],
            });
        });

        it("closes the connection on invalid BSON in a document sequence", function () {
            // size (4 + 2 + 12 bytes) + name "a" + the invalid document.
            const hex = "12000000" + "6100" + kInvalidBSONHex;
            assertRequestClosesConnection(fixture.conn, {sections: [hello(), rawSection(1, hex)]});
        });

        it("replies with an error when the document sequence has no name", function () {
            // A size of 4 covers only the size field itself, so there is no name to read.
            const result = sendThenPing(fixture.conn, {
                sections: [hello(), docSequence("a", [], 4)],
            });
            assertReplyError(result, ErrorCodes.Overflow);
        });

        it("replies with an error when the size is smaller than the size field", function () {
            for (const size of [0, 3, -1]) {
                jsTest.log.info("Sending document sequence size", {size});
                const result = sendThenPing(fixture.conn, {
                    sections: [hello(), docSequence("a", [], size)],
                });
                assertReplyError(result, ErrorCodes.Overflow);
            }
        });

        it("replies with an error when the size exceeds the message", function () {
            const result = sendThenPing(fixture.conn, {
                sections: [hello(), docSequence("a", [], 1000)],
            });
            assertReplyError(result, ErrorCodes.Overflow);
        });
    });

    describe("telemetry context section (kind 3)", function () {
        it("accepts one telemetry context section", function () {
            assertReplyOk(sendThenPing(fixture.conn, {sections: [hello(), telemetry()]}));
            assertReplyOk(sendThenPing(fixture.conn, {sections: [telemetry(), hello()]}));
        });

        it("replies with an error on repeated telemetry context sections", function () {
            const result = sendThenPing(fixture.conn, {
                sections: [hello(), telemetry(), telemetry()],
            });
            assertReplyError(result, ErrorCodes.BadValue);
        });

        it("accepts a telemetry context section at the size limit", function () {
            const doc = {otel: {traceparent: kValidTraceparent}, $padTo: kMaxTelemetrySectionSize};
            assertReplyOk(sendThenPing(fixture.conn, {sections: [hello(), telemetry(doc)]}));
        });

        it("replies with an error on a telemetry context section over the size limit", function () {
            const doc = {
                otel: {traceparent: kValidTraceparent},
                $padTo: kMaxTelemetrySectionSize + 1,
            };
            const result = sendThenPing(fixture.conn, {sections: [hello(), telemetry(doc)]});
            assertReplyError(result, ErrorCodes.BSONObjectTooLarge);
        });

        it("ignores unknown fields", function () {
            const doc = {otel: {traceparent: kValidTraceparent, extra: 1}, extra: "x"};
            assertReplyOk(sendThenPing(fixture.conn, {sections: [hello(), telemetry(doc)]}));
        });

        it("replies with an error when required fields are missing", function () {
            for (const doc of [{}, {otel: {}}]) {
                jsTest.log.info("Sending telemetry context", {doc});
                const result = sendThenPing(fixture.conn, {sections: [hello(), telemetry(doc)]});
                assertReplyError(result, ErrorCodes.IDLFailedToParse);
            }
        });

        it("replies with an error when fields have the wrong type", function () {
            for (const doc of [{otel: 1}, {otel: {traceparent: 1}}]) {
                jsTest.log.info("Sending telemetry context", {doc});
                const result = sendThenPing(fixture.conn, {sections: [hello(), telemetry(doc)]});
                assertReplyError(result, ErrorCodes.TypeMismatch);
            }
        });

        it("replies with an error on an invalid traceparent", function () {
            const result = sendThenPing(fixture.conn, {
                sections: [hello(), telemetry({otel: {traceparent: "not-a-traceparent"}})],
            });
            assertReplyError(result, ErrorCodes.BadValue);
        });

        it("closes the connection on invalid BSON", function () {
            assertRequestClosesConnection(fixture.conn, {
                sections: [hello(), rawSection(3, kInvalidBSONHex)],
            });
        });
    });

    describe("unknown section kinds", function () {
        it("closes the connection", function () {
            for (const kind of [2, 4, 127, 255]) {
                jsTest.log.info("Sending section kind", {kind});
                // Kind 2 used to carry a security token document, so send one with a document too.
                const sections = [hello(), {kind, doc: {a: 1}}];
                assertRequestClosesConnection(fixture.conn, {sections});
            }
        });
    });
}

describe("OP_MSG validation", function () {
    describe("mongod before authentication", function () {
        const fixture = {};
        before(function () {
            fixture.conn = MongoRunner.runMongod({auth: ""});
            fixture.exhaustHelloWithoutMaxAwaitTimeCode = 51756;
        });
        after(function () {
            MongoRunner.stopMongod(fixture.conn);
        });

        definePreAuthCases(fixture);
    });

    describe("mongos before authentication", function () {
        const fixture = {};
        before(function () {
            fixture.st = new ShardingTest({
                shards: 1,
                mongos: 1,
                config: 1,
                keyFile: "jstests/libs/key1",
            });
            fixture.conn = fixture.st.s;
            fixture.exhaustHelloWithoutMaxAwaitTimeCode = 51763;
        });
        after(function () {
            fixture.st.stop();
        });

        definePreAuthCases(fixture);
    });

    describe("mongod without authentication", function () {
        const fixture = {};
        before(function () {
            fixture.conn = MongoRunner.runMongod();
        });
        after(function () {
            MongoRunner.stopMongod(fixture.conn);
        });

        it("accepts a message over the pre-auth size limit", function () {
            const request = {sections: [hello()], padMessageTo: kPreAuthMaxMessageSize + 1};
            assertReplyOk(sendThenPing(fixture.conn, request));
        });

        it("accepts a body at the size limit", function () {
            const request = {sections: [body({hello: 1, $padTo: kMaxBodySize})]};
            assertReplyOk(sendThenPing(fixture.conn, request));
        });

        it("replies with an error on a body over the size limit", function () {
            const request = {sections: [body({hello: 1, $padTo: kMaxBodySize + 1})]};
            assertReplyError(sendThenPing(fixture.conn, request), ErrorCodes.BSONObjectTooLarge);
        });

        it("accepts document sequences for commands that support them", function () {
            const insert = {
                sections: [body({insert: "coll"}), docSequence("documents", [{_id: 1}, {_id: 2}])],
            };
            assert.eq(assertReplyOk(sendThenPing(fixture.conn, insert)).doc.n, 2);

            // bulkWrite reads two document sequences.
            const bulkWrite = {
                sections: [
                    body({bulkWrite: 1}),
                    docSequence("ops", [{insert: 0, document: {_id: 3}}]),
                    docSequence("nsInfo", [{ns: "admin.coll"}]),
                ],
            };
            assert.eq(assertReplyOk(sendThenPing(fixture.conn, bulkWrite)).doc.nInserted, 1);

            assert.eq(fixture.conn.getDB("admin").coll.countDocuments({}), 3);
            assert(fixture.conn.getDB("admin").coll.drop());
        });

        it("replies with an error on a document sequence document over the size limit", function () {
            // The command does not exist, so only the parser can fail the request with
            // BSONObjectTooLarge. A document under the internal limit would get CommandNotFound.
            const request = {
                sections: [
                    body({notARealCommand: 1}),
                    docSequence("documents", [{_id: 1, $padTo: kMaxInternalBSONSize + 1}]),
                ],
            };
            assertReplyError(sendThenPing(fixture.conn, request), ErrorCodes.BSONObjectTooLarge);
        });
    });
});
