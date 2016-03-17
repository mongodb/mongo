load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'aggregate',

        load: function(writableCollection) {
            assert.doesNotThrow(() => {
                writableCollection.insertMany([
                    {
                      award: "Best Picture",
                      nominations: [
                          {title: "The Big Short"},
                          {title: "Bridge of Spies"},
                          {title: "Brooklyn"},
                          {title: "Max Max: Fury Road"},
                          {title: "The Martian"},
                          {title: "The Revenant"},
                          {title: "Room"},
                          {title: "Spotlight"}
                      ]
                    },
                    {
                      award: "Best Actor",
                      nominations: [
                          {title: "Trumbo", person: "Bryan Cranston"},
                          {title: "The Martian", person: "Matt Damon"},
                          {title: "The Revenant", person: "Leonardo DiCaprio"},
                          {title: "Steve Jobs", person: "Michael Fassbender"},
                          {title: "The Danish Girl", person: "Eddie Redmayne"}
                      ]
                    },
                    {
                      award: "Best Actress",
                      nominations: [
                          {title: "Carol", person: "Cate Blanchett"},
                          {title: "Room", person: "Brie Larson"},
                          {title: "Joy", person: "Jennifer Lawrence"},
                          {title: "45 Years", person: "Charlotte Rampling"},
                          {title: "Brooklyn", person: "Saoirse Ronan"}
                      ]
                    },
                    {
                      award: "Best Supporting Actor",
                      nominations: [
                          {title: "The Big Short", person: "Christian Bale"},
                          {title: "The Revenant", person: "Tom Hardy"},
                          {title: "Spotlight", person: "Mark Ruffalo"},
                          {title: "Bridge Of Spies", person: "Mark Rylance"},
                          {title: "Creed", person: "Sylvester Stallone"}
                      ]
                    },
                    {
                      award: "Best Supporting Actress",
                      nominations: [
                          {title: "The Hateful Eight", person: "Jennifer Jason Leigh"},
                          {title: "Carol", person: "Rooney Mara"},
                          {title: "Spotlight", person: "Rachel McAdams"},
                          {title: "The Danish Girl", person: "Alicia Vikander"},
                          {title: "Steve Jobs", person: "Kate Winslet"}
                      ]
                    }
                ]);
            });
        },
        exec: function(readableCollection) {

            // Find titles nominated for the most awards.
            var mostAwardsPipeline = [
                {$unwind: "$nominations"},
                {$group: {_id: "$nominations.title", count: {$sum: 1}}},
                {$sort: {count: -1, _id: 1}},
                {$limit: 2},
            ];

            assert.docEq(readableCollection.aggregate(mostAwardsPipeline).toArray(),
                         [{_id: "Spotlight", count: 3}, {_id: "The Revenant", count: 3}]);

            // Check that pipelines fail with allowDiskUse true. We use runCommand manually because
            // the helper has conflicting error handling logic.
            var allowDiskUseCmd = {
                aggregate: readableCollection.getName(),
                pipeline: [],
                allowDiskUse: true
            };

            assert.commandFailedWithCode(readableCollection.runCommand(allowDiskUseCmd),
                                         ErrorCodes.IllegalOperation,
                                         "'allowDiskUse' is not allowed in read-only mode");
        }
    };
}());
