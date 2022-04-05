// Test setUserWriteBlockMode command.
//
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_replication,
//   featureFlagUserWriteBlocking,
// ]

load("jstests/noPassthrough/libs/user_write_blocking.js");

(function() {
'use strict';

const {
    WriteBlockState,
    ShardingFixture,
    ReplicaFixture,
    bypassUser,
    noBypassUser,
    password,
    keyfile
} = UserWriteBlockHelpers;

function runTest(fixture) {
    // For this test to work, we expect the state of the collection passed to be a single {a: 2}
    // document. This test is expected to maintain that state.
    function testCUD(coll, shouldSucceed, expectedFailure) {
        // Ensure we successfully maintained state from last run.
        assert.eq(0, coll.find({a: 1}).count());
        assert.eq(1, coll.find({a: 2}).count());

        if (shouldSucceed) {
            assert.commandWorked(coll.insert({a: 1}));
            assert.eq(1, coll.find({a: 1}).count());
            assert.commandWorked(coll.update({a: 1}, {a: 1, b: 2}));
            assert.eq(1, coll.find({a: 1, b: 2}).count());
            assert.commandWorked(coll.remove({a: 1}));
        } else {
            assert.commandFailedWithCode(coll.insert({a: 1}), expectedFailure);
            assert.commandFailedWithCode(coll.update({a: 2}, {a: 2, b: 2}), expectedFailure);
            assert.eq(0, coll.find({a: 2, b: 2}).count());
            assert.commandFailedWithCode(coll.remove({a: 2}), expectedFailure);
        }

        // Ensure we successfully maintained state on this run.
        assert.eq(0, coll.find({a: 1}).count());
        assert.eq(1, coll.find({a: 2}).count());
    }

    // Set up backing collections
    fixture.asUser(({coll}) => assert.commandWorked(coll.insert({a: 2})));

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    // Ensure that without setUserWriteBlockMode, both users are privileged for CUD ops
    fixture.asUser(({coll}) => testCUD(coll, true));
    fixture.asAdmin(({coll}) => testCUD(coll, true));

    fixture.enableWriteBlockMode();

    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    // Now with setUserWriteBlockMode enabled, ensure that only the bypassUser can CUD
    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asUser(({coll}) => {
        testCUD(coll, false, ErrorCodes.OperationFailed);
    });

    // Restarting the cluster has no impact, as write block state is durable
    fixture.restart();

    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asUser(({coll}) => {
        testCUD(coll, false, ErrorCodes.OperationFailed);
    });

    // Now disable userWriteBlockMode and ensure both users can CUD again

    fixture.disableWriteBlockMode();

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    fixture.asUser(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });

    if (fixture.takeGlobalLock) {
        let globalLock = fixture.takeGlobalLock();
        try {
            fixture.assertWriteBlockMode(WriteBlockState.UNKNOWN);
        } finally {
            globalLock.unlock();
        }
    }
}

{
    // Validate that setting user write blocking fails on standalones
    const conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
    const admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand(
        {createUser: "root", pwd: "root", roles: [{role: "__system", db: "admin"}]}));
    assert(admin.auth("root", "root"));

    assert.commandFailedWithCode(admin.runCommand({setUserWriteBlockMode: 1, global: true}),
                                 ErrorCodes.IllegalOperation);
    MongoRunner.stopMongod(conn);
}

// Test on replset primary
const rst = new ReplicaFixture();
runTest(rst);
rst.stop();

// Test on a sharded cluster
const st = new ShardingFixture();
runTest(st);
st.stop();
})();
