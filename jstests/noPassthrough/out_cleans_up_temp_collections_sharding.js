/*
 * Tests that $out on sharded clusters cleans up the temporary collections on failure.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagAggOutTimeseries,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const dbName = 'test';

const st = new ShardingTest({shards: 1});

const testDB = st.s.getDB(dbName);
const sourceColl = testDB['foo'];
const outColl = testDB['out'];

assert.commandWorked(sourceColl.insert({x: 1}));

function runOut(dbName, sourceCollName, targetCollName, expectCommandWorked, timeseries) {
    const cmdRes = db.getSiblingDB(dbName).runCommand({
        aggregate: sourceCollName,
        pipeline: [{
            $out:
                {db: dbName, coll: targetCollName, timeseries: timeseries ? {timeField: 't'} : null}
        }],
        cursor: {},
        comment: "testComment",
    });
    if (expectCommandWorked) {
        assert.commandWorked(cmdRes);
    } else {
        assert.commandFailed(cmdRes);
    }
}

function getTempCollections() {
    return testDB.getCollectionNames().filter(coll => coll.startsWith('tmp.agg_out'));
}

function failFn_sigkill() {
    const SIGKILL = 9;
    const opts = {allowedExitCode: MongoRunner.EXIT_SIGKILL};
    st.rs0.restart(st.rs0.getPrimary(), opts, SIGKILL);
    st.rs0.awaitNodesAgreeOnPrimary();
}

function failFn_killOp() {
    let adminDB = st.rs0.getPrimary().getDB("admin");
    // The create coordinator issues fire and forget refreshes after creating a collection. We
    // filter these out to ensure we are killing the correct operation.
    const curOps = adminDB
                       .aggregate([
                           {$currentOp: {allUsers: true}},
                           {
                               $match: {
                                   "command.comment": "testComment",
                                   "command._flushRoutingTableCacheUpdates": {$exists: false}
                               }
                           }
                       ])
                       .toArray();
    assert.eq(1, curOps.length, curOps);
    adminDB.killOp(curOps[0].opid);
}

function failFn_dropDbAndSigKill() {
    testDB.dropDatabase();
    failFn_sigkill();
}

function failFn_dropDbAndKillOp() {
    testDB.dropDatabase();
    failFn_killOp();
}

function testFn(timeseries, failFn) {
    assert.eq(0, getTempCollections().length);

    const shardPrimaryNode = st.rs0.getPrimary();
    const fp = configureFailPoint(
        shardPrimaryNode, 'outWaitAfterTempCollectionCreation', {shouldCheckForInterrupt: true});

    let outShell = startParallelShell(funWithArgs(runOut,
                                                  testDB.getName(),
                                                  sourceColl.getName(),
                                                  outColl.getName(),
                                                  false /*expectCommandWorked*/,
                                                  timeseries),
                                      st.s.port);

    fp.wait();

    // Check temp coll created.
    let tempCollections = getTempCollections();
    assert.eq(1, tempCollections.length, tempCollections);

    // Check the temporary collection was annotated to the garbage-collector collection.
    assert.eq(1,
              shardPrimaryNode.getDB('config')['agg_temp_collections'].count(
                  {_id: dbName + '.' + tempCollections[0]}));

    // Provoke failure.
    failFn();

    outShell();

    // Check temp coll deleted
    // assert.soon because the garbage-collection happens asynchronously after stepup.
    assert.soon(() => {
        let tempCollections = getTempCollections();
        let garbageCollectionEntries =
            st.rs0.getPrimary().getDB('config')['agg_temp_collections'].count();

        return tempCollections.length === 0 && garbageCollectionEntries === 0;
    });
}

jsTest.log("Running test with normal collection and SIGKILL");
testFn(false, failFn_sigkill);

jsTest.log("Running test with normal collection and dropDbAndSigKill");
testFn(false, failFn_dropDbAndSigKill);
assert.commandWorked(sourceColl.insert({x: 1}));

jsTest.log("Running test with normal collection and killOp");
testFn(false, failFn_killOp);

jsTest.log("Running test with normal collection and dropDbAndkillOp");
testFn(false, failFn_dropDbAndKillOp);
assert.commandWorked(sourceColl.insert({x: 1}));

jsTest.log("Running test with timeseries collection and SIGKILL");
testFn(true, failFn_sigkill);

jsTest.log("Running test with timeseries collection and killOp");
testFn(true, failFn_killOp);

st.stop();
