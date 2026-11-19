/**
 * Tests how a secondary applies an update by recordId when it does not have the document, for plain
 * updates and retryable findAndModify, with and without change stream pre-images. With steady state
 * constraints not enforced, the update is applied as an upsert that inserts the document at the
 * oplog entry's recordId, unless a change stream pre-image must be recorded. With them enforced, or
 * when a pre-image must be recorded, the secondary crashes.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// The secondary is expected to abort on an invariant in some cases, which leaves a core dump.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

const dbName = jsTestName();
const collName = "coll";
const preImagesOptions = {changeStreamPreAndPostImages: {enabled: true}};

// Starts a two-node replica set whose secondary is never elected, so that it keeps applying the
// primary's oplog, and creates the test collection with 'collOptions'. An update applied as an
// upsert cannot verify its size delta, so per-collection validation failures are made non-fatal.
function startReplSet(collOptions = {}) {
    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {
            setParameter: {fassertOnPerCollectionContinuousInternodeValidationFailure: false},
        },
    });
    rst.startSet();
    rst.initiate();

    const primaryDB = rst.getPrimary().getDB(dbName);
    assert.commandWorked(primaryDB.createCollection(collName, collOptions));
    const collInfo = primaryDB.getCollectionInfos({name: collName})[0];
    assert(collInfo.info.recordIdsReplicated, "Expected replicated recordIds", {collInfo});
    return rst;
}

function primaryColl(rst) {
    return rst.getPrimary().getDB(dbName)[collName];
}

function secondaryDB(rst, name) {
    const secondary = rst.getSecondary();
    secondary.setSecondaryOk();
    return secondary.getDB(name);
}

function findWithRecordId(coll, id) {
    const docs = coll.find({_id: id}).showRecordId().toArray();
    assert.eq(1, docs.length, "Expected exactly one document", {id, docs});
    return docs[0];
}

function updatesOnMissingDoc(node) {
    const status = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    return Number(status.opcountersRepl.constraintsRelaxed?.updateOnMissingDoc ?? 0);
}

function setSteadyStateConstraints(rst, enforce) {
    assert.commandWorked(
        rst.getSecondary().adminCommand({
            setParameter: 1,
            oplogApplicationEnforcesSteadyStateConstraints: enforce,
        }),
    );
}

// Inserts {_id: id, x: 1} on both nodes, then removes it from the secondary only, by deleting it
// while the node runs as a standalone.
function insertOnBothDeleteOnSecondary(rst, id) {
    assert.commandWorked(primaryColl(rst).insert({_id: id, x: 1}, {writeConcern: {w: "majority"}}));

    // A standalone starts from the last checkpoint, which must include the document.
    rst.awaitReplication();
    rst.awaitLastStableRecoveryTimestamp();
    assert.commandWorked(rst.getSecondary().adminCommand({fsync: 1}));

    const secondaryId = rst.getNodeId(rst.getSecondary());
    const standalone = rst.restart(secondaryId, {noReplSet: true});
    const res = assert.commandWorked(
        standalone
            .getDB(dbName)
            .runCommand({delete: collName, deletes: [{q: {_id: id}, limit: 1}]}),
    );
    assert.eq(1, res.n, "Expected to delete the document on the standalone", {res});

    rst.restart(secondaryId, {noReplSet: false});
    rst.awaitSecondaryNodes();
    assert.eq(0, secondaryDB(rst, dbName)[collName].find({_id: id}).itcount());
}

// Runs a retryable findAndModify on the primary that sets x to 2, returning the new document when
// 'returnNew' is true and the old one otherwise. Returns the session id it ran under.
function retryableFindAndModify(rst, id, returnNew, writeConcern) {
    const lsid = {id: UUID()};
    assert.commandWorked(
        rst
            .getPrimary()
            .getDB(dbName)
            .runCommand({
                findAndModify: collName,
                query: {_id: id},
                update: {$set: {x: 2}},
                new: returnNew,
                lsid,
                txnNumber: NumberLong(1),
                writeConcern,
            }),
    );
    return lsid;
}

// Returns the retry image a node stored for the session 'lsid' in config.image_collection.
function retryImage(configDB, lsid) {
    const entries = configDB.image_collection.find({"_id.id": lsid.id}).toArray();
    assert.eq(1, entries.length, "Expected one retry image for the session", {entries});
    return entries[0];
}

// Applies 'write' on the primary to a document the secondary is missing, then waits for the
// secondary to crash and checks how it exited.
function assertSecondaryCrashes(rst, id, {enforceConstraints, write, exitCode, logPattern}) {
    insertOnBothDeleteOnSecondary(rst, id);
    setSteadyStateConstraints(rst, enforceConstraints);
    const secondary = rst.getSecondary();
    clearRawMongoProgramOutput();

    // The secondary is expected to crash rather than acknowledge the write.
    write(rst, {w: 1});

    // Wait for the process to exit rather than stopping it, which would shut it down cleanly if it
    // had not applied the write yet.
    let res;
    assert.soon(() => {
        res = checkProgram(secondary.pid);
        return !res.alive;
    });
    assert.eq(exitCode, res.exitCode, "Unexpected secondary exit code", {res});
    assert(rawMongoProgramOutput(logPattern), "Expected the secondary to log the failure", {
        logPattern,
    });
}

describe("Updates by recordId on a secondary that is missing the document", function () {
    describe("with steady state constraints not enforced", function () {
        let rst;

        before(function () {
            rst = startReplSet();
        });

        after(function () {
            // The retry image of a findAndModify that returned the old document is invalidated on
            // the secondary but not on the primary, so config is left out of the comparison.
            rst.checkReplicatedDataHashes(jsTestName(), ["config"]);
            rst.stopSet(undefined, undefined, {skipCheckDBHashes: true});
        });

        it("applies an update to an existing document at the same recordId", function () {
            setSteadyStateConstraints(rst, false);
            assert.commandWorked(primaryColl(rst).insert({_id: 1, x: 1}));
            assert.commandWorked(primaryColl(rst).update({_id: 1}, {$set: {x: 2}}));
            rst.awaitReplication();

            assert.docEq(
                findWithRecordId(primaryColl(rst), 1),
                findWithRecordId(secondaryDB(rst, dbName)[collName], 1),
            );
        });

        it("upserts an update at the primary's recordId", function () {
            insertOnBothDeleteOnSecondary(rst, 2);
            setSteadyStateConstraints(rst, false);
            const missingDocsBefore = updatesOnMissingDoc(rst.getSecondary());
            assert.commandWorked(primaryColl(rst).update({_id: 2}, {$set: {x: 2}}));
            rst.awaitReplication();

            assert.docEq(
                findWithRecordId(primaryColl(rst), 2),
                findWithRecordId(secondaryDB(rst, dbName)[collName], 2),
            );
            assert.eq(missingDocsBefore + 1, updatesOnMissingDoc(rst.getSecondary()));
        });

        it("upserts a findAndModify returning the old document and invalidates its retry image", function () {
            insertOnBothDeleteOnSecondary(rst, 3);
            setSteadyStateConstraints(rst, false);
            const missingDocsBefore = updatesOnMissingDoc(rst.getSecondary());
            const lsid = retryableFindAndModify(rst, 3, /*returnNew=*/ false, {w: "majority"});
            rst.awaitReplication();

            assert.docEq(
                findWithRecordId(primaryColl(rst), 3),
                findWithRecordId(secondaryDB(rst, dbName)[collName], 3),
            );
            assert.eq(missingDocsBefore + 1, updatesOnMissingDoc(rst.getSecondary()));

            // The secondary had no old document, so it has no pre-image to store for a retry.
            const primaryImage = retryImage(rst.getPrimary().getDB("config"), lsid);
            assert.eq("preImage", primaryImage.imageKind, {primaryImage});
            assert(!primaryImage.invalidated, "Expected a valid image on the primary", {
                primaryImage,
            });
            const secondaryImage = retryImage(secondaryDB(rst, "config"), lsid);
            assert.eq("preImage", secondaryImage.imageKind, {secondaryImage});
            assert(secondaryImage.invalidated, "Expected an invalidated image on the secondary", {
                secondaryImage,
            });
        });

        it("upserts a findAndModify returning the new document and stores it as the retry image", function () {
            insertOnBothDeleteOnSecondary(rst, 4);
            setSteadyStateConstraints(rst, false);
            const missingDocsBefore = updatesOnMissingDoc(rst.getSecondary());
            const lsid = retryableFindAndModify(rst, 4, /*returnNew=*/ true, {w: "majority"});
            rst.awaitReplication();

            assert.docEq(
                findWithRecordId(primaryColl(rst), 4),
                findWithRecordId(secondaryDB(rst, dbName)[collName], 4),
            );
            assert.eq(missingDocsBefore + 1, updatesOnMissingDoc(rst.getSecondary()));

            // The upserted document is stored as the post-image, the same one the primary stored.
            const primaryImage = retryImage(rst.getPrimary().getDB("config"), lsid);
            const secondaryImage = retryImage(secondaryDB(rst, "config"), lsid);
            assert.eq("postImage", secondaryImage.imageKind, {secondaryImage});
            assert(!secondaryImage.invalidated, "Expected a valid image on the secondary", {
                secondaryImage,
            });
            assert.docEq(primaryImage.image, secondaryImage.image);
        });
    });

    describe("crashes the secondary", function () {
        const id = 10;
        const update = (rst, writeConcern) =>
            assert.commandWorked(
                primaryColl(rst).update({_id: id}, {$set: {x: 2}}, {writeConcern}),
            );
        const findAndModify = (returnNew) => (rst, writeConcern) =>
            retryableFindAndModify(rst, id, returnNew, writeConcern);
        // The record is missing and the update cannot be applied as an upsert.
        const recordNotFound = "11902401.*the record was not found";
        // A change stream pre-image cannot be taken as RETURN_OLD from a missing record.
        const preImageInvariant = "Invariant failure.*recordChangeStreamPreImage";

        // Each case leaves the secondary crashed and out of sync with the primary, so each one gets
        // its own replica set.
        function runCrashCase(collOptions, crashSpec) {
            const rst = startReplSet(collOptions);
            try {
                assertSecondaryCrashes(rst, id, crashSpec);
            } finally {
                rst.stopSet(undefined, undefined, {skipCheckDBHashes: true});
            }
        }

        it("on an update when steady state constraints are enforced", function () {
            runCrashCase(
                {},
                {
                    enforceConstraints: true,
                    write: update,
                    exitCode: MongoRunner.EXIT_ABRUPT,
                    logPattern: recordNotFound,
                },
            );
        });

        it("on an update that records a change stream pre-image", function () {
            runCrashCase(preImagesOptions, {
                enforceConstraints: false,
                write: update,
                exitCode: MongoRunner.EXIT_ABORT,
                logPattern: preImageInvariant,
            });
        });

        it("on a findAndModify returning the old document that records a change stream pre-image", function () {
            runCrashCase(preImagesOptions, {
                enforceConstraints: false,
                write: findAndModify(/*returnNew=*/ false),
                exitCode: MongoRunner.EXIT_ABORT,
                logPattern: preImageInvariant,
            });
        });

        it("on a findAndModify returning the new document that records a change stream pre-image", function () {
            // The pre-image would be copied from the record before the update, but there is no
            // record, so this fails as an inconsistency instead of being applied as an upsert.
            runCrashCase(preImagesOptions, {
                enforceConstraints: false,
                write: findAndModify(/*returnNew=*/ true),
                exitCode: MongoRunner.EXIT_ABRUPT,
                logPattern: recordNotFound,
            });
        });
    });
});
