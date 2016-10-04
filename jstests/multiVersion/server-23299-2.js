/**
 * This test confirms that the behavior prescribed in SERVER-23299 is
 * only applied when starting up a node where the immediately prior
 * start-up was for versions 3.2.0 through 3.2.4, inclusive.
 */

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
    "use strict";

    var versionsSubjectToSERVER23299 = ['3.2.1'];

    // A smattering of versions not subject to the bug, but that we could legally encounter
    var versionsNotSubjectToSERVER23299 = ['latest', '3.0'];

    function doTest(priorVersion, expectTempToDrop) {
        var storageEngine = jsTest.options().storageEngine;
        jsTest.log((expectTempToDrop ? "" : " not") + " expecting temp collections created in " +
                   priorVersion + " to be dropped when starting latest mongod version");
        var mongod =
            MongoRunner.runMongod({binVersion: priorVersion, storageEngine: storageEngine});
        assert.binVersion(mongod, priorVersion);
        assert.commandWorked(mongod.getDB("test").createCollection("tempcoll", {temp: true}));
        assert.writeOK(mongod.getDB("test").tempcoll.insert({_id: 0}));
        assert.eq(1, mongod.getDB("test").tempcoll.find().itcount());

        MongoRunner.stopMongod(mongod);
        var newOpts = Object.extend({}, mongod.fullOptions);
        mongod = MongoRunner.runMongod(
            Object.extend(Object.extend({}, mongod.fullOptions),
                          {restart: true, binVersion: "latest", storageEngine: storageEngine}));
        assert.binVersion(mongod, "latest");
        assert.eq(expectTempToDrop ? 0 : 1, mongod.getDB("test").tempcoll.find().itcount());
    }

    versionsNotSubjectToSERVER23299.forEach(function(priorVersion) {
        doTest(priorVersion, true);
    });
    versionsSubjectToSERVER23299.forEach(function(priorVersion) {
        doTest(priorVersion, false);
    });
}());
