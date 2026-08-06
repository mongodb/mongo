/**
 * Regression test for SERVER-130988.
 *
 * For each operation that can be run by-UUID (count, distinct, find, listIndexes), we assert that
 * the result is correct when the UUID is commit pending in the catalog, for DDLs that affect the
 * UUID to namespace mapping (create, drop, rename).
 *
 * Tests with authz on/off, because the original bug involved authz UUID to namespace resolution
 * causing incorrectly returning NamespaceNotFound.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collName = "coll";
const srcName = "src_coll";
const dstName = "dst_coll";
const resultsCollName = "probe_results";

// Delay between the failpoint latching (DDL parked with the catalog unpublished) and its
// release. This is a best effort to keep the test catching regressions, as perfectly synchronizing
// is not possible as the fix involves blocking authz checks until the catalog is published.
const kHoldMs = 1000;

function startSet(authOn) {
    const opts = authOn ? {nodes: 1, keyFile: "jstests/libs/key1"} : {nodes: 1};
    const rst = new ReplSetTest(opts);
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    if (authOn) {
        const admin = primary.getDB("admin");
        admin.createUser({user: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]});
        assert(admin.auth("admin", "admin"));
        admin.createRole({
            role: "useUUIDRole",
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ["useUUID"]}],
        });
        admin.getSiblingDB(dbName).createUser({
            user: "u",
            pwd: "p",
            roles: [
                {role: "useUUIDRole", db: "admin"},
                {role: "read", db: dbName},
                {role: "readWrite", db: dbName},
            ],
        });
    }
    return rst;
}

function getUUID(db, nss) {
    return assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: nss}})).cursor
        .firstBatch[0].info.uuid;
}

function makeProbeCmds(uuid) {
    return {
        count: {count: uuid},
        distinct: {distinct: uuid, key: "k"},
        find: {find: uuid},
        listIndexes: {listIndexes: uuid},
    };
}

// Runs in the probe shell: executes one by-UUID command and records the result document.
// JSON.stringify uses BSON toJSON, so the stored res stays BSON-readable.
function probeAndRecord(host, probeDbName, resultsColl, cmdObj, resultId, authOn) {
    const d = new Mongo(host).getDB(probeDbName);
    if (authOn) {
        assert(d.auth("u", "p"));
    }
    const res = d.runCommand(cmdObj);
    d.getCollection(resultsColl).insert({
        _id: resultId,
        res: JSON.parse(JSON.stringify(res)),
    });
}

// Starts one probe shell per command. Returns [{cmdName, awaitShell}].
function startProbes(primary, probeDbName, uuid, authOn, tag) {
    const awaits = [];
    for (const [cmdName, cmdObj] of Object.entries(makeProbeCmds(uuid))) {
        const awaitShell = startParallelShell(
            funWithArgs(
                probeAndRecord,
                primary.host,
                probeDbName,
                resultsCollName,
                cmdObj,
                `${tag}:${cmdName}`,
                authOn,
            ),
            primary.port,
        );
        awaits.push({cmdName, awaitShell});
    }
    return awaits;
}

function startDDL(primary, dbName, adminCommand, ddlCmdObj, authOn) {
    // Runs DDL in shell while failpoint parks it before catalog publish.
    function runDDL(dbName, adminCommand, ddlCmdObj, authOn) {
        const admin = db.getSiblingDB("admin");
        if (authOn) {
            assert(admin.auth("admin", "admin"));
        }
        const target = adminCommand ? admin : db.getSiblingDB(dbName);
        const run = adminCommand ? (c) => target.adminCommand(c) : (c) => target.runCommand(c);
        assert.commandWorked(run(ddlCmdObj));
    }

    return startParallelShell(
        funWithArgs(runDDL, dbName, adminCommand, ddlCmdObj, authOn),
        primary.port,
    );
}

// Releases the failpoint after kHoldMs, awaits the probe shells and the DDL, and returns the
// recorded result documents keyed by command name.
function collectProbes(setupDb, fp, awaits, tag, awaitDDL) {
    sleep(kHoldMs);
    fp.off();
    for (const {awaitShell} of awaits) {
        awaitShell();
    }
    awaitDDL();

    const out = {};
    for (const {cmdName} of awaits) {
        const doc = setupDb.getCollection(resultsCollName).findOne({_id: `${tag}:${cmdName}`});
        assert(doc, `${tag}:${cmdName}: probe never wrote a result`);
        out[cmdName] = doc.res;
    }
    return out;
}

function nsNotFound(res, label) {
    assert.commandFailedWithCode(
        res,
        ErrorCodes.NamespaceNotFound,
        `${label}: expected NamespaceNotFound, got ${tojson(res)}`,
    );
}

for (const authOn of [false, true]) {
    jsTest.log(`===== auth ${authOn ? "ON" : "OFF"} =====`);
    const rst = startSet(authOn);
    const primary = rst.getPrimary();
    const setupDb = primary.getDB(dbName);
    if (authOn) {
        // The root user lives on the admin db; auth there, then use the sibling test db.
        assert(primary.getDB("admin").auth("admin", "admin"));
    }
    setupDb.createCollection("dummy"); // Ensure the db exists.
    setupDb.createCollection(resultsCollName);

    // ---------- DDL: create (populated while pending) ----------
    {
        const tag = `create:auth${authOn ? "on" : "off"}`;
        const fp = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
            collectionNS: `${dbName}.${collName}`,
        });
        const awaitDDL = startDDL(
            primary,
            dbName,
            false /*adminCommand*/,
            {create: collName},
            authOn,
        );
        fp.wait();
        const uuid = getUUID(setupDb, collName);
        assert.commandWorked(setupDb.getCollection(collName).insert({k: "v1"}));

        const awaits = startProbes(primary, dbName, uuid, authOn, tag);
        const results = collectProbes(setupDb, fp, awaits, tag, awaitDDL);

        // Expect an OK result for all. The pending commit create UUID should be resolved.
        for (const [cmdName, res] of Object.entries(results)) {
            assert(res.ok, `${tag}:${cmdName}: expected ok, got ${tojson(res)}`);
        }
        assert.eq(results.count.n, 1, `${tag}: count`);
        assert.sameMembers(results.distinct.values, ["v1"], `${tag}: distinct`);
        assert.eq(results.find.cursor.firstBatch.length, 1, `${tag}: find`);
        assert.gte(results.listIndexes.cursor.firstBatch.length, 1, `${tag}: listIndexes`);
        jsTest.log(`${tag}: OK`);
    }

    // ---------- DDL: drop ----------
    {
        const tag = `drop:auth${authOn ? "on" : "off"}`;
        const uuid = getUUID(setupDb, collName);
        const fp = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
            collectionNS: `${dbName}.${collName}`,
        });
        const awaitDDL = startDDL(
            primary,
            dbName,
            false /*adminCommand*/,
            {drop: collName},
            authOn,
        );
        fp.wait();

        const awaits = startProbes(primary, dbName, uuid, authOn, tag);
        const results = collectProbes(setupDb, fp, awaits, tag, awaitDDL);

        // Expect a NamespaceNotFound result for all. The pending commit create UUID should fail to
        // resolve.
        for (const [cmdName, res] of Object.entries(results)) {
            nsNotFound(res, `${tag}:${cmdName}`);
        }
        jsTest.log(`${tag}: OK`);
    }

    // ---------- DDL: rename ----------
    {
        const tag = `rename:auth${authOn ? "on" : "off"}`;
        assert.commandWorked(setupDb.createCollection(srcName));
        assert.commandWorked(setupDb.getCollection(srcName).insert([{k: "a"}, {k: "b"}]));
        const uuid = getUUID(setupDb, srcName);
        const fp = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
            collectionNS: `${dbName}.${srcName}`,
        });
        const awaitDDL = startDDL(
            primary,
            dbName,
            true /*adminCommand*/,
            {renameCollection: `${dbName}.${srcName}`, to: `${dbName}.${dstName}`},
            authOn,
        );
        fp.wait();

        const awaits = startProbes(primary, dbName, uuid, authOn, tag);
        const results = collectProbes(setupDb, fp, awaits, tag, awaitDDL);

        // Expect correct UUID resolution for all, and results should reflect the rename.
        for (const [cmdName, res] of Object.entries(results)) {
            assert(res.ok, `${tag}:${cmdName}: expected ok, got ${tojson(res)}`);
        }
        assert.eq(results.count.n, 2, `${tag}: count`);
        assert.sameMembers(results.distinct.values.sort(), ["a", "b"], `${tag}: distinct`);
        assert.eq(setupDb.getCollection(dstName).count({}), 2, `${tag}: dst populated`);
        assert.eq(setupDb.getCollection(srcName).exists(), null, `${tag}: src gone`);
        jsTest.log(`${tag}: OK`);
    }

    rst.stopSet();
}
