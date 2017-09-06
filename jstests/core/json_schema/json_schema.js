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
    assert.writeOK(coll.insert({_id: 8, num: "string"}));
    assert.writeOK(coll.insert({_id: 9}));

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

    // Test that $jsonSchema fails to parse if the values for the maximum, maxLength, and
    // minlength keywords are not numbers.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maximum: "0"}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maximum: {}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maxLength: "0"}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {maxLength: {}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {minLength: "0"}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {minLength: {}}}}}).itcount();
    });

    // Test that the empty schema matches everything.
    assert.eq(10, coll.find({$jsonSchema: {}}).itcount());

    // Test that a schema just checking that the type of stored documents is "object" is legal and
    // matches everything.
    assert.eq(10, coll.find({$jsonSchema: {type: "object"}}).itcount());

    // Test that schemas whose top-level type is not object matches nothing.
    assert.eq(0, coll.find({$jsonSchema: {type: "string"}}).itcount());
    assert.eq(0, coll.find({$jsonSchema: {bsonType: "long"}}).itcount());
    assert.eq(0, coll.find({$jsonSchema: {bsonType: "objectId"}}).itcount());

    // Test that type:"number" only matches numbers, or documents where the field is missing.
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 9}],
              coll.find({$jsonSchema: {properties: {num: {type: "number"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction is enforced correctly.
    assert.eq([{_id: 1}, {_id: 3}, {_id: 5}, {_id: 9}],
              coll.find({$jsonSchema: {properties: {num: {type: "number", maximum: -1}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Repeat the test, but include an explicit top-level type:"object".
    assert.eq(
        [{_id: 1}, {_id: 3}, {_id: 5}, {_id: 9}],
        coll.find({$jsonSchema: {type: "object", properties: {num: {type: "number", maximum: -1}}}},
                  {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that type:"long" only matches longs, or documents where the field is missing.
    assert.eq([{_id: 4}, {_id: 5}, {_id: 9}],
              coll.find({$jsonSchema: {properties: {num: {bsonType: "long"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction is enforced correctly with type:"long".
    assert.eq(
        [{_id: 5}, {_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {bsonType: "long", maximum: 0}}}}, {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that maximum restriction without a numeric type specified only applies to numbers.
    assert.eq([{_id: 1}, {_id: 3}, {_id: 5}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}],
              coll.find({$jsonSchema: {properties: {num: {maximum: 0}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maximum restriction does nothing if a non-numeric type is also specified.
    assert.eq([{_id: 7}, {_id: 8}, {_id: 9}],
              coll.find({$jsonSchema: {properties: {num: {type: "string", maximum: 0}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that maxLength restriction doesn't return strings with length greater than maxLength.
    assert.eq(
        [{_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {type: "string", maxLength: 2}}}}, {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that maxLength restriction returns strings with length less than or equal to maxLength.
    assert.eq(
        [{_id: 7}, {_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {type: "string", maxLength: 3}}}}, {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that minLength restriction doesn't return strings with length less than minLength.
    assert.eq(
        [{_id: 8}, {_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {type: "string", minLength: 4}}}}, {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that minLength restriction returns strings with length greater than or equal to
    // minLength.
    assert.eq(
        [{_id: 7}, {_id: 8}, {_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {type: "string", minLength: 3}}}}, {_id: 1})
            .sort({_id: 1})
            .toArray());

    // Test that $jsonSchema fails to parse if the values for the pattern keyword is not a string.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {pattern: 0}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {num: {pattern: {}}}}}).itcount();
    });

    // Tests that the pattern keyword only returns strings that match the regex pattern.
    assert.eq(
        [{_id: 8}, {_id: 9}],
        coll.find({$jsonSchema: {properties: {num: {type: "string", pattern: "ing"}}}}, {_id: 1})
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

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, arr: 3}));
    assert.writeOK(coll.insert({_id: 1, arr: [1, "foo"]}));
    assert.writeOK(coll.insert({_id: 2, arr: [{a: 1}, {b: 2}]}));
    assert.writeOK(coll.insert({_id: 3, arr: []}));
    assert.writeOK(coll.insert({_id: 4, arr: {a: []}}));

    // Test that the type:"array" restriction works as expected.
    assert.eq([{_id: 1}, {_id: 2}, {_id: 3}],
              coll.find({$jsonSchema: {properties: {arr: {type: "array"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that type:"number" works correctly in the presence of arrays.
    assert.eq([{_id: 0}],
              coll.find({$jsonSchema: {properties: {arr: {type: "number"}}}}, {_id: 1})
                  .sort({_id: 1})
                  .toArray());

    // Test that the following keywords fail to parse although present in the spec:
    // - default
    // - definitions
    // - format
    // - id
    // - $ref
    // - $schema
    let res = coll.runCommand({find: coll.getName(), query: {$jsonSchema: {default: {_id: 0}}}});
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({
        find: coll.getName(),
        query: {$jsonSchema: {definitions: {numberField: {type: "number"}}}}
    });
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({find: coll.getName(), query: {$jsonSchema: {format: "email"}}});
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({find: coll.getName(), query: {$jsonSchema: {id: "someschema.json"}}});
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({
        find: coll.getName(),
        query: {$jsonSchema: {properties: {a: {$ref: "#/definitions/positiveInt"}}}}
    });
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({find: coll.getName(), query: {$jsonSchema: {$schema: "hyper-schema"}}});
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

    res = coll.runCommand({
        find: coll.getName(),
        query: {$jsonSchema: {$schema: "http://json-schema.org/draft-04/schema#"}}
    });
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
}());
