/**
 * Verify that DDL operations on timeseries bucket namesapces requires special authorization
 *
 * @tags: [
 * requires_fcv_61,
 * ]
 */

(function() {
'use strict';

const dbName = jsTest.name() + "_db";
const normalCollName = "normalColl";
const timeseriesCollName = "timeseriesColl";
const bucketCollName = "system.buckets." + timeseriesCollName;
const pass = "password";
const skeyPattern = {
    k: 1
};

const st = new ShardingTest({keyFile: "jstests/libs/key1", other: {shardOptions: {auth: ""}}});

// Create the admin user.
st.admin.createUser({user: "root", pwd: pass, roles: ["userAdminAnyDatabase"]});

assert(st.admin.auth("root", pass));

const db = st.s.getDB(dbName);

db.createUser({user: "rw", pwd: pass, roles: ["readWrite"]});
db.createUser({
    user: "c2c",
    pwd: pass,
    roles: [{db: "admin", role: "restore"}, {db: "admin", role: "backup"}]
});
st.admin.logout();

function createCollectionsAsRegularUser() {
    assert(db.auth("rw", pass));
    assert.commandWorked(db.createCollection(normalCollName));
    assert.commandWorked(
        db.createCollection(timeseriesCollName, {timeseries: {timeField: "time"}}));
    assert.commandFailedWithCode(db.createCollection(bucketCollName), ErrorCodes.Unauthorized);
    db.logout();
}

{
    createCollectionsAsRegularUser();
    assert(db.auth("rw", pass));
    assert.commandWorked(db.runCommand({drop: normalCollName}));
    assert.commandFailedWithCode(db.runCommand({drop: bucketCollName}), ErrorCodes.Unauthorized);
    assert.commandWorked(db.runCommand({drop: timeseriesCollName}));
    db.logout();
}

{
    createCollectionsAsRegularUser();
    assert(db.auth("c2c", pass));
    assert.commandWorked(db.runCommand({drop: normalCollName}));
    assert.commandWorked(db.runCommand({drop: bucketCollName}));
    assert.commandWorked(db.runCommand({drop: timeseriesCollName}));
    db.logout();
}

st.stop();
}());
