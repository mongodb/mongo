/**
 * Tests that the host resolves an extension command's target namespace and passes the collection
 * UUID, source namespace, and view definition in the command context. Uses testEcho from the
 * mock_commands extension, which returns the command context it received.
 *
 * @tags: [featureFlagExtensionsAPI, featureFlagExtensionCommands]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {getUUIDFromListCollections, extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function uuidOf(db, coll) {
    return extractUUIDFromObject(getUUIDFromListCollections(db, coll));
}

function assertContext(db, coll, {uuid, source, view}) {
    const res = assert.commandWorked(db.runCommand({testEcho: coll}));
    assert.eq(res.db, db.getName(), {res});
    assert.eq(res.coll, coll, {res});
    assert.eq(res.uuid, uuid, {res});
    assert.eq(res.source, {db: db.getName(), coll: source ?? coll}, {res});
    assert.eq(res.view, view, {res});
}

const kResolveErrorPrefix = "Failed to resolve the target namespace";

function testContext(conn) {
    const db = conn.getDB(jsTestName());
    const otherDb = conn.getDB(jsTestName() + "_other");
    assert.commandWorked(db.createCollection("coll"));
    assert.commandWorked(otherDb.createCollection("coll"));
    assert.commandWorked(db.createView("view", "coll", [{$match: {a: 1}}]));
    assert.commandWorked(db.createView("viewOnView", "view", [{$project: {b: 0}}]));
    assert.commandWorked(db.createView("viewOnMissing", "missing", []));
    assert.commandWorked(db.createCollection("ts", {timeseries: {timeField: "t"}}));
    assert.commandWorked(db.createView("viewOnTs", "ts", []));

    const collUuid = uuidOf(db, "coll");

    assertContext(db, "coll", {uuid: collUuid});
    assertContext(otherDb, "coll", {uuid: uuidOf(otherDb, "coll")});

    // A view passes its source collection's UUID and its pipeline.
    assertContext(db, "view", {
        uuid: collUuid,
        source: "coll",
        view: {namespace: {db: db.getName(), coll: "view"}, pipeline: [{$match: {a: 1}}]},
    });

    // A view on a view passes the underlying collection and the combined pipeline.
    assertContext(db, "viewOnView", {
        uuid: collUuid,
        source: "coll",
        view: {
            namespace: {db: db.getName(), coll: "viewOnView"},
            pipeline: [{$match: {a: 1}}, {$project: {b: 0}}],
        },
    });

    // A view whose source collection does not exist passes an empty UUID with the view.
    assertContext(db, "viewOnMissing", {
        uuid: "",
        source: "missing",
        view: {namespace: {db: db.getName(), coll: "viewOnMissing"}, pipeline: []},
    });

    // A missing collection, or a collection in a missing database, passes an empty UUID. A router
    // fails to route to a missing database, which the host treats the same as a missing collection.
    assertContext(db, "missing", {uuid: ""});
    const missingDb = conn.getDB(jsTestName() + "_missing");
    assertContext(missingDb, "coll", {uuid: ""});

    // A timeseries collection, or a view on one, is rejected before the extension runs.
    for (const coll of ["ts", "viewOnTs"]) {
        const res = assert.commandFailedWithCode(
            db.runCommand({testEcho: coll}),
            [10840700, 10840701, 13717000, 13717001],
        );
        assert(res.errmsg.includes(kResolveErrorPrefix), "unexpected error", {res});
    }
}

describe("extension command context", function () {
    it("passes the resolved namespace, UUID, and view", function () {
        withExtensions({"libmock_commands_extension.so": {}}, testContext);
    });
});
