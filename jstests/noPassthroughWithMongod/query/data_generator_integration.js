/**
 * A simple test for the DataGenerator class shows how to use it to call
 * the data generator Python script located at src/mongo/db/query/benchmark/data_generator
 *
 * See src/mongo/db/query/benchmark/data_generator/README.md for more information.
 */

import {DataGenerator} from "jstests/libs/query/data_generator.js";

const dg = new DataGenerator({db: db, module: "specs.test", seed: 1});
try {
    const collName = "Test";
    const size = 10;

    dg.execute({spec: collName, size: size, indices: "test_index", analyze: true});

    assert.eq(db[collName].find({i: {$exists: true}}).count(), size);
    assert.eq(db[collName].getIndexes().length, 2); // _id and i_idx test index

    // Confirm that 'analyze' has run on all nested fields.
    assert.eq(db["system.statistics.Test"].find().count(), 4);
} finally {
    dg.cleanup();
}

const tg = new DataGenerator({db: db, module: "specs.test_types", seed: 1});
try {
    const collName = "TypesTest";

    // size: 2 to force various comparison functions to be used internally in the data_generator.
    tg.execute({spec: collName, size: 2});

    let dbMetadata;
    // Remove the 'export const' in the schema file so that we can eval() it
    const file = cat(tg.out + "/" + collName + ".schema").replace(/export const/, "");
    eval(file);
    const fields = dbMetadata.fields;

    assert.eq(fields.float_field.types.dbl.unique[0], 1.1);
    assert.eq(fields.int_field.types.int.unique[0], 1);
    assert.eq(fields.bson_decimal128_field.types.dec.unique[0], "1.1");

    assert.eq(fields.str_field.types.str.unique[0], "A");
    assert.eq(fields.bool_field.types.bool.unique[0], true);

    printjson(fields.datetime_datetime_field);
    assert.eq(fields.datetime_datetime_field.types.dt.unique[0], "2024-01-01T12:11:10+00:00");
    assert.eq(fields.bson_datetime_ms_field.types.dt_ms.unique[0], 1704111070);
    assert.eq(fields.bson_timestamp_field.types.ts.unique[0], 1704111070);

    // All array values are flattened in a single array of unique values
    assert.eq(fields.array_field.types.int.unique.length, 2);
    assert.eq(fields.array_field.types.int.unique[0], 1);

    // Nested objects are either dicts or nested_object, depending on how
    // they were initially defined in the data_generator specification.
    assert.eq(fields.dict_field.types.obj.unique[0].a, 1);
    assert.eq(fields.obj_field.nested_object.str_field.types.str.unique[0], "A");

    // Enums are stored as strings, with the enum's __repr__ value as the value.
    assert.eq(fields.enum_field.types.str.unique[0], "A");
    assert.eq(fields.int_enum_field.types.str.unique[0], 1);

    assert.eq(fields.missing_field.missing_count, 2);
    assert.eq(fields.null_field.null_count, 2);
} finally {
    tg.cleanup();
}
