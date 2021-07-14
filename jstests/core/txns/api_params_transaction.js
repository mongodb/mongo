/**
 * Tests passing API parameters into transaction-continuing commands.
 * @tags: [uses_transactions, requires_fcv_50, uses_api_parameters]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.isMongos().
load(
    "jstests/libs/auto_retry_transaction_in_sharding.js");  // For
                                                            // retryOnceOnTransientAndRestartTxnOnMongos().

const dbName = jsTestName();
const collName = "test";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(
    testDB.runCommand({create: testColl.getName(), writeConcern: {w: "majority"}}));

const apiParamCombos = [
    {},
    {apiVersion: "1"},
    {apiVersion: "1", apiDeprecationErrors: true},
    {apiVersion: "1", apiDeprecationErrors: false},
    {apiVersion: "1", apiStrict: true},
    {apiVersion: "1", apiStrict: true, apiDeprecationErrors: true},
    {apiVersion: "1", apiStrict: true, apiDeprecationErrors: false},
    {apiVersion: "1", apiStrict: false},
    {apiVersion: "1", apiStrict: false, apiDeprecationErrors: true},
    {apiVersion: "1", apiStrict: false, apiDeprecationErrors: false}
];

function addApiParams(obj, params) {
    return Object.assign(Object.assign({}, obj), params);
}

for (const txnInitiatingParams of apiParamCombos) {
    for (const txnContinuingParams of apiParamCombos) {
        for (const txnEndingCmdName of ["commitTransaction", "abortTransaction"]) {
            const compatibleParams = (txnContinuingParams === txnInitiatingParams);
            const session = db.getMongo().startSession();
            const sessionDb = session.getDatabase(dbName);

            function checkCommand(db, command) {
                const commandWithParams = addApiParams(command, txnContinuingParams);
                jsTestLog(`Session ${session.getSessionId().id}, ` +
                          `initial params: ${tojson(txnInitiatingParams)}, ` +
                          `continuing params: ${tojson(txnContinuingParams)}, ` +
                          `compatible: ${tojson(compatibleParams)}`);
                jsTestLog(`Command: ${tojson(commandWithParams)}`);
                const reply = db.runCommand(commandWithParams);
                jsTestLog(`Reply: ${tojson(reply)}`);

                if (compatibleParams) {
                    assert.commandWorked(reply);
                } else {
                    assert.commandFailedWithCode(reply, ErrorCodes.APIMismatchError);
                }
            }

            session.startTransaction();
            retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
                assert.commandWorked(sessionDb.runCommand(addApiParams(
                    {insert: collName, documents: [{}, {}, {}]}, txnInitiatingParams)));

                /*
                 * Check "insert" with API params in a transaction.
                 */
                checkCommand(sessionDb, {insert: collName, documents: [{}]});
            }, {});

            /*
             * Check "commitTransaction" or "abortTransaction".
             */
            let txnEndingCmd = {};
            txnEndingCmd[txnEndingCmdName] = 1;
            Object.assign(txnEndingCmd,
                          {txnNumber: session.getTxnNumber_forTesting(), autocommit: false});

            checkCommand(session.getDatabase("admin"), txnEndingCmd);

            if (!compatibleParams) {
                jsTestLog('Cleaning up');
                // Clean up by calling abortTransaction with the right API parameters.
                const abortCmd = {
                    abortTransaction: 1,
                    txnNumber: session.getTxnNumber_forTesting(),
                    autocommit: false
                };
                const cleanupReply = session.getDatabase("admin").runCommand(
                    addApiParams(abortCmd, txnInitiatingParams));
                jsTestLog(`Cleanup reply ${tojson(cleanupReply)}`);
            }

            session.endSession();
        }
    }
}
})();
