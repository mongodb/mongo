import {runReadOnlyTest} from "jstests/readonly/lib/read_only_test.js";

runReadOnlyTest(
    (function () {
        return {
            name: "get_more",
            count: 3000,
            load: function (writableCollection) {
                // Insert enough data that we need multiple batches to get it.
                let n = this.count;
                let bulkSize = 500;

                for (let i = 0; i < n / bulkSize; ++i) {
                    let bulk = writableCollection.initializeUnorderedBulkOp();
                    for (let j = 0; j < bulkSize; ++j) {
                        let idx = i * bulkSize + j;
                        bulk.insert({x: idx, y: idx + 1});
                    }
                    assert.commandWorked(bulk.execute());
                }
                assert.eq(writableCollection.count(), this.count);
            },
            exec: function (readableCollection) {
                let cursor = readableCollection.find();
                let count = 0;
                while (cursor.hasNext()) {
                    ++count;
                    let doc = cursor.next();
                    assert.eq(doc.y, doc.x + 1);
                }
                assert.eq(count, this.count);
            },
        };
    })(),
);
