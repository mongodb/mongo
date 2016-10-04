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

load('jstests/aggregation/extras/utils.js');

// Use aggdb
db = db.getSiblingDB('aggdb');

// Empty and fill aggdb
db.agg.drop();
db.agg.insert({key: "string", value: 17});
db.agg.insert({key: "yarn", value: 42});

// As pipeline
assertErrorCode(db.agg, [{}], 40323);
// Start of pipeline
assertErrorCode(db.agg, [{$project: {value: 1}}, {}], 40323);
// End of pipeline
assertErrorCode(db.agg, [{}, {$project: {value: 1}}], 40323);
// Middle of pipeline
assertErrorCode(db.agg, [{$project: {value: 1}}, {}, {$project: {value: 1}}], 40323);
