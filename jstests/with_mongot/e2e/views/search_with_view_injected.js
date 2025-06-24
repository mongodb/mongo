/**
 * This test checks that the server rejects any search query that attempts to inject a view into its
 * definition.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const testDb = db.getSiblingDB(jsTestName());
const teamColl = assertDropAndRecreateCollection(testDb, `${jsTest.name()}_teams`);
const playerColl = assertDropAndRecreateCollection(testDb, `${jsTest.name()}_players`);

assert.commandWorked(teamColl.insertMany([{
    _id: 1,
    name: "Sacramento Kings",
}]));

assert.commandWorked(playerColl.insertMany([{
    _id: 4,
    teamId: 1,
    name: "De'Aaron Fox",
}]));

const injectionNotAllowedErrorCode = 5491300;

const addFieldsViewName = "addFieldsView";
const addFieldsViewPipeline = [{$addFields: {number: 4}}];
assert.commandWorked(
    testDb.createView(addFieldsViewName, playerColl.getName(), addFieldsViewPipeline));

const matchViewName = "matchView";
const matchViewPipeline = [{$match: {name: "Sacramento Kings"}}];
assert.commandWorked(testDb.createView(matchViewName, teamColl.getName(), matchViewPipeline));

const injectingViewTest = function(coll) {
    assert.commandFailedWithCode(testDb.runCommand({
        aggregate: coll,
        pipeline: [
            {$search: {view: {name: addFieldsViewName, effectivePipeline: addFieldsViewPipeline}}}
        ],
        cursor: {}
    }),
                                 [injectionNotAllowedErrorCode]);
    assert.commandFailedWithCode(testDb.runCommand({
        aggregate: coll,
        pipeline: [{
            $vectorSearch:
                {view: {name: addFieldsViewName, effectivePipeline: addFieldsViewPipeline}}
        }],
        cursor: {}
    }),
                                 [injectionNotAllowedErrorCode]);
    assert.commandFailedWithCode(testDb.runCommand({
        aggregate: coll,
        pipeline: [{
            $searchMeta: {view: {name: addFieldsViewName, effectivePipeline: addFieldsViewPipeline}}
        }],
        cursor: {}
    }),
                                 [injectionNotAllowedErrorCode]);
};

injectingViewTest(playerColl.getName());
injectingViewTest(matchViewName);
injectingViewTest(addFieldsViewName);

// Make sure the server is still running.
assert.commandWorked(testDb.runCommand("ping"));