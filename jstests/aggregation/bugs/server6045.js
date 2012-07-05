/* @file : jstests/aggregation/bugs/server6045.js
 *
 * SERVER 6045 : aggregate cmd crashes server with empty pipeline argument
 *
 * This test validates part of SERVER 6045 ticket. Return errmsg upon blank
 * document in pipeline. Previously blank documents in pipeline would cause
 * server to crash
 */

/*
 * 1) Grab aggdb
 * 2) Empty aggdb
 * 3) Populate aggdb
 * 4) Run aggregate with an empty document as the pipeline, at the start of the
 *    pipeline, at the end of the pipeline, and in the middle of the pipeline
 * 5) Assert that all four position return the expected error
 */

// Use aggdb
db = db.getSiblingDB('aggdb');

// Empty and fill aggdb
db.agg.drop();
db.agg.insert({key: "string", value: 17});
db.agg.insert({key: "yarn", value: 42});

// As pipeline
var s6045p1 = db.runCommand({aggregate:"aggtype", pipeline: [{}]});
// Start of pipeline
var s6045p2 = db.runCommand({aggregate:"aggtype", pipeline: [
                           { $project : {
                               value : 1,
                           } },
                           {}
                           ]});
// End of pipeline
var s6045p3 = db.runCommand({aggregate:"aggtype", pipeline: [
                           {},
                           { $project : {
                               value : 1,
                           } }
                           ]});
// Middle of pipeline
var s6045p4 = db.runCommand({aggregate:"aggtype", pipeline: [
                           { $project : {
                               value : 1,
                           } },
                           {},
                           { $project : {
                               value : 1,
                           } }
                           ]});
// Expected result
var a6045 = {
	"errmsg" : "Pipeline received empty document as argument",
	"ok" : 0
};

// Asserts
assert.eq(s6045p1, a6045, 'server6045 failed' + s6045p1);
assert.eq(s6045p2, a6045, 'server6045 failed' + s6045p2);
assert.eq(s6045p3, a6045, 'server6045 failed' + s6045p3);
assert.eq(s6045p4, a6045, 'server6045 failed' + s6045p4);
