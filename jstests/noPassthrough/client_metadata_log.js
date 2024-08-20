/**
 * Test that verifies client metadata is logged into log file on new connections.
 * @tags: [
 *   requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let checkLogForMetadata = function(mongo, options) {
    const normalizedOptions = JSON.parse(JSON.stringify(options));
    if (!normalizedOptions.networkMessageCompressors)
        normalizedOptions.networkMessageCompressors = [];
    let argv = ["mongo", "--eval", "tostrictjson(db.hello());", "--port", mongo.port];
    if (normalizedOptions.networkMessageCompressors.length > 0)
        argv.push(
            `--networkMessageCompressors=${normalizedOptions.networkMessageCompressors.join(',')}`);
    assert.eq(runMongoProgram(...argv), 0);

    print(`Checking ${mongo.fullOptions.logFile} for client metadata message`);
    let log = cat(mongo.fullOptions.logFile);

    const predicate =
        /"id":51800,.*"msg":"client metadata","attr":.*"doc":{"application":{"name":".*"},"driver":{"name":".*","version":".*"},"os":{"type":".*","name":".*","architecture":".*","version":".*"}}/g;

    const matches = log.match(predicate);
    assert(matches,
           "'client metadata' log line missing in log file!\n" +
               "Log file contents: " + mongo.fullOptions.logFile +
               "\n************************************************************\n" + log +
               "\n************************************************************");
    const negotiatedCompressorsMatches =
        matches[matches.length - 1].match(/"negotiatedCompressors":(\[[^\]]*\])/);
    assert(negotiatedCompressorsMatches,
           "'client metadata' log line did not include negotiated compressors\n" +
               "Log file contents: " + mongo.fullOptions.logFile +
               "\n************************************************************\n" + log +
               "\n************************************************************");
    const negotiatedCompressors = JSON.parse(negotiatedCompressorsMatches[1]);
    negotiatedCompressors.sort();
    normalizedOptions.networkMessageCompressors.sort();
    assert(JSON.stringify(negotiatedCompressors) ===
               JSON.stringify(normalizedOptions.networkMessageCompressors),
           "'client metadata' log line reports unexpected negotiated compressors:\n" +
               `included [${negotiatedCompressors.join(', ')}]\n` +
               `expected [${normalizedOptions.networkMessageCompressors.join(', ')}]`);
};

// Test MongoD
let testMongoD = function(options) {
    let runOptions = JSON.parse(JSON.stringify(options));
    runOptions.useLogFiles = true;
    let mongo = MongoRunner.runMongod(runOptions);
    assert.neq(null, mongo, 'mongod was unable to start up');

    checkLogForMetadata(mongo, options);

    MongoRunner.stopMongod(mongo);
};

// Test MongoS
let testMongoS = function(options) {
    let runOptions = JSON.parse(JSON.stringify(options));
    runOptions.useLogFiles = true;
    let otherOptions = {
        mongosOptions: runOptions,
    };

    let st = new ShardingTest({shards: 1, mongos: 1, other: otherOptions});

    checkLogForMetadata(st.s0, options);

    // Validate db.currentOp() contains mongos information
    let curOp = st.s0.adminCommand({currentOp: 1});
    print(tojson(curOp));

    var inprogSample = null;
    for (let inprog of curOp.inprog) {
        if (inprog.hasOwnProperty("clientMetadata") &&
            inprog.clientMetadata.hasOwnProperty("mongos")) {
            inprogSample = inprog;
            break;
        }
    }

    assert.neq(inprogSample.clientMetadata.mongos.host, "unknown");
    assert.neq(inprogSample.clientMetadata.mongos.client, "unknown");
    assert.neq(inprogSample.clientMetadata.mongos.version, "unknown");

    st.stop();
};

const optionCases = [
    {},
    {networkMessageCompressors: ['snappy', 'zstd']},
];
for (const optionCase of optionCases) {
    testMongoD(optionCase);
    testMongoS(optionCase);
}
