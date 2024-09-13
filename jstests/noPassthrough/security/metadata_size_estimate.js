// Test the impact of having too many roles
// @tags: [requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Use a relatively small record size to more reliably hit a tipping point where the write batching
// logic thinks we have more space available for metadata than we really do. Note also that by using
// small records, we are verifying that the batching logic is accounting for the overhead required
// to serialize each document into a BSONArray.
const kDataBlockSize = 4 * 1024;
const kDataBlock = 'x'.repeat(kDataBlockSize);
const kBSONMaxObjSize = 16 * 1024 * 1024;
const kNumRows = (kBSONMaxObjSize / kDataBlockSize) + 5;
const kDBPrefix = 'qwertyuiopasdfghjklzxcvbnm_';

function runTest(conn) {
    const admin = conn.getDB('admin');
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(admin.auth('admin', 'pwd'));

    // Create more than 16KB of role data.
    // These roles are grouped into a meta-role to avoid calls to `usersInfo` unexpectedly
    // overflowing from duplication of roles/inheritedRoles plus showPrivileges.
    // bigRole has 100 roles: groupRole_[0..99]
    // Each groupRole has 100 subordinate builtin roles: read@prefix_XX[0..99]
    let firstDB = null;
    const userRoles = [];
    for (let i = 0; i < 100; ++i) {
        const roleRoles = [];
        for (let j = 0; j < 100; ++j) {
            const db = kDBPrefix + String((i * 100) + j);
            if (firstDB === null) {
                firstDB = db;
            }
            roleRoles.push({db: db, role: 'read'});
        }
        assert.commandWorked(
            admin.runCommand({createRole: 'groupRole_' + i, roles: roleRoles, privileges: []}));
        userRoles.push({db: 'admin', role: 'groupRole_' + i});
    }
    assert.commandWorked(
        admin.runCommand({createRole: 'bigRole', roles: userRoles, privileges: []}));
    assert.commandWorked(admin.runCommand({createUser: 'user', pwd: 'pwd', roles: ['bigRole']}));

    const db = conn.getDB(firstDB);

    // Fill a collection with enough rows to necessitate paging.
    for (let i = 1; i <= kNumRows; ++i) {
        assert.commandWorked(db.myColl.insert({_id: i, data: kDataBlock}));
    }
    // Verify initial write.
    assert.eq(kNumRows, db.myColl.count({}));

    // Switch to user with all the roles.
    admin.logout();
    assert(admin.auth('user', 'pwd'));

    // Create an aggregation which will batch up to kMaxWriteBatchSize or 16MB
    // (not counting metadata)
    assert.eq(0, db.myColl.aggregate([{"$out": 'yourColl'}]).itcount(), 'Aggregation failed');

    // Verify the $out stage completed.
    assert.eq(db.myColl.count({}), db.yourColl.count({}));
    assert.eq(kNumRows, db.yourColl.count({}));
}

{
    const st = new ShardingTest({mongos: 1, config: 1, shards: 1});
    runTest(st.s0);
    st.stop();
}