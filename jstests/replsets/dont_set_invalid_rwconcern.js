/**
 * Tests valid and invalid writeConcern settings, and that both setDefaultRWConcern and inserting
 * with a writeConcern set will succeed and fail on the same values of writeConcern.
 */

(function() {
"use strict";
// Define repl set with custom write concern multiRegion which assures that writes are propagated to
// two different regions (specified in node tags).
const rst = new ReplSetTest({
    name: 'testSet',
    nodes: [
        {rsConfig: {tags: {region: "us"}}},
        {rsConfig: {tags: {region: "us"}}},
        {rsConfig: {tags: {region: "eu"}}}
    ],
    settings: {getLastErrorModes: {multiRegion: {region: 2}}}
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

// Test setDefaultRWConcern succeeds for "majority" and numbers.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}}));
assert.commandWorked(primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}));

// Test setDefaultRWConcern will fail for anything besides integers and strings.
assert.commandFailedWithCode(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: true}}),
    ErrorCodes.FailedToParse);

// Test setDefaultRWConcern will fail for general strings (besides majority).
assert.commandFailedWithCode(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "1"}}),
    ErrorCodes.UnknownReplWriteConcern);
assert.commandFailedWithCode(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "bajority"}}),
    ErrorCodes.UnknownReplWriteConcern);

// Test setDefaultRWConcern will succeed for custom RW concerns.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));

// Test insert with a writeConcern set succeeds and fails in the same cases that setDefaultRWConcern
// does.
const coll = primary.getDB("db").getCollection("coll");
assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: 1}}));
assert.commandFailedWithCode(coll.insert({a: 1}, {writeConcern: {w: true}}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(coll.insert({a: 1}, {writeConcern: {w: "1"}}),
                             ErrorCodes.UnknownReplWriteConcern);
assert.commandFailedWithCode(coll.insert({a: 1}, {writeConcern: {w: "bajority"}}),
                             ErrorCodes.UnknownReplWriteConcern);
assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: "multiRegion"}}));

rst.stopSet();
})();
