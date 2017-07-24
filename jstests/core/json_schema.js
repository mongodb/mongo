(function() {
    "use strict";

    let coll = db.jstests_json_schema;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, num: 3}));
    assert.writeOK(coll.insert({_id: 1, num: -3}));
    assert.writeOK(coll.insert({_id: 2, num: NumberInt(2)}));
    assert.writeOK(coll.insert({_id: 3, num: NumberInt(-2)}));
    assert.writeOK(coll.insert({_id: 4, num: NumberLong(1)}));
    assert.writeOK(coll.insert({_id: 5, num: NumberLong(-1)}));
    assert.writeOK(coll.insert({_id: 6, num: {}}));
    assert.writeOK(coll.insert({_id: 7, num: "str"}));
    assert.writeOK(coll.insert({_id: 8}));

    // Test that $jsonSchema fails to parse if its argument is not an object.
    assert.throws(function() {
        coll.find({$jsonSchema: "foo"}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: []}).itcount();
    });

    // Test that $jsonSchema fails to parse if the value for the "type" keyword is not a string.
    assert.throws(function() {
        coll.find({$jsonSchema: {type: 3}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {type: {}}}).itcount();
    });

    // Test that $jsonSchema fails to parse if the value for the properties keyword is not an
    // object.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: 3}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: []}}).itcount();
    });

    // Test that $jsonSchema fails to parse if one of the properties named inside the argument for
    // the properties keyword is not an object.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: "number"}}}).itcount();
    });

    // Test that $jsonSchema fails to parse if the value for the maximum keyword is not a number.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maximum: "0"}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maximum: {}}}}}).itcount();
    });

    // Test that the empty schema matches everything.
    assert.eq(9, coll.find({$jsonSchema: {}}).itcount());

    // Test that a schema just checking that the type of stored documents is "object" is legal and
    // matches everything.
    assert.eq(9, coll.find({$jsonSchema: {type: "object"}}).itcount());

    // Test that schemas whose top-level type is not object matches nothing.
    assert.eq(0, coll.find({$jsonSchema: {type: "string"}}).itcount());
    assert.eq(0, coll.find({$jsonSchema: {type: "long"}}).itcount());
    assert.eq(0, coll.find({$jsonSchema: {type: "objectId"}}).itcount());

    // Test that type:"number" only matches numbers, or documents where the field is missing.
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {type: "number"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction is enforced correctly.
    assert.eq([{_id: 1}, {_id: 3}, {_id: 5}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {type: "number", maximum: -1}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Repeat the test, but include an explicit top-level type:"object".
    assert.eq(
        [{_id: 1}, {_id: 3}, {_id: 5}, {_id: 8}],
        coll.find({$jsonSchema: {type: "object", properties: {num: {type: "number", maximum: -1}}}},
                  {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that type:"long" only matches longs, or documents where the field is missing.
    assert.eq([{_id: 4}, {_id: 5}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {type: "long"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction is enforced correctly with type:"long".
    assert.eq([{_id: 5}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {type: "long", maximum: 0}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction without a numeric type specified only applies to numbers.
    assert.eq([{_id: 1}, {_id: 3}, {_id: 5}, {_id: 6}, {_id: 7}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {maximum: 0}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction does nothing if a non-numeric type is also specified.
    assert.eq([{_id: 7}, {_id: 8}],
              coll.find({$jsonSchema: {properties: {num: {type: "string", maximum: 0}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, obj: 3}));
    assert.writeOK(coll.insert({_id: 1, obj: {f1: {f3: "str"}, f2: "str"}}));
    assert.writeOK(coll.insert({_id: 2, obj: {f1: "str", f2: "str"}}));
    assert.writeOK(coll.insert({_id: 3, obj: {f1: 1, f2: "str"}}));

    // Test that properties keyword can be used recursively, and that it does not apply when the
    // field does not contain on object.
    assert.eq([{_id: 0}, {_id: 1}],
              coll.find({
                      $jsonSchema: {
                          properties: {
                              obj: {
                                  properties: {
                                      f1: {type: "object", properties: {f3: {type: "string"}}},
                                      f2: {type: "string"}
                                  }
                              }
                          }
                      }
                  },
                        {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that $jsonSchema can be combined with other operators in the match language.
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}],
        coll.find({
                $or: [
                    {"obj.f1": "str"},
                    {
                      $jsonSchema: {
                          properties: {
                              obj: {
                                  properties: {
                                      f1: {type: "object", properties: {f3: {type: "string"}}},
                                      f2: {type: "string"}
                                  }
                              }
                          }
                      }
                    }
                ]
            },
                  {_id: 1})
            .sort({_id: 1})
            .toArray());
}());
