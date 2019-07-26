/**
 * Tests for the validation of the "on" fields at parse-time of $merge stage itself, as well as
 * during runtime extraction of the "on" fields from documents in the aggregation pipeline.
 *
 * This test creates unique indexes on various combinations of fields, so it cannot be run in suites
 * that implicitly shard the collection with a hashed shard key.
 * @tags: [cannot_create_unique_index_when_using_hashed_shard_key]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

const source = db.unique_key_validation_source;
const target = db.unique_key_validation_target;

[source, target].forEach(coll => coll.drop());
assert.commandWorked(source.insert({_id: 0}));

//
// Tests for invalid "on" fields specifications.
//
function assertOnFieldsIsInvalid(onFields, expectedErrorCode) {
    const stage = {
        $merge:
            {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert", on: onFields}
    };
    assertErrorCode(source, stage, expectedErrorCode);
}

// A non-array or string "on" fields is prohibited.
assertOnFieldsIsInvalid(3.14, 51186);
assertOnFieldsIsInvalid({_id: 1}, 51186);

// Explicitly specifying an empty-array "on" fields is invalid.
assertOnFieldsIsInvalid([], 51187);

// The "on" fields array won't be accepted if any element is not a string.
assertOnFieldsIsInvalid(["hashed", 1], 51134);
assertOnFieldsIsInvalid([["_id"]], 51134);
assertOnFieldsIsInvalid([null], 51134);
assertOnFieldsIsInvalid([true, "a"], 51134);

//
// An error is raised if $merge encounters a document that is missing one or more of the
// "on" fields.
//
assert.commandWorked(target.remove({}));
assert.commandWorked(target.createIndex({name: 1, team: -1}, {unique: true}));
const pipelineNameTeam = [{
    $merge: {
        into: target.getName(),
        whenMatched: "replace",
        whenNotMatched: "insert",
        on: ["name", "team"]
    }
}];

// Missing both "name" and "team".
assertErrorCode(source, pipelineNameTeam, 51132);

// Missing "name".
assert.commandWorked(source.update({_id: 0}, {_id: 0, team: "query"}));
assertErrorCode(source, pipelineNameTeam, 51132);

// Missing "team".
assert.commandWorked(source.update({_id: 0}, {_id: 0, name: "nicholas"}));
assertErrorCode(source, pipelineNameTeam, 51132);

// A document with both "name" and "team" will be accepted.
assert.commandWorked(source.update({_id: 0}, {_id: 0, name: "nicholas", team: "query"}));
assert.doesNotThrow(() => source.aggregate(pipelineNameTeam));
assert.eq(target.find().toArray(), [{_id: 0, name: "nicholas", team: "query"}]);

//
// An error is raised if $merge encounters a document where one of the "on" fields is a nullish
// value.
//
assert.commandWorked(target.remove({}));
assert.commandWorked(target.createIndex({"song.artist": 1}, {unique: 1}));
const pipelineSongDotArtist = [{
    $merge: {
        into: target.getName(),
        whenMatched: "replace",
        whenNotMatched: "insert",
        on: ["song.artist"]
    }
}];

// Explicit null "song" (a prefix of an "on" field).
assert.commandWorked(source.update({_id: 0}, {_id: 0, song: null}));
assertErrorCode(source, pipelineSongDotArtist, 51132);

// Explicit undefined "song" (a prefix of an "on" field).
assert.commandWorked(source.update({_id: 0}, {_id: 0, song: undefined}));
assertErrorCode(source, pipelineSongDotArtist, 51132);

// Explicit null "song.artist".
assert.commandWorked(source.update({_id: 0}, {_id: 0, song: {artist: null}}));
assertErrorCode(source, pipelineSongDotArtist, 51132);

// Explicit undefined "song.artist".
assert.commandWorked(source.update({_id: 0}, {_id: 0, song: {artist: undefined}}));
assertErrorCode(source, pipelineSongDotArtist, 51132);

// A valid "artist" will be accepted.
assert.commandWorked(source.update({_id: 0}, {_id: 0, song: {artist: "Illenium"}}));
assert.doesNotThrow(() => source.aggregate(pipelineSongDotArtist));
assert.eq(target.find().toArray(), [{_id: 0, song: {artist: "Illenium"}}]);

//
// An error is raised if $merge encounters a document where one of the "on" fields (or a prefix
// of an "on" field) is an array.
//
assert.commandWorked(target.remove({}));
assert.commandWorked(target.createIndex({"address.street": 1}, {unique: 1}));
const pipelineAddressDotStreet = [{
    $merge: {
        into: target.getName(),
        whenMatched: "replace",
        whenNotMatched: "insert",
        on: ["address.street"]
    }
}];

// "address.street" is an array.
assert.commandWorked(
    source.update({_id: 0}, {_id: 0, address: {street: ["West 43rd St", "1633 Broadway"]}}));
assertErrorCode(source, pipelineAddressDotStreet, 51185);

// "address" is an array (a prefix of an "on" field).
assert.commandWorked(source.update({_id: 0}, {_id: 0, address: [{street: "1633 Broadway"}]}));
assertErrorCode(source, pipelineAddressDotStreet, 51132);

// A scalar "address.street" is accepted.
assert.commandWorked(source.update({_id: 0}, {_id: 0, address: {street: "1633 Broadway"}}));
assert.doesNotThrow(() => source.aggregate(pipelineAddressDotStreet));
assert.eq(target.find().toArray(), [{_id: 0, address: {street: "1633 Broadway"}}]);
}());
