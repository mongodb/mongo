/**
 * Test the behavior of field names containing special characters, including:
 * - dots
 * - $-prefixed
 * - reserved $-prefixed ($ref, $id, $db)
 * - regex
 *
 * contained in a top-level element, embedded element, and within _id.
 *
 * @tags: [assumes_unsharded_collection]
 */
(function() {
    "use strict";

    const coll = db.field_name_validation;
    coll.drop();

    //
    // Insert command field name validation.
    //

    // Test that dotted field names are allowed.
    assert.writeOK(coll.insert({"a.b": 1}));
    assert.writeOK(coll.insert({"_id.a": 1}));
    assert.writeOK(coll.insert({a: {"a.b": 1}}));
    assert.writeOK(coll.insert({_id: {"a.b": 1}}));

    // Test that _id cannot be a regex.
    assert.writeError(coll.insert({_id: /a/}));

    // Test that _id cannot be an array.
    assert.writeError(coll.insert({_id: [9]}));

    // Test that $-prefixed field names are allowed in embedded objects.
    assert.writeOK(coll.insert({a: {$b: 1}}));
    assert.eq(1, coll.find({"a.$b": 1}).itcount());

    // Test that $-prefixed field names are not allowed at the top level.
    assert.writeErrorWithCode(coll.insert({$a: 1}), ErrorCodes.BadValue);
    assert.writeErrorWithCode(coll.insert({valid: 1, $a: 1}), ErrorCodes.BadValue);

    // Test that reserved $-prefixed field names are also not allowed.
    assert.writeErrorWithCode(coll.insert({$ref: 1}), ErrorCodes.BadValue);
    assert.writeErrorWithCode(coll.insert({$id: 1}), ErrorCodes.BadValue);
    assert.writeErrorWithCode(coll.insert({$db: 1}), ErrorCodes.BadValue);

    // Test that _id cannot be an object with an element that has a $-prefixed field name.
    assert.writeErrorWithCode(coll.insert({_id: {$b: 1}}), ErrorCodes.DollarPrefixedFieldName);
    assert.writeErrorWithCode(coll.insert({_id: {a: 1, $b: 1}}),
                              ErrorCodes.DollarPrefixedFieldName);

    // Should not enforce the same restrictions on an embedded _id field.
    assert.writeOK(coll.insert({a: {_id: [9]}}));
    assert.writeOK(coll.insert({a: {_id: /a/}}));
    assert.writeOK(coll.insert({a: {_id: {$b: 1}}}));

    //
    // Update command field name validation.
    //
    coll.drop();

    // Dotted fields are allowed in an update.
    assert.writeOK(coll.update({}, {"a.b": 1}, {upsert: true}));
    assert.eq(0, coll.find({"a.b": 1}).itcount());
    assert.eq(1, coll.find({}).itcount());

    // Dotted fields represent paths in $set.
    assert.writeOK(coll.update({}, {$set: {"a.b": 1}}, {upsert: true}));
    assert.eq(1, coll.find({"a.b": 1}).itcount());

    // Dotted fields represent paths in the query object.
    assert.writeOK(coll.update({"a.b": 1}, {$set: {"a.b": 2}}));
    assert.eq(1, coll.find({"a.b": 2}).itcount());
    assert.eq(1, coll.find({a: {b: 2}}).itcount());

    assert.writeOK(coll.update({"a.b": 2}, {"a.b": 3}));
    assert.eq(0, coll.find({"a.b": 3}).itcount());

    // $-prefixed field names are not allowed.
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$c: 1}, {upsert: true}),
                              ErrorCodes.FailedToParse);
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$set: {$c: 1}}, {upsert: true}),
                              ErrorCodes.DollarPrefixedFieldName);
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$set: {c: {$d: 1}}}, {upsert: true}),
                              ErrorCodes.DollarPrefixedFieldName);

    // Reserved $-prefixed field names are also not allowed.
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$ref: 1}), ErrorCodes.FailedToParse);
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$id: 1}), ErrorCodes.FailedToParse);
    assert.writeErrorWithCode(coll.update({"a.b": 1}, {$db: 1}), ErrorCodes.FailedToParse);

    //
    // FindAndModify field name validation.
    //
    coll.drop();

    // Dotted fields are allowed in update object.
    coll.findAndModify({query: {_id: 0}, update: {_id: 0, "a.b": 1}, upsert: true});
    assert.eq([{_id: 0, "a.b": 1}], coll.find({_id: 0}).toArray());

    // Dotted fields represent paths in $set.
    coll.findAndModify({query: {_id: 1}, update: {$set: {_id: 1, "a.b": 1}}, upsert: true});
    assert.eq([{_id: 1, a: {b: 1}}], coll.find({_id: 1}).toArray());

    // Dotted fields represent paths in the query object.
    coll.findAndModify({query: {_id: 0, "a.b": 1}, update: {"a.b": 2}});
    assert.eq([{_id: 0, "a.b": 1}], coll.find({_id: 0}).toArray());

    coll.findAndModify({query: {_id: 1, "a.b": 1}, update: {$set: {_id: 1, "a.b": 2}}});
    assert.eq([{_id: 1, a: {b: 2}}], coll.find({_id: 1}).toArray());

    // $-prefixed field names are not allowed.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 1}, update: {_id: 1, $invalid: 1}});
    });
    assert.throws(function() {
        coll.findAndModify({query: {_id: 1}, update: {$set: {_id: 1, $invalid: 1}}});
    });

    // Reserved $-prefixed field names are also not allowed.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 1}, update: {_id: 1, $ref: 1}});
    });
    assert.throws(function() {
        coll.findAndModify({query: {_id: 1}, update: {_id: 1, $id: 1}});
    });
    assert.throws(function() {
        coll.findAndModify({query: {_id: 1}, update: {_id: 1, $db: 1}});
    });

    //
    // Aggregation field name validation.
    //
    coll.drop();

    assert.writeOK(coll.insert({_id: {a: 1, b: 2}, "c.d": 3}));

    // Dotted fields represent paths in an aggregation pipeline.
    assert.eq(coll.aggregate([{$match: {"_id.a": 1}}, {$project: {"_id.b": 1}}]).toArray(),
              [{_id: {b: 2}}]);
    assert.eq(coll.aggregate([{$match: {"c.d": 3}}, {$project: {"_id.b": 1}}]).toArray(), []);

    assert.eq(coll.aggregate([{$project: {"_id.a": 1}}]).toArray(), [{_id: {a: 1}}]);
    assert.eq(coll.aggregate([{$project: {"c.d": 1, _id: 0}}]).toArray(), [{}]);

    assert.eq(coll.aggregate([
                      {$addFields: {"new.field": {$multiply: ["$c.d", "$_id.a"]}}},
                      {$project: {"new.field": 1, _id: 0}}
                  ])
                  .toArray(),
              [{new: {field: null}}]);

    assert.eq(coll.aggregate([{$group: {_id: "$_id.a", e: {$sum: "$_id.b"}}}]).toArray(),
              [{_id: 1, e: 2}]);
    assert.eq(coll.aggregate([{$group: {_id: "$_id.a", e: {$sum: "$c.d"}}}]).toArray(),
              [{_id: 1, e: 0}]);

    // Accumulation statements cannot have a dotted field name.
    assert.commandFailed(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$group: {_id: "$_id.a", "e.f": {$sum: "$_id.b"}}}]
    }));

    // $-prefixed field names are not allowed in an aggregation pipeline.
    assert.commandFailed(
        db.runCommand({aggregate: coll.getName(), pipeline: [{$match: {"$invalid": 1}}]}));

    assert.commandFailed(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$project: {"_id.a": 1, "$newField": {$multiply: ["$_id.b", "$_id.a"]}}}]
    }));

    assert.commandFailed(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$addFields: {"_id.a": 1, "$newField": {$multiply: ["$_id.b", "$_id.a"]}}}]
    }));

    assert.commandFailed(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$group: {_id: "$_id.a", "$invalid": {$sum: "$_id.b"}}}]
    }));
})();
