// Test that the $group stage and all accumulators respect the collation.
(function() {
    "use strict";

    var coll = db.collation_group;
    coll.drop();

    var results;
    var caseInsensitive = {collation: {locale: "en_US", strength: 2}};
    var diacriticInsensitive = {collation: {locale: "en_US", strength: 1, caseLevel: true}};
    var numericOrdering = {collation: {locale: "en_US", numericOrdering: true}};
    var caseAndDiacriticInsensitive = {collation: {locale: "en_US", strength: 1}};

    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));

    assert.writeOK(coll.insert({_id: 0, str: "A", str2: "á"}));
    assert.writeOK(coll.insert({_id: 1, str: "a", str2: "a"}));
    assert.writeOK(coll.insert({_id: 2, str: "B", str2: "é"}));
    assert.writeOK(coll.insert({_id: 3, str: "b", str2: "e"}));

    // Ensure that equality of groups respects the collation inherited from the collection default.
    assert.eq(2, coll.aggregate([{$group: {_id: "$str"}}]).itcount());

    // Ensure that equality of groups respects an explicit collation.
    assert.eq(2, coll.aggregate([{$group: {_id: "$str2"}}], diacriticInsensitive).itcount());

    // Ensure that equality of groups created by $sortByCount respects the inherited collation.
    assert.eq(2, coll.aggregate([{$sortByCount: "$str"}]).itcount());
    assert.eq(4, coll.aggregate([{$sortByCount: "$str2"}]).itcount());

    // Ensure that equality of groups created by $sortByCount respects an explicit collation.
    assert.eq(4, coll.aggregate([{$sortByCount: "$str"}], diacriticInsensitive).itcount());
    assert.eq(2, coll.aggregate([{$sortByCount: "$str2"}], diacriticInsensitive).itcount());

    // Ensure that equality of groups inside $facet stage respects the inherited collation.
    results =
        coll.aggregate([{
                $facet:
                    {facetStr: [{$group: {_id: "$str"}}], facetStr2: [{$group: {_id: "$str2"}}]}
            }])
            .toArray();
    assert.eq(1, results.length);
    assert.eq(2, results[0].facetStr.length);
    assert.eq(4, results[0].facetStr2.length);

    // Test that the $addToSet accumulator respects the inherited collation.
    results = coll.aggregate([{$group: {_id: null, set: {$addToSet: "$str"}}}]).toArray();
    assert.eq(1, results.length);
    assert.eq(2, results[0].set.length);

    // Test that the $addToSet accumulator respects an explicit collation.
    results =
        coll.aggregate([{$group: {_id: null, set: {$addToSet: "$str2"}}}], diacriticInsensitive)
            .toArray();
    assert.eq(1, results.length);
    assert.eq(2, results[0].set.length);

    // Ensure that a subexpression inside $push respects the collation.
    results = coll.aggregate(
                      [
                        {$match: {_id: 0}},
                        {$group: {_id: null, areEqual: {$push: {$eq: ["$str", "$str2"]}}}}
                      ],
                      caseAndDiacriticInsensitive)
                  .toArray();
    assert.eq(1, results.length);
    assert.eq(1, results[0].areEqual.length);
    assert.eq(true, results[0].areEqual[0]);

    // Test that the $min and $max accumulators respect the inherited collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    assert.writeOK(coll.insert({num: "100"}));
    assert.writeOK(coll.insert({num: "2"}));
    results = coll.aggregate([{$group: {_id: null, min: {$min: "$num"}}}]).toArray();
    assert.eq(1, results.length);
    assert.eq("2", results[0].min);
    results = coll.aggregate([{$group: {_id: null, max: {$max: "$num"}}}]).toArray();
    assert.eq(1, results.length);
    assert.eq("100", results[0].max);
})();
