/**
 * Test the behavior of aggregation containing $out with a concurrent DDL operation. When concurrent
 * DDL operation happens, the observed behavior should either be $out failing, or the same result as
 * if the 2 operations were not interleaved.
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function execDuringOutAgg(st, sourceColl, targetColl, failpointName, func) {
    let res = FixtureHelpers.runCommandOnAllShards({
        db: sourceColl.getDB().getSiblingDB("admin"),
        cmdObj: {
            configureFailPoint: failpointName,
            mode: "alwaysOn",
        }
    });
    res.forEach(cmdResult => assert.commandWorked(cmdResult));

    const aggCommand = {
        aggregate: sourceColl.getName(),
        pipeline: [{$out: {db: targetColl.getDB().getName(), coll: targetColl.getName()}}],
        cursor: {}
    };

    let aggThread = new Thread((host, dbName, aggCommand) => {
        const conn = new Mongo(host);
        return conn.getDB(dbName).runCommand(aggCommand);
    }, st.s.host, sourceColl.getDB().getName(), aggCommand);
    aggThread.start();

    waitForCurOpByFailPointNoNS(sourceColl.getDB(), failpointName);

    func();

    FixtureHelpers.runCommandOnAllShards({
        db: sourceColl.getDB().getSiblingDB("admin"),
        cmdObj: {
            configureFailPoint: failpointName,
            mode: "off",
        }
    });

    return aggThread.returnData();
}

const st = new ShardingTest({shards: 3, mongos: 1, rs: {nodes: 3}});
const sourceDB = st.s.getDB('sourceDB');
const sourceColl = sourceDB['sourceColl'];
const targetDB = st.s.getDB('destDB');
const targetColl = targetDB['destColl'];

function getTempOutColl() {
    const colls = targetDB.getCollectionNames();
    jsTest.log(`Collections: ${tojson(colls)}`);
    assert.eq(1, colls.length);
    const tmpAggCollName = colls[0];
    assert(tmpAggCollName.startsWith('tmp.agg_out.'));
    const tmpAggColl = targetDB[tmpAggCollName];
    return tmpAggColl;
}

function setup() {
    // Drop all DBs.
    sourceDB.dropDatabase();
    targetDB.dropDatabase();

    st.s.adminCommand({enableSharding: sourceDB.getName(), primaryShard: st.shard0.shardName});

    var batch = sourceColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 10; i++) {
        batch.insert({val: i});
    }
    assert.commandWorked(batch.execute());
}

/**
 * Assert that executing a concurrent DDL operation and agg results in the aggregate failing and the
 * DDL operation succeeding, or the result is the same as if they had been run serially.
 */
function assertSerializedOrError({desc, failpointName, setupFn, ddlFn}) {
    jsTestLog(desc);
    setupFn();
    const aggRes = execDuringOutAgg(st, sourceColl, targetColl, failpointName, ddlFn);

    const aggCommand = {
        aggregate: sourceColl.getName(),
        pipeline: [{$out: {db: targetColl.getDB().getName(), coll: targetColl.getName()}}],
        cursor: {}
    };

    if (!aggRes.ok) {
        return aggRes;
    }

    const collState = (dbName, collName) => {
        const coll = st.s.getDB(dbName).getCollection(collName);
        return ({
            exists: st.s.getDB(dbName).getCollectionInfos({name: collName}).length == 1,
            count: coll.countDocuments({}),
            docs: coll.find({}, {_id: 0}).sort({val: 1}).toArray(),
            placement: {
                shard0: st.shard0.getCollection(coll.getFullName()).countDocuments({}),
                shard1: st.shard1.getCollection(coll.getFullName()).countDocuments({}),
                shard2: st.shard2.getCollection(coll.getFullName()).countDocuments({}),
            },
        });
    };
    const summarizeState = () => ({
        targetColl: collState(targetDB.getName(), targetColl.getName()),
        sourceColl: collState(sourceDB.getName(), sourceColl.getName()),
    });

    const parallelState = summarizeState();

    setupFn();
    assert.commandWorked(sourceDB.runCommand(aggCommand));
    ddlFn();
    const serialADState = summarizeState();

    setupFn();
    ddlFn();
    assert.commandWorked(sourceDB.runCommand(aggCommand));
    const serialDAState = summarizeState();

    // If it's not equivalent to atleast one doc then assert an error.
    const sameAsAD = bsonUnorderedFieldsCompare(serialADState, parallelState) === 0;
    const sameAsDA = bsonUnorderedFieldsCompare(serialDAState, parallelState) === 0;
    if (!(sameAsAD || sameAsDA)) {
        throw doassert(
            `parallel $out and DDL operation test "${desc}" did not throw an error and the ` +
            `state of the database after the command was not equivalent to running them in ` +
            `serial.\n` +
            `parallel: ${tojson(parallelState)}\n` +
            `aggregate then DDL operation: ${tojson(serialADState)}\n` +
            `DDL operation then aggregate: ${tojson(serialDAState)}`);
    }
    return aggRes;
}

assert.commandWorked(assertSerializedOrError({
    desc: "Concurrent $out and moveCollection on target",
    failpointName: "hangWhileBuildingDocumentSourceOutBatch",
    setupFn() {
        setup();
        st.s.adminCommand({enableSharding: targetDB.getName(), primaryShard: st.shard1.shardName});
        targetColl.insertOne({val: "should get overwritten"});
    },
    ddlFn() {
        assert.commandWorked(st.s.adminCommand(
            {moveCollection: targetColl.getFullName(), toShard: st.shard2.shardName}));
    }
}));

assert.commandWorked(assertSerializedOrError({
    desc: "Concurrent $out and drop target collection",
    failpointName: "hangWhileBuildingDocumentSourceOutBatch",
    setupFn() {
        setup();
        st.s.adminCommand({enableSharding: targetDB.getName(), primaryShard: st.shard1.shardName});
        targetColl.insertOne({val: "should get overwritten"});
    },
    ddlFn() {
        assert(sourceColl.drop());
    }
}));

// TODO SERVER-97621 improve test harness to deal with placement issues when $out creates the db
for (const targetDBExists of [true]) {
    const targetDBDesc = ` and targetDB does${targetDBExists ? ' ' : ' not '}exist`;
    const maybeCreateTargetDB = () => {
        if (targetDBExists) {
            st.s.adminCommand(
                {enableSharding: targetDB.getName(), primaryShard: st.shard1.shardName});
        }
    };

    assert.commandWorked(assertSerializedOrError({
        desc: "Concurrent $out and moveCollection on source" + targetDBDesc,
        failpointName: "hangWhileBuildingDocumentSourceOutBatch",
        setupFn() {
            setup();
            maybeCreateTargetDB();
        },
        ddlFn() {
            assert.commandWorked(st.s.adminCommand(
                {moveCollection: sourceColl.getFullName(), toShard: st.shard2.shardName}));
        }
    }));

    assert.commandWorked(assertSerializedOrError({
        desc: "Concurrent $out and movePrimary" + targetDBDesc,
        failpointName: "hangWhileBuildingDocumentSourceOutBatch",
        setupFn() {
            setup();
            maybeCreateTargetDB();
        },
        ddlFn() {
            sourceDB.adminCommand({movePrimary: targetDB.getName(), to: st.shard2.shardName});
        }
    }));

    // TODO SERVER-97621 Because this test drops the target database, it gets implicitly recreated
    // by the $out on a random database, so we can't compare the placement. Unblock this test after
    // we can predict where the placement of the implicitly created db is.
    /*
    assert.commandWorked(assertSerializedOrError({
        desc: "Concurrent $out and dropDatabase on targetDB" + targetDBDesc,
        failpointName: "hangWhileBuildingDocumentSourceOutBatch",
        setupFn() {
            setup();
            maybeCreateTargetDB();
        },
        ddlFn() {
            assert.commandWorked(targetDB.dropDatabase());
        }
    }));
    */
    assert.commandWorked(assertSerializedOrError({
        desc: "Concurrent $out and dropDatabase on sourceDB" + targetDBDesc,
        failpointName: "hangWhileBuildingDocumentSourceOutBatch",
        setupFn() {
            setup();
            maybeCreateTargetDB();
        },
        ddlFn() {
            assert.commandWorked(sourceDB.dropDatabase());
        }
    }));

    // This should fail since uuid changes and the reincarnation gets detected.
    assert.commandFailedWithCode(
        assertSerializedOrError({
            desc: "Concurrent $out and drop temp collection" + targetDBDesc,
            failpointName: "outWaitBeforeTempCollectionRename",
            setupFn() {
                setup();
                maybeCreateTargetDB();
            },
            ddlFn() {
                assert.eq(10, getTempOutColl().countDocuments({}));
                assert(getTempOutColl().drop());
            }
        }),
        ErrorCodes.NamespaceNotFound);

    assert.commandWorked(assertSerializedOrError({
        desc: "Concurrent $out and drop source collection" + targetDBDesc,
        failpointName: "hangWhileBuildingDocumentSourceOutBatch",
        setupFn() {
            setup();
            maybeCreateTargetDB();
        },
        ddlFn() {
            assert(sourceColl.drop());
        }
    }));

    if (!FeatureFlagUtil.isEnabled(st.s, "TrackUnshardedCollectionsUponCreation")) {
        assert.commandWorked(assertSerializedOrError({
            desc: "Concurrent $out and drop tracked source collection" + targetDBDesc,
            failpointName: "hangWhileBuildingDocumentSourceOutBatch",
            setupFn() {
                setup();
                st.s.adminCommand(
                    {moveCollection: sourceColl.getFullName(), toShard: st.shard0.shardName});
                maybeCreateTargetDB();
            },
            ddlFn() {
                assert(sourceColl.drop());
            }
        }));
    } else {
        assert.commandWorked(assertSerializedOrError({
            desc: "Concurrent $out and drop untracked source collection" + targetDBDesc,
            failpointName: "hangWhileBuildingDocumentSourceOutBatch",
            setupFn() {
                setup();
                st.s.adminCommand({untrackUnshardedCollection: sourceColl.getFullName()});
                maybeCreateTargetDB();
            },
            ddlFn() {
                assert(sourceColl.drop());
            }
        }));
    }
}

st.stop();
