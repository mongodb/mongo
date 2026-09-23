// @tags: [assumes_no_implicit_collection_creation_after_drop]

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$bucket literal values", function () {
    const coll = db.bucket_literal_values;

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {x: 5, color: "red"},
                {x: 10, color: "blue"},
            ]),
        );
    });

    after(function () {
        coll.drop();
    });

    it("treats default {$literal: '$color'} as the literal string", function () {
        const results = coll
            .aggregate([{$bucket: {groupBy: "$x", boundaries: [0, 1], default: {$literal: "$color"}}}])
            .toArray();
        assert.eq(1, results.length, {results});
        assert.eq({_id: "$color", count: 2}, results[0], {results});
    });

    it("treats default {$literal: '$$ROOT'} as the literal string", function () {
        const results = coll
            .aggregate([{$bucket: {groupBy: "$x", boundaries: [0, 1], default: {$literal: "$$ROOT"}}}])
            .toArray();
        assert.eq(1, results.length, {results});
        assert.eq({_id: "$$ROOT", count: 2}, results[0], {results});
    });

    it("treats boundary literal strings as literals", function () {
        const boundaryColl = db.bucket_literal_values_boundaries;
        boundaryColl.drop();
        assert.commandWorked(
            boundaryColl.insert([
                {x: "$m", l: "$j", n: "$o"},
                {x: "$m", l: "$i", n: "$p"},
            ]),
        );

        const results = boundaryColl
            .aggregate([
                {
                    $bucket: {
                        groupBy: "$x",
                        boundaries: [{$literal: "$l"}, {$literal: "$n"}],
                        default: "other",
                    },
                },
            ])
            .toArray();
        assert.eq(1, results.length, {results});
        assert.eq({_id: "$l", count: 2}, results[0], {results});

        boundaryColl.drop();
    });
});
