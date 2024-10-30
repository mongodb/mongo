/**
 * Test that TextMatch plans with eof stages are cached and can be recovered from the SBE plan cache
 * after the change in SERVER-84536.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheClear.
 *   not_allowed_with_signed_security_token,
 *   # This test attempts to perform queries and introspect/manipulate the server's plan cache
 *   # entries. The former operation may be routed to a secondary in the replica set, whereas the
 *   # latter must be routed to the primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   requires_fcv_81,
 * ]
 */

import {getPlanStages, getQueryPlanner, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const isUsingSbePlanCache = checkSbeFullFeatureFlagEnabled(db);
if (!isUsingSbePlanCache) {
    jsTestLog("Skipping test because SBE is disabled");
    quit();
}

const coll = db.plan_cache_eof;
coll.drop();

// creation of collection documents
// content generated using wikipedia random article
assert.commandWorked(coll.insert({
    _id: 1,
    title: "Olivia Shakespear",
    text:
        "Olivia Shakespear (born Olivia Tucker; 17 March 1863 – 3 October 1938) was a British novelist, playwright, and patron of the arts. She wrote six books that are described as \"marriage problem\" novels. Her works sold poorly, sometimes only a few hundred copies. Her last novel, Uncle Hilary, is considered her best. She wrote two plays in collaboration with Florence Farr."
}));
assert.commandWorked(coll.insert({
    _id: 2,
    title: "Mahim Bora",
    text:
        "Mahim Bora (born 1926) is an Indian writer and educationist from Assam state. He was born at a tea estate of Sonitpur district. He is an M.A. in Assamese literature from Gauhati University and had been a teacher in the Nowgong College for most of his teaching career. He has now retired and lives at Nagaon. Bora spent a good part of his childhood in the culture-rich surroundings of rural Nagaon, where the river Kalong was the life-blood of a community. His impressionable mind was to capture a myriad memories of that childhood, later to find expression in his poems, short stories and novels with humour, irony and pathos woven into their texture. When this river was dammed up, its disturbing effect was on the entire community dependant on nature's bounty."
}));
assert.commandWorked(coll.insert({
    _id: 3,
    title: "A break away!",
    text:
        "A break away! is an 1891 painting by Australian artist Tom Roberts. The painting depicts a mob of thirsty sheep stampeding towards a dam. A drover on horseback is attempting to turn the mob before they drown or crush each other in their desire to drink. The painting, an \"icon of Australian art\", is part of a series of works by Roberts that \"captures what was an emerging spirit of national identity.\" Roberts painted the work at Corowa. The painting depicts a time of drought, with little grass and the soil kicked up as dust. The work itself is a reflection on the pioneering days of the pastoral industry, which were coming to an end by the 1890s."
}));

assert.commandWorked(coll.createIndex({"title": "text"}));

coll.getPlanCache().clear();

// Query with empty string and eof child node is cached.
coll.find({"$text": {"$search": ""}}).toArray();
let cache = coll.getPlanCache().list();
assert.eq(1, cache.length, cache);

let explain = coll.find({"$text": {"$search": ""}}).explain("executionStats");
let winningPlan = getQueryPlanner(explain).winningPlan;
assert.eq(true, winningPlan.isCached);
assert(planHasStage(db, explain, "EOF"));
let eofStages = getPlanStages(winningPlan, "EOF");
eofStages.forEach(stage => assert.eq(stage.type, "predicateEvalsToFalse"));

// Query with a space as its search string and eof child node is cached.
coll.find({"$text": {"$search": " "}}).toArray();
cache = coll.getPlanCache().list();
assert.eq(2, cache.length, cache);

explain = coll.find({"$text": {"$search": " "}}).explain("executionStats");
winningPlan = getQueryPlanner(explain).winningPlan;
assert.eq(true, winningPlan.isCached);
assert(planHasStage(db, explain, "EOF"));
eofStages = getPlanStages(winningPlan, "EOF");
eofStages.forEach(stage => assert.eq(stage.type, "predicateEvalsToFalse"));
