// Test that the $sort stage respects the collation.
(function() {
    "use strict";

    // In French, words are sometimes ordered on the secondary level (a.k.a. at the level of
    // diacritical marks) by the *last* accent difference rather than the first. This is specified
    // by the {backwards: true} option.
    //
    // For example, côte < coté, since the last accent difference is "e" < "é". Without the reverse
    // accent weighting turned on, these two words would sort in the opposite order, since "ô" >
    // "o".
    var frenchAccentOrdering = {collation: {locale: "fr", backwards: true}};

    var coll = db.collation_sort;
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, word1: "pêche", word2: "côté"}));
    assert.writeOK(coll.insert({_id: 2, word1: "pêche", word2: "coté"}));
    assert.writeOK(coll.insert({_id: 3, word1: "pêche", word2: "côte"}));
    assert.writeOK(coll.insert({_id: 4, word1: "pèché", word2: "côté"}));
    assert.writeOK(coll.insert({_id: 5, word1: "pèché", word2: "coté"}));
    assert.writeOK(coll.insert({_id: 6, word1: "pèché", word2: "côte"}));
    assert.writeOK(coll.insert({_id: 7, word1: "pêché", word2: "côté"}));
    assert.writeOK(coll.insert({_id: 8, word1: "pêché", word2: "coté"}));
    assert.writeOK(coll.insert({_id: 9, word1: "pêché", word2: "côte"}));

    // Test that ascending sort respects the collation.
    assert.eq([{_id: "pèché"}, {_id: "pêche"}, {_id: "pêché"}],
              coll.aggregate([{$group: {_id: "$word1"}}, {$sort: {_id: 1}}]).toArray());
    assert.eq([{_id: "pêche"}, {_id: "pèché"}, {_id: "pêché"}],
              coll.aggregate([{$group: {_id: "$word1"}}, {$sort: {_id: 1}}], frenchAccentOrdering)
                  .toArray());

    // Test that descending sort respects the collation.
    assert.eq([{_id: "pêché"}, {_id: "pêche"}, {_id: "pèché"}],
              coll.aggregate([{$group: {_id: "$word1"}}, {$sort: {_id: -1}}]).toArray());
    assert.eq([{_id: "pêché"}, {_id: "pèché"}, {_id: "pêche"}],
              coll.aggregate([{$group: {_id: "$word1"}}, {$sort: {_id: -1}}], frenchAccentOrdering)
                  .toArray());

    // Test that compound, mixed ascending/descending sort respects the collation.
    assert.eq([4, 6, 5, 1, 3, 2, 7, 9, 8],
              coll.aggregate([
                      {$sort: {word1: 1, word2: -1}},
                      {$project: {_id: 1}},
                      {$group: {_id: null, out: {$push: "$_id"}}}
                  ])
                  .toArray()[0]
                  .out);
    assert.eq([1, 2, 3, 4, 5, 6, 7, 8, 9],
              coll.aggregate(
                      [
                        {$sort: {word1: 1, word2: -1}},
                        {$project: {_id: 1}},
                        {$group: {_id: null, out: {$push: "$_id"}}}
                      ],
                      frenchAccentOrdering)
                  .toArray()[0]
                  .out);

    // Test that compound, mixed descending/ascending sort respects the collation.
    assert.eq([8, 9, 7, 2, 3, 1, 5, 6, 4],
              coll.aggregate([
                      {$sort: {word1: -1, word2: 1}},
                      {$project: {_id: 1}},
                      {$group: {_id: null, out: {$push: "$_id"}}}
                  ])
                  .toArray()[0]
                  .out);
    assert.eq([9, 8, 7, 6, 5, 4, 3, 2, 1],
              coll.aggregate(
                      [
                        {$sort: {word1: -1, word2: 1}},
                        {$project: {_id: 1}},
                        {$group: {_id: null, out: {$push: "$_id"}}}
                      ],
                      frenchAccentOrdering)
                  .toArray()[0]
                  .out);

    // Test that sort inside a $facet respects the collation.
    const results = coll.aggregate([{
                                      $facet: {
                                          fct: [
                                              {$sort: {word1: -1, word2: 1}},
                                              {$project: {_id: 1}},
                                              {$group: {_id: null, out: {$push: "$_id"}}}
                                          ]
                                      }
                                   }],
                                   frenchAccentOrdering)
                        .toArray();
    assert.eq(1, results.length);
    assert.eq(1, results[0].fct.length);
    assert.eq([9, 8, 7, 6, 5, 4, 3, 2, 1], results[0].fct[0].out);
})();
