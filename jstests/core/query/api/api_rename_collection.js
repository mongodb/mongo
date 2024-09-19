/**
 * API version test around rename collection.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_zones,
 *   requires_fcv_81,
 *   requires_non_retryable_commands,
 *   simulate_atlas_proxy_incompatible,
 *   uses_api_parameters,
 * ]
 */

// API parameter variants we are about to test here.
const variants = [
    {},
    {apiVersion: "1"},
    {apiVersion: "1", apiStrict: true},
    {apiVersion: "1", apiStrict: false},
    {apiVersion: "1", apiStrict: false, apiDeprecationErrors: true},
    {apiVersion: "1", apiStrict: true, apiDeprecationErrors: true},
];

const collNamePrefix = "rename_coll_api_test_";
let collCounter = 0;

function getNewCollName() {
    return collNamePrefix + collCounter++;
}

function getNewColl() {
    let coll = db[getNewCollName()];
    coll.drop();
    return coll;
}

function filterResponseFields(response) {
    return Object.keys(response)
        .filter((field) => field !== '$clusterTime' && field !== 'operationTime')
        .sort();
}

function runCommandVariant(cmd, variant) {
    // Merge fields together, without clobbering original objects.
    const merged = Object.assign({}, cmd, variant);
    return db.adminCommand(merged);
}

function assertCollectionRenameWorked(response) {
    // Assert that operation worked.
    assert.commandWorked(response);
    // Assert that the response only contains the "ok" field and nothing else
    // except the generic $clusterTime and operationTime fields.
    assert.eq(["ok"], filterResponseFields(response));
    assert.eq(1, response.ok);
}

function assertCollectionRenameFailed(response) {
    // Assert that operation failed.
    assert.commandFailed(response);
    // Assert that the response only contains the allowed error fields.
    assert.eq(["code", "codeName", "errmsg", "ok"], filterResponseFields(response));
    assert.eq(0, response.ok);
    assert.eq("number", typeof response.code);
    assert.eq("string", typeof response.errmsg);
    assert.eq("string", typeof response.codeName);
}

function assertCollectionRenameFailedWithCode(response, code) {
    assertCollectionRenameFailed(response);
    assert.eq(code, response.code);
}

// Rename an existing collection with documents in it.
variants.forEach((variant) => {
    jsTest.log(`Rename collection with documents - variant ${tojson(variant)}`);

    const src = getNewColl();
    const dstName = getNewCollName();

    assert.commandWorked(src.insert([{x: 1}, {x: 2}, {x: 3}]));

    assert.eq(3, src.countDocuments({}));

    const baseCommand = {renameCollection: src.getFullName(), to: db.getName() + '.' + dstName};
    assertCollectionRenameWorked(runCommandVariant(baseCommand, variant));

    assert.eq(0, src.countDocuments({}));
    const dst = db[dstName];
    assert.eq(3, dst.countDocuments({}));
    dst.drop();
});

// Rename a non-existing collection.
variants.forEach((variant) => {
    jsTest.log(`Rename non-existing collection - variant ${tojson(variant)}`);

    const src = getNewColl();
    const dst = getNewColl();

    const baseCommand = {renameCollection: src.getFullName(), to: dst.getFullName()};
    assertCollectionRenameFailed(runCommandVariant(baseCommand, variant));
});

// Rename an existing collection with indexes.
variants.forEach((variant) => {
    jsTest.log(`Rename collection with indexes - variant ${tojson(variant)}`);

    const src = getNewColl();
    const dstName = getNewCollName();
    const existingDst = getNewColl();

    assert.commandWorked(src.insert([{a: 1}, {a: 2}]));
    assert.commandWorked(src.createIndexes([{a: 1}, {b: 1}]));

    assert.commandWorked(existingDst.insert({a: 100}));

    // Rename command fails where because the target collection already exists.
    const failCommand = {renameCollection: src.getFullName(), to: existingDst.getFullName()};
    assertCollectionRenameFailed(runCommandVariant(failCommand, variant));

    const originalNumberOfIndexes = src.getIndexes().length;
    const okCommand = {renameCollection: src.getFullName(), to: db.getName() + '.' + dstName};
    assertCollectionRenameWorked(runCommandVariant(okCommand, variant));
    assert.eq(0, src.countDocuments({}));

    const dst = db[dstName];
    assert.eq(2, dst.countDocuments({}));
    assert(db.getCollectionNames().indexOf(dst.getName()) >= 0);
    assert(db.getCollectionNames().indexOf(src.getName()) < 0);
    assert.eq(originalNumberOfIndexes, dst.getIndexes().length);
    assert.eq(0, src.getIndexes().length);
    dst.drop();

    existingDst.drop();
});

// Rename a collection and cause a name conflict with an existing collection.
variants.forEach((variant) => {
    jsTest.log(`Rename collection with existing target - variant ${tojson(variant)}`);

    const src = getNewColl();
    const dst = getNewColl();

    assert.commandWorked(src.insert({x: 1}));
    assert.commandWorked(dst.insert({x: 2}));

    assert.eq(1, src.countDocuments({x: 1}));
    assert.eq(1, dst.countDocuments({x: 2}));

    const failCommand = {renameCollection: src.getFullName(), to: dst.getName()};
    assertCollectionRenameFailed(runCommandVariant(failCommand, variant));

    assert.eq(1, src.countDocuments({x: 1}));
    assert.eq(1, dst.countDocuments({x: 2}));

    const okCommand = {
        renameCollection: src.getFullName(),
        to: dst.getFullName(),
        dropTarget: true
    };
    assertCollectionRenameWorked(runCommandVariant(okCommand, variant));

    assert.eq(0, src.countDocuments({x: 2}));
    assert.eq(1, dst.countDocuments({}));

    dst.drop();
});

// Test invalid or missing input parameters for renameCollection adminCommand.
{
    const src = getNewColl();
    assert.commandWorked(src.insert({x: 1}));

    // Test extra command parameters with apiVersion: "1" and apiStrict.
    assertCollectionRenameFailedWithCode(db.adminCommand({
        renameCollection: src.getFullName(),
        to: src.getFullName(),
        something: "foo",
        apiVersion: "1",
        apiStrict: true
    }),
                                         ErrorCodes.IDLUnknownField);

    // Test wrong input parameters with apiVersion: "1".
    assertCollectionRenameFailedWithCode(
        db.adminCommand({renameCollection: {}, to: src.getFullName(), apiVersion: "1"}),
        ErrorCodes.TypeMismatch);
    assertCollectionRenameFailedWithCode(
        db.adminCommand({renameCollection: src.getFullName(), to: {}, apiVersion: "1"}),
        ErrorCodes.TypeMismatch);
    assertCollectionRenameFailedWithCode(
        db.adminCommand(
            {renameCollection: {}, to: src.getFullName(), apiVersion: "1", apiStrict: true}),
        ErrorCodes.TypeMismatch);
    assertCollectionRenameFailedWithCode(
        db.adminCommand(
            {renameCollection: src.getFullName(), to: {}, apiVersion: "1", apiStrict: true}),
        ErrorCodes.TypeMismatch);

    src.drop();
}
