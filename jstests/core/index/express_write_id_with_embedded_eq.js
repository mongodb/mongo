/**
 * Tests that writes with an object-valued _id filter containing a non-leading '$eq' field (e.g.
 * {_id: {a: 1, $eq: 2}}) are treated as a literal object equality, matching find semantics, rather
 * than being unwrapped to {_id: 2} on the express/IDHACK write paths.
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_91,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];
const docs = [{_id: 1}, {_id: 2}];

describe("object-valued _id filter with embedded $eq", function () {
    beforeEach(function () {
        coll.drop();
        assert.commandWorked(coll.insert(docs));
    });

    afterEach(function () {
        assert.eq(coll.find().sort({_id: 1}).toArray(), docs);
        coll.drop();
    });

    it("find matches nothing", function () {
        assert.eq(coll.find({_id: {a: 1, $eq: 1}}).itcount(), 0);
        assert.eq(coll.find({_id: {a: 1, $eq: 2}}).itcount(), 0);
    });

    it("updateOne matches nothing", function () {
        const res = assert.commandWorked(coll.updateOne({_id: {a: 1, $eq: 1}}, {$set: {x: 1}}));
        assert.eq(res.matchedCount, 0, "unexpected match", {res});
    });

    it("deleteOne deletes nothing", function () {
        const res = assert.commandWorked(coll.deleteOne({_id: {a: 1, $eq: 2}}));
        assert.eq(res.deletedCount, 0, "unexpected delete", {res});
    });

    it("findAndModify update matches nothing", function () {
        assert.eq(coll.findAndModify({query: {_id: {a: 1, $eq: 1}}, update: {$set: {x: 1}}}), null);
    });

    it("findAndModify remove matches nothing", function () {
        assert.eq(coll.findAndModify({query: {_id: {a: 1, $eq: 2}}, remove: true}), null);
    });

    it("bulkWrite matches nothing", function () {
        const res = assert.commandWorked(
            db.adminCommand({
                bulkWrite: 1,
                ops: [
                    {update: 0, filter: {_id: {a: 1, $eq: 1}}, updateMods: {$set: {x: 1}}},
                    {delete: 0, filter: {_id: {a: 1, $eq: 2}}},
                ],
                nsInfo: [{ns: coll.getFullName()}],
            }),
        );
        assert.eq(res.nErrors, 0, "unexpected bulkWrite error", {res});
        assert.eq(res.nMatched, 0, "unexpected match", {res});
        assert.eq(res.nDeleted, 0, "unexpected delete", {res});
    });

    it("upsert does not insert the unwrapped _id", function () {
        assert.throwsWithCode(
            () => coll.updateOne({_id: {a: 1, $eq: 3}}, {$set: {x: 1}}, {upsert: true}),
            ErrorCodes.DollarPrefixedFieldName,
        );
    });
});
