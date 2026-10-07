/**
 * Tests the remediation path for documents that a platform's libm makes degenerate
 * (SERVER-129401 / SERVER-132874): a collection holding such documents cannot get a 2dsphere index
 * while geoRecoveryFromSinCosRoundingErrors is off, and it recovers by enabling the parameter
 * and resyncing the affected members.
 *
 * Resyncing a member means removing its data and letting it run an initial sync, so what the two
 * resync cases below really compare is the state initial sync leaves behind with recovery off
 * versus on. Both use the default logical initial sync method.
 *
 * Whether these polygons are degenerate at all depends on the platform's libm, so the test records
 * up front (before any describe() block runs) which polygons this platform rejects, and both cases
 * do nothing if this platform is unaffected.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {
    findPolygonsRejectedWithoutRecovery,
    kRecoverablePolygons,
} from "jstests/libs/query/geo_sin_cos_rounding_polygons.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = jsTestName();
const kCollName = "degenerate_polygons";
const kIndexName = "geo_2dsphere";

function restartWithRecovery(rst, nodeId, enabled, options = {}) {
    return rst.restart(rst.nodes[nodeId], {
        ...options,
        setParameter: {
            ...(options.setParameter || {}),
            geoRecoveryFromSinCosRoundingErrors: enabled,
        },
    });
}

function setRecovery(rst, enabled) {
    // The secondary is restarted first so that the primary keeps the set writable for as long as
    // possible.
    for (const nodeId of [1, 0]) {
        restartWithRecovery(rst, nodeId, enabled);
    }
    // ReplSetTest runs with an effectively infinite election timeout, so the restarted primary is
    // never re-elected on its own; step it up explicitly.
    rst.stepUp(rst.nodes[0], {awaitReplicationBeforeStepUp: false});
    rst.awaitSecondaryNodes();
    for (const node of rst.nodes) {
        const res = assert.commandWorked(
            node.adminCommand({getParameter: 1, geoRecoveryFromSinCosRoundingErrors: 1}),
        );
        assert.eq(enabled, res.geoRecoveryFromSinCosRoundingErrors, "parameter not applied", {
            host: node.host,
            res,
        });
    }
}

// Wipes the secondary's data directory and lets it run a logical initial sync from the primary.
function resyncSecondary(rst, recoveryEnabled) {
    const secondary = restartWithRecovery(rst, 1, recoveryEnabled, {
        startClean: true,
        setParameter: {initialSyncMethod: "logical"},
    });
    rst.awaitSecondaryNodes(null, [secondary]);
    secondary.setSecondaryOk();
    return secondary;
}

// Record which polygons this platform's libm actually makes degenerate, as a module-level value
// computed from a throwaway mongod *before* any describe() block runs.
const probeConn = MongoRunner.runMongod({
    setParameter: {geoRecoveryFromSinCosRoundingErrors: false},
});
const rejectedWithoutRecovery = findPolygonsRejectedWithoutRecovery(probeConn.getDB(jsTestName()));
MongoRunner.stopMongod(probeConn);

describe("sin/cos rounding recovery remediation by resync", function () {
    let rst;
    // Whether before() got as far as initiating the set; read by after().
    let setIsInitiated = false;
    // Geo key count of a correctly built index, recorded by the recovery-enabled case and used as
    // the baseline by the disabled one.
    let healthyGeoKeyCount = 0;

    function collOnPrimary() {
        return rst.getPrimary().getDB(kDbName)[kCollName];
    }

    before(function () {
        rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0}}],
            nodeOptions: {setParameter: {geoRecoveryFromSinCosRoundingErrors: false}},
        });
        rst.startSet();
        rst.initiate();
        setIsInitiated = true;

        if (rejectedWithoutRecovery.size === 0) {
            jsTest.log.info(
                "This platform does not make kRecoverablePolygons degenerate; nothing to remediate",
            );
            return;
        }

        const docs = kRecoverablePolygons.map(({name, geo}, i) => ({_id: i, name: name, geo: geo}));
        assert.commandWorked(collOnPrimary().insert(docs, {writeConcern: {w: "majority"}}));
        assert.eq(kRecoverablePolygons.length, collOnPrimary().find().itcount());
        // Building the index is where the degenerate geometry gets rejected.
        assert.commandFailedWithCode(
            collOnPrimary().createIndex({geo: "2dsphere"}, {name: kIndexName}),
            ErrorCodes.GeoKeyExtractionFailed,
        );

        // Put the set into the remediated state with recovery enabled, so the
        // primary can build the index that initial sync then clones.
        setRecovery(rst, true);
        assert.commandWorked(collOnPrimary().createIndex({geo: "2dsphere"}, {name: kIndexName}));
        rst.awaitReplication();
    });

    after(function () {
        if (!rst) {
            return;
        }
        // The collection only exists once before() got past initiate(); dropping it before
        // stopSet() keeps teardown's validation off the index the last case broke on purpose.
        // Guarded so that a failure earlier in before() surfaces as itself rather than as a
        // TypeError here, and so the mongods are always reaped.
        if (setIsInitiated) {
            collOnPrimary().drop();
            rst.awaitReplication();
        }
        rst.stopSet();
    });

    // The remediation itself: with recovery enabled on both members, initial sync clones the
    // collection and builds a usable 2dsphere index over the degenerate documents.
    it("indexes every document when the member resyncs with recovery enabled", function () {
        if (rejectedWithoutRecovery.size === 0) {
            return;
        }
        const coll = resyncSecondary(rst, true).getDB(kDbName)[kCollName];
        rst.awaitReplication();

        assert.eq(kRecoverablePolygons.length, coll.find().itcount(), "resync lost documents");
        assert(
            coll.getIndexes().some((ix) => ix.name === kIndexName),
            "resynced node is missing the 2dsphere index",
            {indexes: coll.getIndexes()},
        );
        const validateRes = assert.commandWorked(coll.validate({full: true}));
        assert(validateRes.valid, "expected a valid collection after the resync", {validateRes});
        // A 2dsphere index emits a cell covering per document rather than a single key, so the
        // absolute count is not interesting; what matters is that keys exist, and that this is the
        // complete count the disabled case below is compared against.
        assert.gt(validateRes.keysPerIndex[kIndexName], 0, "expected geo keys", {validateRes});
        healthyGeoKeyCount = validateRes.keysPerIndex[kIndexName];

        // Every document must be reachable through the resynced index, using its own polygon as the
        // query operand.
        for (const {name, geo} of kRecoverablePolygons) {
            const found = coll
                .find({geo: {$geoIntersects: {$geometry: geo}}})
                .hint(kIndexName)
                .toArray();
            assert(
                found.some((doc) => doc.name === name),
                `${name}: not found through the index on the resynced node`,
                {found},
            );
        }
    });

    it("leaves an inconsistent geo index when the member resyncs with recovery disabled", function () {
        if (rejectedWithoutRecovery.size === 0) {
            return;
        }
        const coll = resyncSecondary(rst, false).getDB(kDbName)[kCollName];
        // Initial sync itself reports success and brings over every document and the index...
        assert.eq(kRecoverablePolygons.length, coll.find().itcount());
        assert(
            coll.getIndexes().some((ix) => ix.name === kIndexName),
            "expected initial sync to create the index even without recovery",
            {indexes: coll.getIndexes()},
        );

        // ...but the member is still running without recovery, so its 2dsphere index cannot be
        // rematerialized: validate() re-extracts the geo keys and fails for the degenerate polygons
        // ("Could not build key for index ... BadValue"), flagging the collection as corrupt.
        const validateRes = assert.commandWorked(coll.validate({full: true}));
        assert(!validateRes.valid, "expected validate to reject the inconsistent geo index", {
            validateRes,
        });
        assert.gt(healthyGeoKeyCount, 0, "the recovery-enabled case must run first");
        assert.eq(
            kRecoverablePolygons.length,
            validateRes.nrecords,
            "the documents themselves must survive",
            {validateRes},
        );
        assert(
            validateRes.errors.some((e) =>
                e.includes(`Could not build key for index ${kIndexName}`),
            ),
            "expected a key generation error",
            {errors: validateRes.errors},
        );
    });
});
