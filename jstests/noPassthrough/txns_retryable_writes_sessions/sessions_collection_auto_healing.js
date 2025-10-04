import {validateSessionsCollection} from "jstests/libs/sessions_collection.js";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

let timeoutMinutes = 5;

let startSession = {startSession: 1};
let conn = MongoRunner.runMongod({setParameter: "localLogicalSessionTimeoutMinutes=" + timeoutMinutes});

let admin = conn.getDB("admin");
let config = conn.getDB("config");

// Test that we can use sessions before the sessions collection exists.
{
    validateSessionsCollection(conn, false, false, timeoutMinutes);
    assert.commandWorked(admin.runCommand({startSession: 1}));
    validateSessionsCollection(conn, false, false, timeoutMinutes);
}

// Test that a refresh will create the sessions collection.
{
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
    validateSessionsCollection(conn, true, true, timeoutMinutes);
}

// Test that a refresh will (re)create the TTL index on the sessions collection.
{
    assert.commandWorked(config.system.sessions.dropIndex({lastUse: 1}));
    validateSessionsCollection(conn, true, false, timeoutMinutes);
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
    validateSessionsCollection(conn, true, true, timeoutMinutes);
}

MongoRunner.stopMongod(conn);

timeoutMinutes = 4;
conn = MongoRunner.runMongod({
    restart: conn,
    cleanData: false,
    setParameter: "localLogicalSessionTimeoutMinutes=" + timeoutMinutes,
});
admin = conn.getDB("admin");
config = conn.getDB("config");

// Test that a change to the TTL index expiration on restart will generate a collMod to change
// the expiration time.
{
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
    validateSessionsCollection(conn, true, true, timeoutMinutes);
}

MongoRunner.stopMongod(conn);
