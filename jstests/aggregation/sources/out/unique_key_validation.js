/**
 * Tests for the validation of the "uniqueKey" at parse-time of the "uniqueKey" specification
 * itself, as well as during runtime extraction of the "uniqueKey" from documents in the aggregation
 * pipeline.
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
    // Tests for invalid "uniqueKey" specifications.
    //
    function assertUniqueKeyIsInvalid(uniqueKey, expectedErrorCode) {
        const stage = {
            $out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: uniqueKey}
        };
        assertErrorCode(source, stage, expectedErrorCode);
    }

    // A non-object "uniqueKey" is prohibited.
    assertUniqueKeyIsInvalid(3.14, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid("_id", ErrorCodes.TypeMismatch);

    // Explicitly specifying an empty-object "uniqueKey" is invalid.
    assertUniqueKeyIsInvalid({}, ErrorCodes.InvalidOptions);

    // The "uniqueKey" won't be accepted if any field is not a number.
    assertUniqueKeyIsInvalid({name: "hashed"}, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid({x: 1, y: 1, z: [1]}, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid({nested: {field: 1}}, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid({uniqueKey: true}, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid({string: "true"}, ErrorCodes.TypeMismatch);
    assertUniqueKeyIsInvalid({bool: false}, ErrorCodes.TypeMismatch);

    // A numerical "uniqueKey" won't be accepted if any field isn't exactly the value 1.
    assertUniqueKeyIsInvalid({_id: -1}, ErrorCodes.BadValue);
    assertUniqueKeyIsInvalid({x: 10}, ErrorCodes.BadValue);

    // Test that the value 1 represented as different numerical types will be accepred.
    [1.0, NumberInt(1), NumberLong(1), NumberDecimal(1)].forEach(one => {
        assert.commandWorked(target.remove({}));
        assert.doesNotThrow(
            () => source.aggregate(
                {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {_id: one}}}));
        assert.eq(target.find().toArray(), [{_id: 0}]);
    });

    //
    // An error is raised if $out encounters a document that is missing one or more of the
    // "uniqueKey" fields.
    //
    assert.commandWorked(target.remove({}));
    assert.commandWorked(target.createIndex({name: 1, team: -1}, {unique: true}));
    const pipelineNameTeam =
        [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {name: 1, team: 1}}}];

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
    // An error is raised if $out encounters a document where one of the "uniqueKey" fields is a
    // nullish value.
    //
    assert.commandWorked(target.remove({}));
    assert.commandWorked(target.createIndex({"song.artist": 1}, {unique: 1}));
    const pipelineSongDotArtist =
        [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {"song.artist": 1}}}];

    // Explicit null "song" (a prefix of a "uniqueKey" field).
    assert.commandWorked(source.update({_id: 0}, {_id: 0, song: null}));
    assertErrorCode(source, pipelineSongDotArtist, 51132);

    // Explicit undefined "song" (a prefix of a "uniqueKey" field).
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
    // An error is raised if $out encounters a document where one of the "uniqueKey" fields (or a
    // prefix of a "uniqueKey" field) is an array.
    //
    assert.commandWorked(target.remove({}));
    assert.commandWorked(target.createIndex({"address.street": 1}, {unique: 1}));
    const pipelineAddressDotStreet = [
        {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {"address.street": 1}}}
    ];

    // "address.street" is an array.
    assert.commandWorked(
        source.update({_id: 0}, {_id: 0, address: {street: ["West 43rd St", "1633 Broadway"]}}));
    assertErrorCode(source, pipelineAddressDotStreet, 51185);

    // "address" is an array (a prefix of a "uniqueKey" field).
    assert.commandWorked(source.update({_id: 0}, {_id: 0, address: [{street: "1633 Broadway"}]}));
    assertErrorCode(source, pipelineAddressDotStreet, 51132);

    // A scalar "address.street" is accepted.
    assert.commandWorked(source.update({_id: 0}, {_id: 0, address: {street: "1633 Broadway"}}));
    assert.doesNotThrow(() => source.aggregate(pipelineAddressDotStreet));
    assert.eq(target.find().toArray(), [{_id: 0, address: {street: "1633 Broadway"}}]);
}());
