/**
 * Tests the MaxKey orphan range-deletion guard.
 *
 * The guard classifies, once per epoch at shard-primary step-up, the pending range-deletion tasks
 * that still contain an unrecoverable pre-upgrade MaxKey orphan, recording their ids in
 * config.maxKeyOrphanScanState.blockedTasks. While skipRangeDeletionForMaxKeyChunks is enabled
 * for those tasks the range deleter deletes the ordinary documents in the range but
 * preserves those whose leading shard-key field is MaxKey (left on disk for out-of-band recovery);
 * the task then completes normally. Setting the parameter off forces full deletion. Tasks created
 * after classification (post-upgrade) are not in the set and delete normally.
 *
 * @tags: [
 *   featureFlagMaxKeyOrphanGuard,
 *   requires_persistence,
 *   does_not_support_stepdowns,
 *   multiversion_incompatible,
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {before, after, describe, it} from "jstests/libs/mochalite.js";

// The guard intentionally holds MaxKey orphans on shard0 while it is enabled.
TestData.skipCheckOrphans = true;

const guardSkipLogId = 13018000;
const scanStateId = "scanState";

describe("range deleter MaxKey orphan guard", function () {
    let st;
    let adminDB;
    let testDB;
    let dbName;
    let suspendFps;

    function primary() {
        return st.rs0.getPrimary();
    }

    function rangeDeletions() {
        return primary().getDB("config").getCollection("rangeDeletions");
    }

    function scanState() {
        return primary().getDB("config").getCollection("maxKeyOrphanScanState").findOne({
            _id: scanStateId,
        });
    }

    // FTDC/serverStatus counter of range-deletion tasks for which the guard preserved MaxKey docs,
    // summed across the shard's nodes. The counter is per-process, so a stepUp moves which node
    // increments; summing keeps the total stable across failovers.
    function guardedTaskCount() {
        return st.rs0.nodes.reduce(
            (sum, n) =>
                sum +
                n.getDB("admin").serverStatus().shardingStatistics
                    .countRangeDeletionTasksPreservingMaxKeyOrphans,
            0,
        );
    }

    // Enables (value=true) or disables (value=false) the guard on every rs0 node by toggling the
    // skipRangeDeletionForMaxKeyChunks server parameter. Enabled => blocked tasks preserve their
    // MaxKey-prefixed docs; disabled => the range deleter deletes them normally (the escape hatch).
    function setGuardEnabled(value) {
        for (const node of st.rs0.nodes) {
            assert.commandWorked(
                node.adminCommand({setParameter: 1, skipRangeDeletionForMaxKeyChunks: value}),
            );
        }
    }

    // Suspends/resumes range deletion on every rs0 node so the window between creating a task and
    // (re)classifying it at step-up does not let the deleter drain it first.
    function suspendRangeDeletionEverywhere() {
        suspendFps = st.rs0.nodes.map((n) => configureFailPoint(n, "suspendRangeDeletion"));
    }
    function resumeRangeDeletionEverywhere() {
        suspendFps.forEach((fp) => fp.off());
        suspendFps = [];
    }

    // Forces a fresh classification epoch: clears the scan-state doc so 'blockedTasks' is absent,
    // then steps up the other node so the new primary reclassifies at step-up.
    function clearScanStateAndStepUp() {
        assert.commandWorked(
            primary()
                .getDB("config")
                .runCommand({
                    delete: "maxKeyOrphanScanState",
                    deletes: [{q: {_id: scanStateId}, limit: 0}],
                    writeConcern: {w: "majority"},
                }),
        );
        st.rs0.awaitReplication();
        const newPrimary = st.rs0.getSecondary();
        st.rs0.stepUp(newPrimary);
        st.rs0.waitForPrimary();
    }

    // Shards 'collName' on {a: 1}, inserts a MaxKey doc and a normal doc sharing the global-max
    // chunk, then moves that chunk to shard1 with deletion deferred, leaving a pending global-max
    // range-deletion task and a MaxKey orphan on shard0. Returns the collection UUID.
    function createGlobalMaxOrphanTask(collName) {
        const ns = `${dbName}.${collName}`;
        assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {a: 1}}));
        assert.commandWorked(testDB[collName].insert({a: MaxKey, payload: "maxkey-doc"}));
        assert.commandWorked(testDB[collName].insert({a: 50, payload: "normal-doc"}));
        assert.commandWorked(adminDB.runCommand({split: ns, middle: {a: 0}}));
        assert.commandWorked(
            adminDB.runCommand({
                moveChunk: ns,
                find: {a: 50},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        return testDB.getCollectionInfos({name: collName})[0].info.uuid;
    }

    // Compound shard key {a: 1, b: 1}. All docs land in the global-max chunk moved to shard1
    // (orphans on shard0); only the doc whose shard key is the global max is preserved:
    //   {a: MaxKey, b: MaxKey}: shard key is the global max -> preserved
    //   {a: MaxKey, b: 5}:      below the global max        -> deleted
    //   {a: MaxKey, b: MinKey}: below the global max        -> deleted
    //   {a: 5, b: MaxKey}:      below the global max        -> deleted
    //   {a: 50, b: 50}:         ordinary doc                -> deleted
    function createCompoundGlobalMaxOrphanTask(collName) {
        const ns = `${dbName}.${collName}`;
        assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {a: 1, b: 1}}));
        assert.commandWorked(
            testDB[collName].insertMany([
                {a: MaxKey, b: MaxKey, payload: "lead-max-trail-max"},
                {a: MaxKey, b: 5, payload: "lead-max-trail-num"},
                {a: MaxKey, b: MinKey, payload: "lead-max-trail-min"},
                {a: 5, b: MaxKey, payload: "lead-num-trail-max"},
                {a: 50, b: 50, payload: "normal-doc"},
            ]),
        );
        assert.commandWorked(adminDB.runCommand({split: ns, middle: {a: 0, b: MinKey}}));
        assert.commandWorked(
            adminDB.runCommand({
                moveChunk: ns,
                find: {a: 50, b: 50},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        return testDB.getCollectionInfos({name: collName})[0].info.uuid;
    }

    // Shard key {a: 1} backed by a wider {a: 1, b: 1} index; the {a: MaxKey, b: 10} orphan has shard
    // key {a: MaxKey} (the global max). Returns the collection UUID.
    function createWiderIndexGlobalMaxOrphanTask(collName) {
        const ns = `${dbName}.${collName}`;
        // Create the compound index up front so shardCollection reuses it rather than a {a: 1} index.
        assert.commandWorked(testDB[collName].createIndex({a: 1, b: 1}));
        assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {a: 1}}));
        assert.commandWorked(testDB[collName].insert({a: MaxKey, b: 10, payload: "wider-maxkey"}));
        assert.commandWorked(testDB[collName].insert({a: 50, b: 20, payload: "normal-doc"}));
        assert.commandWorked(adminDB.runCommand({split: ns, middle: {a: 0}}));
        assert.commandWorked(
            adminDB.runCommand({
                moveChunk: ns,
                find: {a: 50},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        return testDB.getCollectionInfos({name: collName})[0].info.uuid;
    }

    // Compound shard key {a: 1, b: 1} backed by a wider {a: 1, b: 1, c: 1} index. Only the doc whose
    // shard key is the global max is preserved:
    //   {a: MaxKey, b: MaxKey, c: 10}: shard key is the global max -> preserved
    //   {a: MaxKey, b: 1,      c: 5}:  below the global max        -> deleted
    //   {a: 50,     b: 50,     c: 50}: ordinary doc                -> deleted
    function createCompoundWiderIndexGlobalMaxOrphanTask(collName) {
        const ns = `${dbName}.${collName}`;
        // Create the wider index up front so shardCollection reuses it rather than a {a, b} index.
        assert.commandWorked(testDB[collName].createIndex({a: 1, b: 1, c: 1}));
        assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {a: 1, b: 1}}));
        assert.commandWorked(
            testDB[collName].insertMany([
                {a: MaxKey, b: MaxKey, c: 10, payload: "compound-wider-global-max"},
                {a: MaxKey, b: 1, c: 5, payload: "compound-wider-below-max"},
                {a: 50, b: 50, c: 50, payload: "normal-doc"},
            ]),
        );
        assert.commandWorked(adminDB.runCommand({split: ns, middle: {a: 0, b: MinKey}}));
        assert.commandWorked(
            adminDB.runCommand({
                moveChunk: ns,
                find: {a: 50, b: 50},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        return testDB.getCollectionInfos({name: collName})[0].info.uuid;
    }

    function taskIdFor(uuid) {
        const doc = rangeDeletions().findOne({collectionUuid: uuid, "range.max.a": MaxKey});
        assert.neq(null, doc, "expected a pending global-max range-deletion task");
        return doc._id;
    }

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 2},
            other: {enableBalancer: false},
            // Ready tasks immediately so the guard, not the cleanup delay, is what holds them.
            rsOptions: {setParameter: {orphanCleanupDelaySecs: 0}},
        });
        dbName = jsTestName();
        adminDB = st.s.getDB("admin");
        testDB = st.s.getDB(dbName);
        assert.commandWorked(
            adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st.stop();
    });

    // Classifies the pending task at step-up and asserts it landed in blockedTasks. Leaves range
    // deletion suspended so the caller controls when the task is processed.
    function classifyAtStepUp(collName, create = createGlobalMaxOrphanTask) {
        const uuid = create(collName);
        const taskId = taskIdFor(uuid);
        clearScanStateAndStepUp();
        assert.soon(() => {
            const doc = scanState();
            return (
                doc &&
                Array.isArray(doc.blockedTasks) &&
                doc.blockedTasks.some((id) => id.toString() === taskId.toString())
            );
        }, "expected the pending task to be classified into blockedTasks");
        return uuid;
    }

    it("deletes ordinary docs but preserves the MaxKey doc for a classified task", function () {
        setGuardEnabled(true); // Default; explicit for clarity.
        // Baseline before any processing: the guard decision (and counter increment) happens as soon
        // as the post-classification primary picks up the task, which can be before resume.
        const guardedBefore = guardedTaskCount();
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("guarded");

        // With the guard engaged, the task drains (completes) after deleting the ordinary doc, but
        // the possibly-never-cloned MaxKey doc is preserved.
        resumeRangeDeletionEverywhere();
        assert.soon(
            () => checkLog.checkContainsOnceJson(primary(), guardSkipLogId),
            "expected the preserve-MaxKey log",
        );
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "blocked task must complete (be removed)",
        );
        assert.eq(
            1,
            primary().getDB(dbName).getCollection("guarded").find({a: MaxKey}).itcount(),
            "MaxKey orphan must be preserved on disk",
        );
        assert.eq(
            0,
            primary().getDB(dbName).getCollection("guarded").find({a: 50}).itcount(),
            "ordinary orphan in the global-max chunk must be deleted",
        );
        assert.gt(
            guardedTaskCount(),
            guardedBefore,
            "guarded-task counter must advance for the guarded task",
        );
    });

    it("deletes a classified task normally when the guard is disabled", function () {
        // Disable the guard before classification/processing, so it never engages for this task (the
        // guard decision is made as soon as the post-classification primary picks the task up, which
        // can precede resume).
        setGuardEnabled(false);
        const guardedBefore = guardedTaskCount();
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("escapehatch");

        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "task must delete with the escape hatch off",
        );
        assert.eq(
            0,
            primary().getDB(dbName).getCollection("escapehatch").find({a: MaxKey}).itcount(),
            "orphan must be deleted with the escape hatch off",
        );
        // Normal deletion must not advance the guarded-task counter.
        assert.eq(
            guardedBefore,
            guardedTaskCount(),
            "guarded-task counter must not change on a normal deletion",
        );
        setGuardEnabled(true);
    });

    it("deletes a task created after classification, even with the guard on", function () {
        // blockedTasks is already present, so no reclassification runs for this new (post-upgrade)
        // task; it must delete normally despite the guard being on.
        setGuardEnabled(true);
        const guardedBefore = guardedTaskCount();
        const uuid = createGlobalMaxOrphanTask("postclassify");
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "a task created after classification must delete normally",
        );
        assert.eq(
            0,
            primary().getDB(dbName).getCollection("postclassify").find({a: MaxKey}).itcount(),
            "orphan must be deleted for an unclassified (post-upgrade) task",
        );
        assert.eq(
            guardedBefore,
            guardedTaskCount(),
            "an unclassified (post-upgrade) task must not advance the guarded-task counter",
        );
    });

    it("keeps a classified task blocked across a failover (rehydrate, no reclassify)", function () {
        setGuardEnabled(true);
        const guardedBefore = guardedTaskCount();
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("failover");
        const taskId = taskIdFor(uuid);
        const blockedAfterClassify = scanState().blockedTasks.map((id) => id.toString());

        // A second step-up (without clearing the state doc) must rehydrate the blocked set from the
        // doc rather than reclassify.
        st.rs0.stepUp(st.rs0.getSecondary());
        st.rs0.waitForPrimary();
        assert.soon(
            () => scanState() && Array.isArray(scanState().blockedTasks),
            "expected the blocked set to be present after failover",
        );
        const blockedAfterFailover = scanState().blockedTasks.map((id) => id.toString());
        assert.sameMembers(
            blockedAfterClassify,
            blockedAfterFailover,
            "blocked set must survive failover unchanged",
        );
        assert(
            blockedAfterFailover.includes(taskId.toString()),
            "the classified task must still be blocked after failover",
        );

        // Still enforced after failover: the task is completed without deleting the orphan.
        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "blocked task must be completed without deleting after failover",
        );
        assert.eq(
            1,
            primary().getDB(dbName).getCollection("failover").find({a: MaxKey}).itcount(),
            "orphan must remain after failover",
        );
        assert.gt(
            guardedTaskCount(),
            guardedBefore,
            "counter must advance for the guarded task (possibly more than once across the failover)",
        );
    });

    it("distinguishes a classified task from a concurrent post-classification task", function () {
        setGuardEnabled(true);
        const guardedBefore = guardedTaskCount();
        suspendRangeDeletionEverywhere();
        // Classify task A (pre-upgrade): blockedTasks = [A].
        const uuidA = classifyAtStepUp("concurrentA");

        // Create task B after classification while deletion is still suspended, so both tasks are
        // pending at once. B is not reclassified (no step-up), so it is not in the blocked set.
        const uuidB = createGlobalMaxOrphanTask("concurrentB");

        resumeRangeDeletionEverywhere();

        // Both drain in the same processing pass: A completed-without-delete (orphan remains), B
        // deleted normally (orphan gone).
        assert.soon(
            () =>
                rangeDeletions().countDocuments({collectionUuid: uuidA}) === 0 &&
                rangeDeletions().countDocuments({collectionUuid: uuidB}) === 0,
            "both tasks must drain from config.rangeDeletions",
        );
        assert.eq(
            1,
            primary().getDB(dbName).getCollection("concurrentA").find({a: MaxKey}).itcount(),
            "classified task A's orphan must remain",
        );
        assert.eq(
            0,
            primary().getDB(dbName).getCollection("concurrentB").find({a: MaxKey}).itcount(),
            "post-classification task B's orphan must be deleted",
        );
        assert.gt(
            guardedTaskCount(),
            guardedBefore,
            "the classified task must advance the guarded-task counter",
        );
    });

    it("preserves only the global-max doc for a compound shard key", function () {
        setGuardEnabled(true);
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("compound", createCompoundGlobalMaxOrphanTask);

        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "guarded compound task must complete",
        );
        const coll = primary().getDB(dbName).getCollection("compound");
        // The deletion stops exclusively at the global max {a: MaxKey, b: MaxKey}, so only the doc
        // whose shard key IS the global max is preserved; leading-MaxKey docs below it are deleted.
        assert.eq(
            1,
            coll.find({a: MaxKey}).itcount(),
            "only the global-max doc {a: MaxKey, b: MaxKey} must be preserved",
        );
        assert.eq(
            1,
            coll.find({a: MaxKey, b: MaxKey}).itcount(),
            "the global-max doc must be the one preserved",
        );
        assert.eq(
            0,
            coll.find({a: MaxKey, b: 5}).itcount(),
            "a leading-MaxKey doc below the global max must be deleted",
        );
        assert.eq(
            0,
            coll.find({a: 5, b: MaxKey}).itcount(),
            "a trailing-only MaxKey doc must be deleted",
        );
        assert.eq(0, coll.find({a: 50}).itcount(), "ordinary orphan must be deleted");
    });

    it("detects and preserves a leading-MaxKey doc through a wider shard-key index", function () {
        setGuardEnabled(true);
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("wideridx", createWiderIndexGlobalMaxOrphanTask);

        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "guarded wider-index task must complete",
        );
        const coll = primary().getDB(dbName).getCollection("wideridx");
        // Classification (inclusive bound) must have found the doc through the wider index for it to
        // be blocked at all; the preserve path (exclusive bound) then keeps it.
        assert.eq(
            1,
            coll.find({a: MaxKey}).itcount(),
            "leading-MaxKey doc read through the wider index must be preserved",
        );
        assert.eq(0, coll.find({a: 50}).itcount(), "ordinary orphan must be deleted");
    });

    it("preserves only the global-max doc for a compound shard key through a wider index", function () {
        setGuardEnabled(true);
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("compoundwider", createCompoundWiderIndexGlobalMaxOrphanTask);

        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "guarded compound wider-index task must complete",
        );
        const coll = primary().getDB(dbName).getCollection("compoundwider");
        // Only the doc whose shard key is the global max {a: MaxKey, b: MaxKey} is preserved; the
        // leading-but-below-max doc {a: MaxKey, b: 1} is deleted despite the wider index.
        assert.eq(
            1,
            coll.find({a: MaxKey, b: MaxKey}).itcount(),
            "the compound global-max doc must be preserved through the wider index",
        );
        assert.eq(
            0,
            coll.find({a: MaxKey, b: 1}).itcount(),
            "a compound leading-MaxKey doc below the global max must be deleted",
        );
        assert.eq(0, coll.find({a: 50}).itcount(), "ordinary orphan must be deleted");
    });

    it("aborts a migration back onto a shard that still holds a preserved MaxKey orphan", function () {
        setGuardEnabled(true);
        suspendRangeDeletionEverywhere();
        const uuid = classifyAtStepUp("remigrate");

        // Let the guard run: the ordinary doc is deleted, the MaxKey doc is preserved on shard0.
        resumeRangeDeletionEverywhere();
        assert.soon(
            () => rangeDeletions().countDocuments({collectionUuid: uuid}) === 0,
            "guarded task must complete",
        );
        assert.eq(
            1,
            primary().getDB(dbName).getCollection("remigrate").find({a: MaxKey}).itcount(),
            "MaxKey orphan must be preserved on shard0",
        );

        // Migrating the chunk back onto shard0 must ABORT rather than corrupt: the recipient's
        // pre-clone check finds the preserved MaxKey doc already present in the incoming range
        // (checkForExistingDocumentsInRange, log 11365900). This is the invariant that makes
        // preserving-in-place safe.
        // The recipient reports a "fail" migrate state (its pre-clone check found the preserved
        // MaxKey doc), which the donor surfaces as OperationFailed ("Data transfer error: ...").
        assert.commandFailedWithCode(
            adminDB.runCommand({
                moveChunk: `${dbName}.remigrate`,
                find: {a: 50},
                to: st.shard0.shardName,
                _waitForDelete: false,
            }),
            ErrorCodes.OperationFailed,
        );
        assert.soon(
            () => checkLog.checkContainsOnceJson(primary(), 11365900),
            "expected the recipient to abort on the existing MaxKey document",
        );
    });
});
