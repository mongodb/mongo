/**
 * Tests that an idhack upsert with a '{$eq: 1} filter reads the _id index for '1' rather than
 * '{$eq: 1}'. Uses the fast-check library for some lightweight randomization.
 *
 * @tags: [
 *   requires_non_retryable_writes,
 * ]
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
const coll = db.getCollection('test');
let fn = (val) => {
    coll.drop();
    for (let rhs of [val, {foo: val}, {eq: val}]) {
        assert.commandWorked(coll.insert({_id: rhs}));
        let res = coll.update({_id: {$eq: rhs}}, {$set: {a: 1}}, {upsert: true});
        assert(res.nMatched === 1);
        assert(res.nUpserted === 0);
        assert(res.nModified === 1);
    }
};
fc.assert(fc.property(fc.integer(), fn));
