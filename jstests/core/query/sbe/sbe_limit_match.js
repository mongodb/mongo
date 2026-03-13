/**
 * Non-leading $match is not pushed down to SBE if there are no $group or $lookup stages.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    # Explain for the aggregate command cannot run within a multi-document transaction
 *    does_not_support_transactions,
 *    requires_fcv_83,
 *    requires_sbe,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestricted} from "jstests/libs/query/sbe_util.js";

if (!checkSbeRestricted(db)) {
    jsTest.log.info("Skipping test because SBE is not in restricted mode.");
    quit();
}

const coll = db.c;
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
const explain = coll.explain().aggregate([{$limit: 1}, {$match: {x: 20}}]);
assert.eq(getEngine(explain), "classic", explain);
