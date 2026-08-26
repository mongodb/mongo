/**
 * Tests that createIndexes does not report success without creating the index when a concurrent
 * moveCollection commit renames a new collection over the namespace mid-build. The build then
 * fails with NamespaceNotFound, which createIndexes used to treat as "collection dropped" and
 * report success for, even though the namespace holds a different collection without the index.
 *
 * A pending createIndexes resolves the collection's UUID to a namespace at two different points,
 * and moveCollection can win the race against either one, so this file covers both.
 *
 * For a sharded request, createIndexes retries its initial checks after NamespaceNotFound. A
 * moveCollection makes those checks report stale routing metadata, allowing mongos to retry the
 * command on the new owner; a concurrent drop lets the normal implicit collection-creation path
 * create the collection and index.
 *
 * moveCollection leaves different local state behind on the donor depending on whether it is the
 * database's primary shard, so every move scenario runs once for each possible primary-shard
 * position relative to the donor and recipient.
 *
 * @tags: [
 *   # This test coordinates parallel shells and an index-build throttle slot; execution-control
 *   # prioritization can delay a child shell past the synchronization timeout.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

// TODO SERVER-133024: remove this once the injected write conflicts are fixed.
// Suites that randomly inject write conflicts on every write can hit cluster setup and
// moveCollection at points unrelated to the createIndexes race this test covers, failing the
// command or crashing a shard. Drop the injection before any node starts for a deterministic run.
delete TestData.setParameters?.["failpoint.WTWriteConflictException"];

describe("createIndexes racing a moveCollection commit or concurrent drop", function () {
    let st;
    let donor;
    let recipient;
    let thirdShard;

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 3,
            configOptions: {
                setParameter: {reshardingMinimumOperationDurationMillis: 0},
            },
            rsOptions: {
                setParameter: {reshardingMinimumOperationDurationMillis: 0},
            },
        });

        donor = st.shard1;
        recipient = st.shard2;
        thirdShard = st.shard0;
    });

    after(function () {
        if (st) {
            st.stop();
        }
    });

    // Every position the database's primary shard can take relative to the fixed donor/recipient
    // pair.
    const primaryShardCases = [
        {label: "donor", title: "primary shard is the donor", getPrimaryShard: () => donor},
        {
            label: "recipient",
            title: "primary shard is the recipient",
            getPrimaryShard: () => recipient,
        },
        {
            label: "thirdShard",
            title: "primary shard is neither the donor nor the recipient",
            getPrimaryShard: () => thirdShard,
        },
    ];

    for (const {label, title, getPrimaryShard} of primaryShardCases) {
        describe(title, function () {
            let dbName;
            let testDB;

            before(function () {
                dbName = `${jsTestName()}_${label}`;
                testDB = st.s.getDB(dbName);
                assert.commandWorked(
                    st.s.adminCommand({
                        enableSharding: dbName,
                        primaryShard: getPrimaryShard().shardName,
                    }),
                );
            });

            function createUnsplittableCollectionWithDocs(collName) {
                assert.commandWorked(
                    testDB.runCommand({
                        createUnsplittableCollection: collName,
                        dataShard: donor.shardName,
                    }),
                );
                const docs = [];
                for (let i = 0; i < 3; i++) {
                    docs.push({createdAt: new Date(), filler: i});
                }
                assert.commandWorked(testDB[collName].insertMany(docs));
            }

            function startCreateIndexInParallelShell(collName, indexName, commentTag) {
                return startParallelShell(
                    funWithArgs(
                        function (dbName, collName, indexName, commentTag) {
                            const cmd = {
                                createIndexes: collName,
                                indexes: [
                                    {
                                        key: {createdAt: 1},
                                        name: indexName,
                                        expireAfterSeconds: 3600,
                                    },
                                ],
                            };
                            if (commentTag) {
                                cmd.comment = commentTag;
                            }
                            assert.commandWorked(db.getSiblingDB(dbName).runCommand(cmd));
                        },
                        dbName,
                        collName,
                        indexName,
                        commentTag,
                    ),
                    st.s.port,
                );
            }

            // createIndexes reporting success must mean the index actually exists and is usable;
            // a subsequent collMod on it is what surfaced this bug as IndexNotFound in
            // production.
            function assertIndexWasActuallyCreated(collName, indexName) {
                const indexes = testDB[collName].getIndexes();
                assert.eq(
                    1,
                    indexes.filter((index) => index.name === indexName).length,
                    () =>
                        `${indexName} is missing even though createIndexes succeeded: ${tojson(indexes)}`,
                );

                assert.commandWorked(
                    testDB.runCommand({
                        collMod: collName,
                        index: {keyPattern: {createdAt: 1}, expireAfterSeconds: 100},
                    }),
                );
            }

            it("retries and creates the index when the collection is dropped concurrently", function () {
                const collName = "raceWithConcurrentDrop";
                const indexName = "createdAt_1";
                const commentTag = "createIndexRacingWithDrop";

                createUnsplittableCollectionWithDocs(collName);

                const collUUID = extractUUIDFromObject(
                    getUUIDFromListCollections(testDB, collName),
                );

                // createIndexes has resolved the UUID and released its locks, so dropCollection
                // can finish before the IndexBuildsCoordinator resolves that UUID again.
                const hangBeforeIndexBuild = configureFailPoint(
                    donor,
                    "hangCreateIndexesBeforeStartingIndexBuild",
                    {comment: commentTag},
                );

                let awaitCreateIndex;
                try {
                    awaitCreateIndex = startCreateIndexInParallelShell(
                        collName,
                        indexName,
                        commentTag,
                    );

                    hangBeforeIndexBuild.wait();

                    assert.commandWorked(testDB.runCommand({drop: collName}));
                } finally {
                    hangBeforeIndexBuild.off();
                    awaitCreateIndex?.();
                }

                const refersToTestCollection = (collectionUUID) =>
                    collectionUUID &&
                    collectionUUID.uuid &&
                    collectionUUID.uuid["$uuid"] === collUUID;
                assert(
                    checkLog.checkContainsOnceJson(donor, 13282200, {
                        collectionUUID: refersToTestCollection,
                    }),
                    "createIndexes did not retry after the NamespaceNotFound error",
                );

                assertIndexWasActuallyCreated(collName, indexName);
                assert.neq(
                    collUUID,
                    extractUUIDFromObject(getUUIDFromListCollections(testDB, collName)),
                    "createIndexes did not recreate the collection after the concurrent drop",
                );
            });

            it("fails the first UUID resolution: paused with a failpoint before reaching the IndexBuildsCoordinator", function () {
                const collName = "raceBeforeCoordinatorEntry";
                const ns = `${dbName}.${collName}`;
                const indexName = "createdAt_1";
                const commentTag = "createIndexRacingWithMoveCollection";

                createUnsplittableCollectionWithDocs(collName);

                // Pause createIndexes just before it hands off to the IndexBuildsCoordinator,
                // holding no locks yet, so moveCollection can commit underneath it. Resuming then
                // fails the first UUID resolution, in
                // IndexBuildsCoordinatorMongod::_startIndexBuild.
                const hangBeforeIndexBuild = configureFailPoint(
                    donor,
                    "hangCreateIndexesBeforeStartingIndexBuild",
                    {comment: commentTag},
                );

                const awaitCreateIndex = startCreateIndexInParallelShell(
                    collName,
                    indexName,
                    commentTag,
                );

                hangBeforeIndexBuild.wait();

                assert.commandWorked(
                    st.s.adminCommand({moveCollection: ns, toShard: recipient.shardName}),
                );

                hangBeforeIndexBuild.off();

                awaitCreateIndex();
                assertIndexWasActuallyCreated(collName, indexName);
            });

            it("fails the second UUID resolution: queued behind maxNumActiveUserIndexBuilds", function () {
                const collName = "raceWhileQueuedForThrottleSlot";
                // Keeps the single index build slot busy; must not be the collection being moved.
                const queueFillerCollName = collName + "QueueFiller";
                const ns = `${dbName}.${collName}`;
                const indexName = "createdAt_1";

                createUnsplittableCollectionWithDocs(collName);
                createUnsplittableCollectionWithDocs(queueFillerCollName);

                const collUUID = extractUUIDFromObject(
                    getUUIDFromListCollections(testDB, collName),
                );

                // Let the filler start even if an unrelated system build is already active. Once
                // it is paused, lower the limit so the test build must queue behind it.
                const maxNumActiveUserIndexBuildsForFiller = 1000;
                const originalMaxNumActiveUserIndexBuilds = assert.commandWorked(
                    donor.adminCommand({
                        setParameter: 1,
                        maxNumActiveUserIndexBuilds: maxNumActiveUserIndexBuildsForFiller,
                    }),
                ).was;

                let hangRegisteredBuild;
                let awaitQueueFillerIndex;
                let awaitCreateIndex;
                try {
                    // Occupies the only slot with an unrelated build, paused after taking the
                    // slot so it stays held until the failpoint is lifted.
                    hangRegisteredBuild = configureFailPoint(
                        donor,
                        "hangAfterRegisteringIndexBuild",
                    );

                    awaitQueueFillerIndex = startParallelShell(
                        funWithArgs(
                            function (dbName, collName) {
                                assert.soon(
                                    () => {
                                        const result = db.getSiblingDB(dbName).runCommand({
                                            createIndexes: collName,
                                            indexes: [{key: {filler: 1}, name: "filler_1"}],
                                        });
                                        if (result.ok) {
                                            return true;
                                        }

                                        if (
                                            result.code === ErrorCodes.FailedToSatisfyReadPreference
                                        ) {
                                            return false;
                                        }
                                        assert.commandWorked(result);
                                    },
                                    "failed to start the queue-filler index build",
                                    10 * 60 * 1000,
                                );
                            },
                            dbName,
                            queueFillerCollName,
                        ),
                        st.s.port,
                    );

                    hangRegisteredBuild.wait();

                    assert.commandWorked(
                        donor.adminCommand({setParameter: 1, maxNumActiveUserIndexBuilds: 1}),
                    );

                    awaitCreateIndex = startCreateIndexInParallelShell(collName, indexName);

                    // Wait until the build for the collection about to be moved is queued: it has
                    // resolved its UUID and dropped all locks, and will only look it up again
                    // once a slot frees up.
                    const refersToTestCollection = (collectionUUID) =>
                        collectionUUID &&
                        collectionUUID.uuid &&
                        collectionUUID.uuid["$uuid"] === collUUID;
                    checkLog.containsJson(
                        donor,
                        4715500,
                        {collectionUUID: refersToTestCollection},
                        10 * 60 * 1000,
                    );

                    assert.commandWorked(
                        st.s.adminCommand({moveCollection: ns, toShard: recipient.shardName}),
                    );

                    // Let the queued build resume now that the collection it was going to build
                    // on is gone.
                    hangRegisteredBuild.off();

                    awaitQueueFillerIndex();
                    awaitCreateIndex();

                    assert(
                        checkLog.checkContainsOnceJson(donor, 13282200, {
                            collectionUUID: refersToTestCollection,
                        }),
                        "the queued index build did not hit the NamespaceNotFound path this test is about",
                    );

                    assertIndexWasActuallyCreated(collName, indexName);
                } finally {
                    hangRegisteredBuild?.off();
                    awaitQueueFillerIndex?.();
                    awaitCreateIndex?.();

                    assert.commandWorked(
                        donor.adminCommand({
                            setParameter: 1,
                            maxNumActiveUserIndexBuilds: originalMaxNumActiveUserIndexBuilds,
                        }),
                    );
                }
            });

            it("gives up instead of retrying forever after repeatedly losing the race", function () {
                const collName = "raceExhaustsRetries";
                const indexName = "createdAt_1";
                // Must match kMaxNamespaceNotFoundRetryAttempts in create_indexes_cmd.cpp.
                const maxRetryAttempts = 10;

                createUnsplittableCollectionWithDocs(collName);

                // Forces createIndexes to hit NamespaceNotFound on every attempt, as if it kept
                // losing a race with a concurrent drop or move, without needing to actually win
                // that race maxRetryAttempts + 1 times in a row.
                const failWithNamespaceNotFound = configureFailPoint(
                    donor,
                    "createIndexesFailWithNamespaceNotFoundBeforeStartingIndexBuild",
                );

                try {
                    assert.commandFailedWithCode(
                        testDB.runCommand({
                            createIndexes: collName,
                            indexes: [
                                {
                                    key: {createdAt: 1},
                                    name: indexName,
                                    expireAfterSeconds: 3600,
                                },
                            ],
                        }),
                        ErrorCodes.ConflictingOperationInProgress,
                    );
                } finally {
                    failWithNamespaceNotFound.off();
                }

                assert(
                    checkLog.checkContainsOnceJson(donor, 13282201, {
                        attempts: maxRetryAttempts,
                    }),
                    "createIndexes did not report exhausting its NamespaceNotFound retries",
                );

                assert.eq(
                    0,
                    testDB[collName].getIndexes().filter((index) => index.name === indexName)
                        .length,
                    "createIndexes should not have created the index after giving up",
                );
            });
        });
    }
});
