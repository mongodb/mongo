// Test that killing a session logs a message.
//
// @tags: [requires_sharding]

"use strict";

function testFixtureSimple(name) {
    this.name = name + " (simple)";
    jsTest.log("Starting test: " + this.name);

    this.mongod = MongoRunner.runMongod();
    this.db = this.mongod.getDB("admin");
    this.db.createUser({user: 'user', pwd: 'pwd', roles: ['root']});

    jsTest.log(this.name + " initialization complete");

    this.check = function() {
        checkLog.contains(this.db, '"msg":"Success: kill session"', 15000);
        jsTest.log(this.name + " verified successfully");
    };

    this.stop = function() {
        jsTest.log(this.name + " shutting down");
        MongoRunner.stopMongod(this.mongod);
        jsTest.log(this.name + " shutdown complete");
    };
}

function testFixtureShard(name) {
    this.name = name + " (shard)";
    jsTest.log("Starting test: " + this.name);

    this.mongos = new ShardingTest({});
    this.db = this.mongos.getDB("admin");
    this.db.createUser({user: 'user', pwd: 'pwd', roles: ['root']});

    jsTest.log(this.name + " initialization complete");

    this.check = function() {
        checkLog.contains(this.db, '"msg":"Success: kill session"', 15000);
        jsTest.log(this.name + " verified successfully");
    };

    this.stop = function() {
        jsTest.log(this.name + " shutting down");
        this.mongos.stop();
        jsTest.log(this.name + " shutdown complete");
    };
}

const testKillSessions = function(fixture) {
    const randomSessionId = UUID("193bd705-3941-4be4-b463-5ca01f384e6f");
    assert.commandWorked(fixture.db.runCommand({killSessions: [{id: randomSessionId}]}));
    fixture.check();
    fixture.stop();
};

const testKillAllSessions = function(fixture) {
    assert.commandWorked(fixture.db.runCommand({killAllSessions: [{user: "user", db: "admin"}]}));
    fixture.check();
    fixture.stop();
};

const testKillAllSessionsByPattern = function(fixture) {
    fixture.db.runCommand({killAllSessionsByPattern: [{users: [{user: "user", db: "admin"}]}]});
    fixture.check();
    fixture.stop();
};

testKillSessions(new testFixtureSimple("testKillSessions"));
testKillAllSessions(new testFixtureSimple("testKillAllSessions"));
testKillAllSessionsByPattern(new testFixtureSimple("testKillAllSessionsByPattern"));
testKillSessions(new testFixtureShard("testKillSessions"));
testKillAllSessions(new testFixtureShard("testKillAllSessions"));
testKillAllSessionsByPattern(new testFixtureShard("testKillAllSessionsByPattern"));
