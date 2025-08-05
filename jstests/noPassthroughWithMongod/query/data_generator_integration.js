/**
 * A simple test for the DataGenerator class shows how to use it to call
 * the data generator Python script located at src/mongo/db/query/benchmark/data_generator
 *
 * See src/mongo/db/query/benchmark/data_generator/README.md for more information.
 */

import {DataGenerator} from "jstests/libs/query/data_generator.js";

const dg = new DataGenerator({db: db, module: 'specs.test', seed: 1});
try {
    const collName = 'Test';
    const size = 10;

    dg.execute({spec: collName, size: size, indices: 'test_index'});

    assert.eq(db[collName].find({i: {$exists: true}}).count(), size);
    assert.eq(db[collName].getIndexes().length, 2);  // _id and i_idx test index
} finally {
    dg.cleanup();
}
