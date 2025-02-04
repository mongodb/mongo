/**
 * Confirms mongot index commands on views fail when the corresponding
 * feature flag is turned turned off.
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {createSearchIndex, dropSearchIndex, updateSearchIndex} from "jstests/libs/search.js";

// TODO SERVER-92932 Remove this test when 'featureFlagMongotIndexedViews' is removed.
const hasFeatureFlagMongotIndexedViewsEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagMongotIndexedViews: 1}))
        .featureFlagMongotIndexedViews;

if (!hasFeatureFlagMongotIndexedViewsEnabled) {
    const testDb = db.getSiblingDB(jsTestName());
    const coll = testDb.underlyingSourceCollection;
    assertDropCollection(testDb, coll.getName());

    // Have to populate the collection in order for it to exist on mongot.
    assert.commandWorked(coll.insertMany([
        {state: "NY", pop: 19000000, facts: {state_motto: "Excelsior", state_flower: "Rose"}},
        {
            state: "CA",
            pop: 39000000,
            facts: {state_motto: "Eureka", state_flower: "California Poppy"}
        },
        {
            state: "NJ",
            pop: 9000000,
            facts: {state_motto: "Liberty and Prosperity", state_flower: "Common Blue Violet"}
        },
        {
            state: "AK",
            pop: 3000000,
            facts: {state_motto: "Regnat Populus", state_flower: "Forget-Me-Not"}
        },
    ]));

    let viewName = "addFields";
    assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', [
        {"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}
    ]));
    let addFieldsView = testDb[viewName];

    // Mongot index commands should fail without feature flag turned on.
    let indexDef = {
        mappings: {dynamic: true, fields: {}},
        storedSource: {exclude: ["facts.state_motto"]}
    };
    assert.throwsWithCode(
        () => createSearchIndex(addFieldsView, {name: "addFieldsIndex", definition: indexDef}),
        ErrorCodes.QueryFeatureNotAllowed);

    indexDef.storedSource = {exclude: ["facts.state_flower"]};
    assert.throwsWithCode(
        () => updateSearchIndex(addFieldsView, {name: "addFieldsIndex", definition: indexDef}),
        ErrorCodes.QueryFeatureNotAllowed);

    assert.throwsWithCode(() => dropSearchIndex(addFieldsView, {name: "addFieldsIndex"}),
                          ErrorCodes.QueryFeatureNotAllowed);
}
