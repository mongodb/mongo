/**
 * Tests for serverStatus metrics.commands.{create,collMod}.validationLevel stats.
 * Mirrors the validator counter tests in server_status_metric_validator_and_json_schema.js.
 * @tags: [requires_sharding, requires_fcv_90]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getMetrics(db) {
    const ss = assert.commandWorked(db.adminCommand({serverStatus: 1}));
    return {
        create: ss.metrics.commands.create.validationLevel,
        collMod: ss.metrics.commands.collMod.validationLevel,
    };
}

// Asserts that no create validationLevel counter changes between snap and getMetrics(db).
function assertNoCreateCounterChange(db, snap, msg) {
    const result = getMetrics(db);
    for (const level of ["default", "strict", "moderate", "off", "constraint"]) {
        assert.eq(delta(snap, result, "create", level), 0, `${msg}: ${level} counter must not change`);
    }
}

function delta(before, after, cmd, level) {
    return after[cmd][level] - before[cmd][level];
}

function defineTests(getDb) {
    const collName = jsTestName() + "_collmod";
    const viewName = jsTestName() + "_view";
    const tsName = jsTestName() + "_ts";

    beforeEach(function () {
        const db = getDb();
        db[collName].drop();
        db[viewName].drop();
        db[tsName].drop();
        assert.commandWorked(db.createCollection(collName, {validator: {a: {$type: "string"}}}));
        assert.commandWorked(db.createView(viewName, collName, []));
        assert.commandWorked(db.createCollection(tsName, {timeseries: {timeField: "t", metaField: "m"}}));
    });

    afterEach(function () {
        const db = getDb();
        db[collName].drop();
        db[viewName].drop();
        db[tsName].drop();
    });

    for (const level of ["strict", "moderate", "off"]) {
        it(`create with explicit validationLevel '${level}' increments counter`, function () {
            const db = getDb();
            const coll = jsTestName() + "_" + level;
            db[coll].drop();
            const snap = getMetrics(db);
            assert.commandWorked(
                db.createCollection(coll, {validator: {a: {$type: "string"}}, validationLevel: level}),
            );
            assert.eq(
                delta(snap, getMetrics(db), "create", level),
                1,
                `expected create ${level} counter to increment by 1`,
            );
            db[coll].drop();
        });
    }

    it("create with validator but no explicit validationLevel increments default counter", function () {
        const db = getDb();
        const coll = jsTestName() + "_no_level";
        db[coll].drop();
        const snap = getMetrics(db);
        assert.commandWorked(db.createCollection(coll, {validator: {b: {$type: "int"}}}));
        const result = getMetrics(db);
        assert.eq(
            delta(snap, result, "create", "default"),
            1,
            "no-level create with validator must increment default counter",
        );
        for (const level of ["strict", "moderate", "off"]) {
            assert.eq(delta(snap, result, "create", level), 0, `no-level create must not affect ${level} counter`);
        }
        db[coll].drop();
    });

    it("create without validator and without validationLevel does not increment any counter", function () {
        const db = getDb();
        const coll = jsTestName() + "_bare";
        db[coll].drop();
        const snap = getMetrics(db);
        assert.commandWorked(db.createCollection(coll));
        assertNoCreateCounterChange(db, snap, "bare create");
        db[coll].drop();
    });

    it("failing create does not increment any counter", function () {
        const db = getDb();
        const snap = getMetrics(db);
        assert.commandFailed(
            db.createCollection(jsTestName() + "_fail", {validator: {$invalidOp: 1}, validationLevel: "strict"}),
        );
        assertNoCreateCounterChange(db, snap, "failed create");
    });

    for (const level of ["off", "moderate", "strict"]) {
        it(`collMod with explicit validationLevel '${level}' increments counter`, function () {
            const db = getDb();
            const snap = getMetrics(db);
            assert.commandWorked(db.runCommand({collMod: collName, validationLevel: level}));
            assert.eq(
                delta(snap, getMetrics(db), "collMod", level),
                1,
                `expected collMod ${level} counter to increment by 1`,
            );
        });
    }

    it("rejected collMod on view does not increment counter", function () {
        const db = getDb();
        const snap = getMetrics(db);
        assert.commandFailed(db.runCommand({collMod: viewName, validationLevel: "strict"}));
        assert.eq(
            delta(snap, getMetrics(db), "collMod", "strict"),
            0,
            "rejected collMod on view must not increment counter",
        );
    });

    it("rejected collMod on timeseries does not increment counter", function () {
        const db = getDb();
        const snap = getMetrics(db);
        assert.commandFailed(db.runCommand({collMod: tsName, validationLevel: "strict"}));
        assert.eq(
            delta(snap, getMetrics(db), "collMod", "strict"),
            0,
            "rejected collMod on timeseries must not increment counter",
        );
    });

    it("collMod without validationLevel does not increment any counter", function () {
        const db = getDb();
        const snap = getMetrics(db);
        assert.commandWorked(db.runCommand({collMod: collName, validator: {a: {$type: "int"}}}));
        const result = getMetrics(db);
        for (const level of ["strict", "moderate", "off"]) {
            assert.eq(
                delta(snap, result, "collMod", level),
                0,
                `collMod without validationLevel must not affect ${level} counter`,
            );
        }
    });

    it("constraint level increments counter (feature-flag gated)", function () {
        const db = getDb();
        if (!FeatureFlagUtil.isPresentAndEnabled(db, "ConstraintValidationLevel")) {
            return;
        }
        const coll = jsTestName() + "_constraint";
        db[coll].drop();

        const snap = getMetrics(db);
        assert.commandWorked(
            db.createCollection(coll, {validator: {a: {$type: "string"}}, validationLevel: "constraint"}),
        );
        assert.eq(
            delta(snap, getMetrics(db), "create", "constraint"),
            1,
            "expected create constraint counter to increment by 1",
        );

        // Downgrade to strict so we can do the prepare+upgrade cycle to test the collMod counter.
        assert.commandWorked(db.runCommand({collMod: coll, validationLevel: "strict"}));
        assert.commandWorked(db.runCommand({collMod: coll, prepareConstraintValidationLevel: true}));

        const snap2 = getMetrics(db);
        assert.commandWorked(db.runCommand({collMod: coll, validationLevel: "constraint"}));
        assert.eq(
            delta(snap2, getMetrics(db), "collMod", "constraint"),
            1,
            "expected collMod constraint counter to increment by 1",
        );

        db[coll].drop();
    });
}

describe("standalone", function () {
    let conn;

    before(function () {
        conn = MongoRunner.runMongod({});
        assert.neq(conn, null, "mongod failed to start");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    defineTests(() => conn.getDB(jsTestName()));
});

// Query shard primary directly so serverStatus reflects shard-role counters.
describe("sharded", function () {
    let st;

    before(function () {
        st = new ShardingTest({shards: 2});
        assert.commandWorked(st.s.adminCommand({enableSharding: jsTestName(), primaryShard: st.shard0.shardName}));
    });

    after(function () {
        st.stop();
    });

    defineTests(() => st.rs0.getPrimary().getDB(jsTestName()));

    it("createCollection through mongos increments counter exactly once across all shards", function () {
        const fanoutDbName = jsTestName() + "_create_fanout";
        const fanoutColl = "coll";
        const fanoutDb = st.s.getDB(fanoutDbName);

        fanoutDb[fanoutColl].drop();

        function totalCreateStrict() {
            const m0 = assert.commandWorked(st.rs0.getPrimary().adminCommand({serverStatus: 1})).metrics.commands.create
                .validationLevel.strict;
            const m1 = assert.commandWorked(st.rs1.getPrimary().adminCommand({serverStatus: 1})).metrics.commands.create
                .validationLevel.strict;
            return m0 + m1;
        }

        const snap = totalCreateStrict();
        assert.commandWorked(
            fanoutDb.createCollection(fanoutColl, {
                validator: {a: {$type: "string"}},
                validationLevel: "strict",
            }),
        );
        st.shardColl(fanoutDb[fanoutColl], {_id: 1}, {_id: 0}, {_id: 0});
        assert.eq(
            totalCreateStrict() - snap,
            1,
            "createCollection through mongos must increment the counter exactly once across all shards",
        );

        fanoutDb[fanoutColl].drop();
    });

    it("collMod through mongos increments counter exactly once across all shards", function () {
        // Use a dedicated database to avoid conflicts with the beforeEach direct-shard setup.
        const fanoutDbName = jsTestName() + "_fanout";
        const fanoutColl = "coll";
        const fanoutDb = st.s.getDB(fanoutDbName);

        fanoutDb[fanoutColl].drop();
        assert.commandWorked(fanoutDb.createCollection(fanoutColl, {validator: {a: {$type: "string"}}}));
        st.shardColl(fanoutDb[fanoutColl], {_id: 1}, {_id: 0}, {_id: 0});

        function totalCollModStrict() {
            const m0 = assert.commandWorked(st.rs0.getPrimary().adminCommand({serverStatus: 1})).metrics.commands
                .collMod.validationLevel.strict;
            const m1 = assert.commandWorked(st.rs1.getPrimary().adminCommand({serverStatus: 1})).metrics.commands
                .collMod.validationLevel.strict;
            return m0 + m1;
        }

        const snap = totalCollModStrict();
        assert.commandWorked(fanoutDb.runCommand({collMod: fanoutColl, validationLevel: "strict"}));
        assert.eq(
            totalCollModStrict() - snap,
            1,
            "collMod through mongos must increment the counter exactly once across all shards",
        );

        fanoutDb[fanoutColl].drop();
    });
});
