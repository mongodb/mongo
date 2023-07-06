/**
 * Test fixture which exposes several methods to test different pre-image truncate behaviors after
 * shutdown.
 *
 * After an unclean shutdown, all expired pre-images must be truncated on startup. This is because
 * WiredTiger truncate cannot guarantee a consistent view of previously truncated data on
 * unreplicated, untimestamped ranges after a crash. Unlike the oplog, which also does
 * untimestamped, unreplicated truncates, the pre-images collection is not logged and it's possible
 * that previously truncated documents can resurface after unclean shutdown.
 *
 * On an unclean shutdown, no preemptive truncation is necessary.
 *
 * Example Usage:
 *      const truncateAfterShutdownTest = new PreImageTruncateAfterShutdownTest(jsTestName());
 *      truncateAfterShutdownTest.setup();
 *      truncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
 *          runAsPrimary: <>,
 *          numExpiredPreImages: <>,
 *          numUnexpiredPreImages: <>,
 *          cleanShutdown: <>,
 *      });
 *      ...
 *      truncateAfterShutdownTest.teardown();
 */

import {
    getPreImages,
    getPreImagesCollection,
    kPreImagesCollectionDatabase,
    kPreImagesCollectionName
} from "jstests/libs/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getFirstOplogEntry, getLatestOp} from "jstests/replsets/rslib.js";

export class PreImageTruncateAfterShutdownTest {
    constructor(testName) {
        this.testName = testName;

        // The @private JSDoc comments cause VS Code to not display the corresponding properties and
        // methods in its autocomplete list. This makes it simpler for test authors to know what the
        // public interface of the ReshardingTest class is.

        /** @private */
        this._oplogSize = 1;
        /** @private */
        this._getDefaultNodeOptions = () => {
            return {
                setParameter: {
                    // Disable the 'ChangeStreamExpiredPreImagesRemover' to isolate the truncate
                    // after
                    // startup behavior from the periodic remover behavior.
                    disableExpiredPreImagesRemover: true,
                },
                // Oplog can be truncated each "sync" cycle. Increase its frequency to once per
                // every 5 seconds to speed up tests which rely on oldest oplog timestamp expiry.
                syncdelay: 5,
            };
        };

        // Property set by setup().
        /** @private */
        this._rst = undefined;
    }

    setup() {
        this._rst = new ReplSetTest(
            {nodes: 2, nodeOptions: this._getDefaultNodeOptions(), oplogSize: 1 /** 1MB */});
        this._rst.startSet();

        // Allow test cases to have complete control over which node is primary.
        this._rst.initiateWithHighElectionTimeout();
    }

    teardown() {
        this._rst.stopSet();
    }

    /** @private */
    _assertNumPreImages(nPreImages, conn, isStandalone = false) {
        // Inserts are replicated while truncates are not. If 'conn' is a secondary, await
        // replication in case there were inserts.
        const awaitReplication = (!isStandalone && conn !== this._rst.getPrimary());
        if (awaitReplication) {
            this._rst.awaitReplication();
        }

        const preImages = getPreImages(conn);
        assert.eq(preImages.length, nPreImages, preImages);
    }

    // Checks if the oplog has been rolled over from the timestamp of 'lastOplogEntryTsToBeRemoved',
    // ie. the timestamp of the first entry in the oplog is greater than the
    // 'lastOplogEntryTsToBeRemoved' on each node of the replica set.
    /** @private */
    _oplogIsRolledOver(lastOplogEntryTsToBeRemoved) {
        return this._rst.nodes.every(
            (node) => timestampCmp(lastOplogEntryTsToBeRemoved,
                                   getFirstOplogEntry(node, {readConcern: "majority"}).ts) <= 0);
    }

    /** @private */
    _rollOverOplog(nodeToRollover) {
        // Pre-images expire independently on each node. Expiration by oldest oplog timestamp
        // depends on the oldest oplog timestamp of the 'nodeToRollover'.
        const lastOplogEntryToBeRemoved = getLatestOp(nodeToRollover);

        // Oplog is rolled over by inserting many writes. Writes still must be done on the primary.
        const primary = this._rst.getPrimary();
        const testDB = primary.getDB(this.testName);
        const largeStr = new Array(1024 * 10).toString();

        // Insert a base amount of documents to speed things up.
        for (let i = 0; i < 200; i++) {
            assert.commandWorked(
                testDB.tmp.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
        }

        while (!this._oplogIsRolledOver(lastOplogEntryToBeRemoved.ts)) {
            assert.commandWorked(
                testDB.tmp.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
        }
    }

    /** @private */
    _assertNumPreImagesAfterShutdown({
        conn,
        numExpiredPreImages,
        numUnexpiredPreImages,
        cleanShutdown,
        standalone = false,
    }) {
        // Only after an unclean shutdown should preemptive truncation of pre-images happen.
        const expectedPreImagesAfterShutdown =
            cleanShutdown ? numUnexpiredPreImages + numExpiredPreImages : numUnexpiredPreImages;
        this._assertNumPreImages(expectedPreImagesAfterShutdown, conn, standalone);
    }

    // Pre-populates 2 pre-image enabled collections. The total number of pre-images adds up to
    // 'numUnexpiredPreImages' unexpired pre-images and 'numExpiredPreImages' expired pre-images.
    // 'expiryFn' is responsible simulating the expiration of all pre-images inserted at the time of
    // it's execution.
    /** @private */
    _prePopulatePreImageCollections({
        conn,
        numExpiredPreImages,
        numUnexpiredPreImages,
        expiryFn = () => {},
    }) {
        const collInfos = [
            {
                name: "collA",
                nUnexpired: parseInt(numUnexpiredPreImages / 2),
                nExpired: parseInt(numExpiredPreImages / 2),
            },
            {
                name: "collB",
                nUnexpired: parseInt(numUnexpiredPreImages / 2) + numUnexpiredPreImages % 2,
                nExpired: parseInt(numExpiredPreImages / 2) + numExpiredPreImages % 2,
            },
        ];

        const testDB = this._rst.getPrimary().getDB(this.testName);
        for (const collInfo of collInfos) {
            const collName = collInfo.name;
            assertDropAndRecreateCollection(
                testDB, collName, {changeStreamPreAndPostImages: {enabled: true}});

            const coll = testDB[collName];
            assert.commandWorked(
                coll.insert({_id: 0, version: 1}, {writeConcern: {w: "majority"}}));
            for (let i = 0; i < collInfo.nExpired; i++) {
                // Do initial insert, then update to create a pre-image.
                assert.commandWorked(coll.update({_id: 0}, {$inc: {version: 1}}));
            }
        }

        this._assertNumPreImages(numExpiredPreImages, conn);

        expiryFn();

        for (const collInfo of collInfos) {
            const coll = testDB[collInfo.name];

            for (let i = 0; i < collInfo.nUnexpired; i++) {
                // Do initial insert, then update to create a pre-image.
                assert.commandWorked(coll.update({_id: 0}, {$inc: {version: 1}}));
            }
        }
        this._assertNumPreImages(numUnexpiredPreImages + numExpiredPreImages, conn);
    }

    // After shutdown, gets 'conn' to it's original state pre-shutdown and verifies the correct
    // pre-images exist after shutdown.
    /** @private */
    _reestablishOriginalTopologyAndValidatePreImagesPostShutdown({
        conn,
        numExpiredPreImages,
        numUnexpiredPreImages,
        runAsPrimary,
        cleanShutdown,
    }) {
        // The pre-images collection cannot be checked until the restarted node starts accepting
        // reads.
        assert.soonNoExcept(function() {
            const nodeState = assert.commandWorked(conn.adminCommand("replSetGetStatus")).myState;
            return nodeState == ReplSetTest.State.SECONDARY;
        });
        conn.setSecondaryOk();

        if (runAsPrimary) {
            // The 'checkPreImageCollection' check relies on there being a writable primary.
            this._stepUp(conn);
            assert.soon(() => conn.adminCommand('hello').isWritablePrimary);
        }

        this._assertNumPreImagesAfterShutdown({
            conn,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
        });

        this._rst.checkPreImageCollection(this.testName);
    }

    /** @private */
    _stepUp(connection) {
        assert.soonNoExcept(() => {
            const res = connection.adminCommand({replSetStepUp: 1});
            if (!res.ok) {
                jsTestLog(`Failed to step up with ${res}`);
            }
            return res.ok;
        }, "Failed to step up");
        jsTestLog(`Forced step up to ${connection}`);
    }

    /** @private */
    _shutdownNode(conn, cleanShutdown) {
        jsTest.log(
            "Force a checkpoint so the connection has data on startup recovery in the case of a crash");
        assert.commandWorked(conn.adminCommand({fsync: 1}));
        if (cleanShutdown) {
            jsTest.log("Forcing a clean shutdown of the node");
            this._rst.stop(conn);
        } else {
            jsTest.log("Forcing an unclean shutdown of the node");
            this._rst.stop(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
        }
    }

    // Common logic for testing truncate behavior of pre-images expired by the oldest oplog entry.
    //
    // 'restartFn': Shuts down the node tied to the original 'conn' and returns the new 'conn' of
    // the node restarted in the replica set.
    /** @private */
    _testExpireByOplogCommon({
        runAsPrimary,
        numExpiredPreImages,
        numUnexpiredPreImages,
        cleanShutdown,
        restartFn = (conn) => {},
    }) {
        let conn = runAsPrimary ? this._rst.getPrimary() : this._rst.getSecondary();
        assertDropAndRecreateCollection(this._rst.getPrimary().getDB(kPreImagesCollectionDatabase),
                                        kPreImagesCollectionName,
                                        {clusteredIndex: true});

        this._prePopulatePreImageCollections({
            conn,
            numExpiredPreImages,
            numUnexpiredPreImages,
            expiryFn: () => {
                this._rollOverOplog(conn);
            },
        });

        conn = restartFn(conn);

        this._reestablishOriginalTopologyAndValidatePreImagesPostShutdown({
            conn,
            numExpiredPreImages,
            numUnexpiredPreImages,
            runAsPrimary,
            cleanShutdown,
        });
    }

    /** @private */
    _setupExpireAfterSecondsTest(expireAfterSeconds) {
        assert.commandWorked(this._rst.getPrimary().getDB("admin").runCommand({
            setClusterParameter: {changeStreamOptions: {preAndPostImages: {expireAfterSeconds}}}
        }));
    }

    /** @private */
    _teardownExpireAfterSecondsTest(conn) {
        assert.commandWorked(this._rst.getPrimary().getDB("admin").runCommand({
            setClusterParameter:
                {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: "off"}}}
        }));

        assert.commandWorked(conn.adminCommand(
            {'configureFailPoint': 'changeStreamPreImageRemoverCurrentTime', 'mode': 'off'}));
    }

    // Tests pre-image truncate behavior on 'conn' after shutdown when pre-image expiration is based
    // on 'expireAfterSeconds'.
    //
    // 'runAsPrimary': Whether to test truncate behavior on the primary or the secondary.
    // 'cleanShutdown': Whether the truncate behavior is tested after a clean or unclean shutdown.
    testTruncateByExpireAfterSeconds({
        runAsPrimary,
        numExpiredPreImages,
        numUnexpiredPreImages,
        cleanShutdown,
    }) {
        jsTest.log(`Running testTruncateByExpireAfterSeconds with ${tojson({
            runAsPrimary,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
        })}`);

        assertDropAndRecreateCollection(this._rst.getPrimary().getDB(kPreImagesCollectionDatabase),
                                        kPreImagesCollectionName,
                                        {clusteredIndex: true});

        let conn = runAsPrimary ? this._rst.getPrimary() : this._rst.getSecondary();
        const expireAfterSeconds = 1;
        this._setupExpireAfterSecondsTest(expireAfterSeconds);

        // To ensure there are no inconsistent ranges after unclean shutdown, all pre-images within
        // 10 seconds of expiry according to 'expireAfterSeconds' are also truncated after unlcean
        // shutdown.
        const truncateAfterUncleanShutdownBufferSecs = 10;

        // Upon startup, the current time 'conn' uses to determine which pre-images to truncate, if
        // any.
        let currentTimeToUseAfterRestart = undefined;
        const expiryFn = () => {
            // Returns the 'operationTime' of the most recent pre-image, if there are any inserted.
            // Otherwise, returns the 'operationTime' of a no-op ping.
            const getMostRecentOperationTime = () => {
                if (numExpiredPreImages == 0) {
                    return assert.commandWorked(conn.getDB(this.testName).runCommand({ping: 1}))
                        .operationTime.getTime();
                }
                const lastPreImageToExpire =
                    getPreImagesCollection(conn).find().sort({"_id.ts": -1}).limit(1).toArray();
                assert.eq(lastPreImageToExpire.length, 1, lastPreImageToExpire);
                return lastPreImageToExpire[0].operationTime.getTime();
            };
            const mostRecentOperationTime = getMostRecentOperationTime();

            // In the case of unclean shutdown, startup expands the truncate range to include
            // all pre-images expired by 'expireAfterSeconds' along with more recent pre-images
            // which were created within 'truncateAfterUncleanShutdownBufferSecs' of pre-images
            // expired by 'expireAfterSeconds'.
            //
            // Fix the 'currentTimeToUseAfterStartup' so the current pre-images fall into the
            // truncate range of startup after an unclean shutdown.
            currentTimeToUseAfterRestart =
                new Date(mostRecentOperationTime -
                         (truncateAfterUncleanShutdownBufferSecs - expireAfterSeconds) * 1000);

            // Sleep 2 seconds to ensure any rounding done to convert the expiration date to a
            // 'Timestamp' for range truncation doesn't include upcoming pre-image inserts.
            sleep(2 * 1000);
        };

        this._prePopulatePreImageCollections({
            conn,
            numExpiredPreImages,
            numUnexpiredPreImages,
            expiryFn,
        });

        this._shutdownNode(conn, cleanShutdown);

        const nodeOptions = this._getDefaultNodeOptions();
        nodeOptions.setParameter["failpoint.changeStreamPreImageRemoverCurrentTime"] = tojson({
            'data':
                {'currentTimeForTimeBasedExpiration': currentTimeToUseAfterRestart.toISOString()},
            'mode': 'alwaysOn'
        });
        conn = this._rst.start(conn, nodeOptions, true /* restart */);

        this._reestablishOriginalTopologyAndValidatePreImagesPostShutdown({
            conn,
            numExpiredPreImages,
            numUnexpiredPreImages,
            runAsPrimary,
            cleanShutdown,
        });

        this._teardownExpireAfterSecondsTest(conn);
    }

    // Tests pre-image truncate behavior when pre-images expire according to the oldest oplog entry
    // timestamp and a node is both shutdown and brought back up as a member of the replica set.
    testTruncateByOldestOplogTS({
        runAsPrimary,
        numExpiredPreImages,
        numUnexpiredPreImages,
        cleanShutdown,
    }) {
        jsTest.log(`Running testTruncateByOldestOplogTS with ${tojson({
            runAsPrimary,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
        })}`);

        const restartFn = (conn) => {
            this._shutdownNode(conn, cleanShutdown);
            return this._rst.start(conn, this._getDefaultNodeOptions(), true);
        };

        this._testExpireByOplogCommon({
            runAsPrimary,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
            restartFn,
        });
    }

    // Tests pre-image truncate behavior when a node in a replica set is shutdown, brought back up
    // as a standalone, then rejoined as a replica set member.
    //
    // 'runAsPrimary': Whether to test truncate behavior on the primary or the secondary.
    // 'cleanShutdown': Whether the truncate behavior is tested after a clean or unclean shutdown.
    // 'restartWithQueryableBackup': Whether to restart node in standalone with
    ///                             'queryableBackupMode'.
    // 'restartWithRecoverToOplogTimestamp': Only applicable when 'restartWithQueryableBackup' is
    //                              true. Indicates 'queryableBackupMode' should startup with a set
    //                              'recoverToOplogTimestamp'.
    // 'restartWithRecoverFromOplogAsStandalone': Whether to restart 'conn' in standalone with
    //                              'recoverFromOplogAsStandalone' set to true.
    testTruncateByOldestOplogTSStandalone({
        runAsPrimary,
        numExpiredPreImages,
        numUnexpiredPreImages,
        cleanShutdown,
        restartWithQueryableBackup,
        restartWithRecoverToOplogTimestamp,
        restartWithRecoverFromOplogAsStandalone,
    }) {
        jsTest.log(`Running testTruncateByOldestOplogTS with ${tojson({
            runAsPrimary,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
            restartWithQueryableBackup,
            restartWithRecoverToOplogTimestamp,
            restartWithRecoverFromOplogAsStandalone,
        })}`);

        // 'recoverToOplogTimestamp' is only compatible when 'queryableBackupMode' is set.
        assert(!restartWithRecoverToOplogTimestamp ||
               (restartWithRecoverToOplogTimestamp && restartWithQueryableBackup));
        // 'queryableBackupMode' isn't compatible with 'recoverFromOplogAsStandalone'.
        assert(!restartWithRecoverFromOplogAsStandalone || !restartWithQueryableBackup);

        // Restarts the node in standalone according to the specified restart parameters.
        // Re-connects the node to the replica set before returning a new "conn".
        const restartFn = (conn) => {
            const connDBPath = conn.dbpath;
            const standaloneStartupOpts = {
                dbpath: connDBPath,
                noReplSet: true,
                noCleanData: true,
            };

            // Finalize the restart options for standalone before shutting down in case
            // a timestamp is needed for 'recoverFromOplogAsStandalone'.
            if (restartWithQueryableBackup) {
                standaloneStartupOpts.queryableBackupMode = "";
            }
            if (restartWithRecoverToOplogTimestamp) {
                const recoveryTimestamp =
                    assert.commandWorked(conn.getDB(this.testName).runCommand({ping: 1}))
                        .operationTime;
                standaloneStartupOpts.setParameter = {
                    recoverToOplogTimestamp: tojson({timestamp: recoveryTimestamp})
                };
            }
            if (restartWithRecoverFromOplogAsStandalone) {
                standaloneStartupOpts.setParameter = 'recoverFromOplogAsStandalone=true';
            }

            this._shutdownNode(conn, cleanShutdown);

            // Start node up in standalone.
            jsTest.log(`Starting node as standalone with startup options ${
                tojson(standaloneStartupOpts)}`);
            const standaloneConn = MongoRunner.runMongod(standaloneStartupOpts);
            this._assertNumPreImagesAfterShutdown({
                conn: standaloneConn,
                numExpiredPreImages,
                numUnexpiredPreImages,
                cleanShutdown,
                standalone: true,
            });
            MongoRunner.stopMongod(standaloneConn);

            const restartNodeOptions = this._getDefaultNodeOptions();
            restartNodeOptions.noReplSet = false;
            return this._rst.start(conn, restartNodeOptions, true);
        };

        // 'expireAfterSeconds' is incompatible with standalone mode because it requires using
        // 'setClusterParameter'. Thus, all tests over restarting in standalone must rely on
        // pre-images expiring by the oldest oplog timestamp.
        this._testExpireByOplogCommon({
            runAsPrimary,
            numExpiredPreImages,
            numUnexpiredPreImages,
            cleanShutdown,
            restartFn,
        });
    }
}
