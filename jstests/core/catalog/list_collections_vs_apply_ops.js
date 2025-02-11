/*
 * Tests the expected output of listCollections against metadata created through
 * applyOps (replication on secondaries, internal temporary collections).
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps.
 *   not_allowed_with_signed_security_token,
 *   # Cannot implicitly shard accessed collections because of collection existing when none
 *   # expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   requires_getmore,
 *   requires_replication,
 *   uses_api_parameters,
 * ]
 */

import {cursorCountMatching, getListCollectionsCursor} from 'jstests/core/catalog/libs/helpers.js';

let mydb = db.getSiblingDB("list_collections_vs_apply_ops");

jsTest.log('Test that the collection metadata object is returned correctly');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("userColl"));
    assert.commandWorked(mydb.runCommand(
        {applyOps: [{op: "c", ns: mydb.getName() + ".$cmd", o: {create: "bar", temp: true}}]}));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(mydb), function(c) {
                  return c.name === "userColl" && c.options.temp === undefined;
              }));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(mydb), function(c) {
                  return c.name === "bar" && c.options.temp === true;
              }));
}

jsTest.log('The "filter" option of listCollections may target temporary collections');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("userColl"));
    assert.commandWorked(mydb.runCommand({
        applyOps: [{op: "c", ns: mydb.getName() + ".$cmd", o: {create: "tempColl", temp: true}}]
    }));

    assert.eq(2, getListCollectionsCursor(mydb, {
                     filter: {name: {$in: ["userColl", "tempColl"]}}
                 }).itcount());
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {"options.temp": true}}).itcount());
    mydb.tempColl.drop();
    assert.eq(1, getListCollectionsCursor(mydb, {
                     filter: {name: {$in: ["userColl", "tempColl"]}}
                 }).itcount());
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {name: /^userColl$/}}).itcount());
    assert.eq(0, getListCollectionsCursor(mydb, {filter: {"options.temp": true}}).itcount());
}
