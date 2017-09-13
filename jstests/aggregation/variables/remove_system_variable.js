/**
 * Tests for the $$REMOVE system variable.
 */
(function() {
    "use strict";

    let coll = db[jsTest.name()];
    coll.drop();

    assert.writeOK(coll.insert({_id: 1, a: 2, b: 3}));
    assert.writeOK(coll.insert({_id: 2, a: 3, b: 4}));
    assert.writeOK(coll.insert({_id: 3, a: {b: 98, c: 99}}));

    let projectStage = {
        $project: {_id: 0, a: 1, b: {$cond: {if: {$eq: ["$b", 4]}, then: "$$REMOVE", else: "$b"}}}
    };

    // Test that we can conditionally remove a field in $project.
    assert.eq([{a: 2, b: 3}], coll.aggregate([{$match: {_id: 1}}, projectStage]).toArray());
    assert.eq([{a: 3}], coll.aggregate([{$match: {_id: 2}}, projectStage]).toArray());

    // Test removal of a nested field, using $project.
    assert.eq([{a: {b: 98}}],
              coll.aggregate([{$match: {_id: 3}}, {$project: {_id: 0, "a.b": 1}}]).toArray());
    assert.eq(
        [{a: {}}],
        coll.aggregate([{$match: {_id: 3}}, {$project: {_id: 0, "a.b": "$$REMOVE"}}]).toArray());
    assert.eq(
        [{a: {}}],
        coll.aggregate([{$match: {_id: 3}}, {$project: {_id: 0, a: {b: "$$REMOVE"}}}]).toArray());

    // Test removal of a nested field, using $addFields.
    assert.eq([{_id: 3, a: {c: 99}}],
              coll.aggregate([{$match: {_id: 3}}, {$addFields: {"a.b": "$$REMOVE"}}]).toArray());

    // Test that any field path following "$$REMOVE" also evaluates to missing.
    assert.eq([{_id: 3}],
              coll.aggregate([{$match: {_id: 3}}, {$addFields: {"a": "$$REMOVE.a.c"}}]).toArray());

    // Test that $$REMOVE can be used together with user-defined variables in a $let.
    assert.eq([{a: {b: 3, d: 4}}],
              coll.aggregate([
                      {$match: {_id: 3}},
                      {
                        $project: {
                            _id: 0,
                            a: {
                                $let: {
                                    vars: {bar: 3, foo: 4},
                                    in : {b: "$$bar", c: "$$REMOVE", d: "$$foo"}
                                }
                            }
                        }
                      }
                  ])
                  .toArray());

    // Test that $$REMOVE cannot be assigned in a $let.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {$match: {_id: 3}},
            {$project: {_id: 0, a: {$let: {vars: {"REMOVE": 3}, in : {b: "$$REMOVE", c: 2}}}}}
        ]
    }),
                                 16867);

    // Test that $$REMOVE, $$CURRENT, $$ROOT, and user-defined variables can all be used together.
    assert.eq(
        [{a: {b: 3, d: {_id: 1, a: 2, b: 3}, e: {_id: 1, a: 2, b: 3}}}],
        coll.aggregate([
                {$match: {_id: 1}},
                {
                  $project: {
                      _id: 0,
                      a: {
                          $let: {
                              vars: {myVar: 3},
                              in : {b: "$$myVar", c: "$$REMOVE", d: "$$ROOT", e: "$$CURRENT"}
                          }
                      }
                  }
                }
            ])
            .toArray());
}());
