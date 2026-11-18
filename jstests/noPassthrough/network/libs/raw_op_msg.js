/**
 * Helpers for sending hand-built OP_MSG requests to a server, for testing OP_MSG validation.
 *
 * The requests are put on the wire by raw_op_msg_client.py, which documents the step and section
 * formats accepted by sendRawOpMsg().
 */

import {getPython3Binary} from "jstests/libs/python.js";

export const kChecksumPresent = 1 << 0;
export const kMoreToCome = 1 << 1;
export const kExhaustAllowed = 1 << 16;

// A traceparent that passes W3C validation.
export const kValidTraceparent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

// The document {a: 1} with its element type changed to the undefined type 0x99.
export const kInvalidBSONHex = "0c000000" + "99" + "6100" + "01000000" + "00";

/** Returns a kind 0 (body) section for `cmd`, adding the $db field every OP_MSG request needs. */
export function body(cmd) {
    return {kind: 0, doc: {...cmd, $db: "admin"}};
}

/** Returns a kind 1 (document sequence) section. `size` overrides the encoded size field. */
export function docSequence(name, docs, size) {
    return size === undefined ? {kind: 1, name, docs} : {kind: 1, name, docs, size};
}

/** Returns a kind 3 (telemetry context) section. */
export function telemetry(doc = {otel: {traceparent: kValidTraceparent}}) {
    return {kind: 3, doc};
}

/** Returns a section made of `kind` followed by the bytes in `hex`. */
export function rawSection(kind, hex) {
    return {kind, hex};
}

let python;
let runCounter = 0;

/**
 * Sends each step in `steps` in order on one new connection to `conn`, and returns one result per
 * step. See raw_op_msg_client.py for the step and result formats.
 */
export function sendRawOpMsg(conn, steps) {
    python ??= getPython3Binary();

    const [host, port] = conn.host.split(":");
    const prefix = `${MongoRunner.dataPath}raw_op_msg_${runCounter++}`;
    const specFile = prefix + "_spec.json";
    const outFile = prefix + "_out.json";
    writeFile(specFile, JSON.stringify({steps}));

    const exitCode = runNonMongoProgram(
        python,
        "-u",
        "jstests/noPassthrough/network/libs/raw_op_msg_client.py",
        "--host",
        host,
        "--port",
        port,
        "--spec",
        specFile,
        "--out",
        outFile,
    );
    assert.eq(exitCode, 0, "raw_op_msg_client.py failed", {steps});

    const results = JSON.parse(cat(outFile)).steps;
    assert.eq(results.length, steps.length, "missing step results", {results});
    for (const result of results) {
        assert(!result.timedOut, "timed out waiting for a reply", {steps, results});
    }
    return results;
}

/** Asserts that `result` holds exactly one successful reply, and returns that reply. */
export function assertReplyOk(result) {
    assert(!result.closed, "connection was unexpectedly closed", {result});
    assert.eq(result.replies.length, 1, "expected exactly one reply", {result});
    const reply = result.replies[0];
    assert.eq(reply.responseTo, result.requestId, "reply is not for this request", {result});
    assert.commandWorked(reply.doc);
    return reply;
}

/** Asserts that `result` holds exactly one error reply with `code`. */
export function assertReplyError(result, code) {
    assert(!result.closed, "connection was unexpectedly closed", {result});
    assert.eq(result.replies.length, 1, "expected exactly one reply", {result});
    assert.eq(result.replies[0].responseTo, result.requestId, "reply is not for this request", {
        result,
    });
    assert.commandFailedWithCode(result.replies[0].doc, code);
}

/** Asserts that the server closed the connection without replying to the request. */
export function assertClosedWithoutReply(result) {
    assert(result.closed, "expected the server to close the connection", {result});
    assert.eq(result.replies.length, 0, "expected no reply before the connection closed", {result});
}
