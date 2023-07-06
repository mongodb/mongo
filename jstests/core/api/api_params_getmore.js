/**
 * Tests passing API parameters into 'getMore' commands.
 * @tags: [
 *   requires_getmore,
 *   uses_api_parameters,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");
testColl.drop();

const nDocs = 2;
const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

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

for (const initParams of apiParamCombos) {
    for (const continueParams of apiParamCombos) {
        const findCmd = addApiParams({find: testColl.getName(), batchSize: 1}, initParams);
        const cursorId = assert.commandWorked(testDB.runCommand(findCmd)).cursor.id;
        const compatibleParams = (continueParams === initParams);

        const getMoreCmd =
            addApiParams({getMore: cursorId, collection: testColl.getName()}, continueParams);

        jsTestLog(`Initial params: ${tojson(initParams)}, ` +
                  `continuing params: ${tojson(continueParams)}, ` +
                  `compatible: ${tojson(compatibleParams)}, ` +
                  `command: ${tojson(getMoreCmd)}`);
        const reply = testDB.runCommand(getMoreCmd);
        jsTestLog(`Reply: ${tojson(reply)}`);

        if (compatibleParams) {
            assert.commandWorked(reply);
        } else {
            assert.commandFailedWithCode(reply, ErrorCodes.APIMismatchError);
        }
    }
}