/**
 * A simple test for the DataGenerator class shows how to use it to call
 * the data generator Python script located at src/mongo/db/query/benchmark/data_generator
 *
 * See src/mongo/db/query/benchmark/data_generator/README.md for more information.
 *
 * @tags: [
 *   # This depends on numpy which isn't currently supported on these platforms.
 *   incompatible_ppc,
 *   incompatible_s390x,
 * ]
 */

import {DataGenerator} from "jstests/libs/query/data_generator.js";

// Ensures that the generated collection does not consist of entirely of copies of the same document.
function checkDifferentDocuments(collName) {
    const documents = db[collName].find({}, {"_id": 0}).toArray();
    assert.gt(documents.length, 1);
    assert(documents.slice(1).some((doc) => doc !== documents[0]));
}

const dg = new DataGenerator({db: db, module: "specs.test", seed: 1});
try {
    const collName = "Test";
    const size = 10;

    dg.execute({spec: collName, size: size, indexes: "test_index", analyze: true});

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

// Data is uncorrelated if only one field has the correlation function
const ucg = new DataGenerator({db: db, module: "specs.test_correlations"});
try {
    const collName = "Uncorrelated";
    const size = 40;

    ucg.execute({spec: collName, size: size});

    checkDifferentDocuments(collName);

    const data = db[collName].find({}).toArray();

    assert(
        data.some((item) => item.field1 !== item.field2),
        data,
    );
} finally {
    ucg.cleanup();
}

// The part of the distribution function outside of the correlation function is uncorrelated
const ucfg = new DataGenerator({db: db, module: "specs.test_correlations", seed: 2});
try {
    const collName = "PartialCorrelation";
    const size = 40;

    ucfg.execute({spec: collName, size: size});

    checkDifferentDocuments(collName);

    // field2 has four potential values: true, false, 1, 2. If field2's value is a boolean, it is
    // correlated with field1, and thus if field1 has value x, field2 will always have value y,
    // which is either true or false.
    const true_with_bool = db[collName]
        .find({"field1": {"$eq": true}, "field2": {"$type": "bool"}}, {"_id": 0})
        .toArray();
    assert(true_with_bool.every((item) => item.field2) || true_with_bool.every((item) => !item.field2), true_with_bool);

    const false_with_bool = db[collName].find({"field1": {"$eq": false}, "field2": {"$type": "bool"}}).toArray();
    assert(
        false_with_bool.every((item) => item.field2) || false_with_bool.every((item) => !item.field2),
        false_with_bool,
    );

    // If the field2's value is an int, it is not correlated with field1, and thus among documents were
    // field1 has value x, 1 and 2 are both expected to be present in field2.

    const true_with_int = db[collName].find({"field1": {"$eq": true}, "field2": {"$type": "int"}}).toArray();
    const false_with_int = db[collName].find({"field1": {"$eq": false}, "field2": {"$type": "int"}}).toArray();

    assert(
        true_with_int.some((item) => item.field2 === 1),
        true_with_int,
    );
    assert(
        true_with_int.some((item) => item.field2 === 2),
        true_with_int,
    );
    assert(
        false_with_int.some((item) => item.field2 === 1),
        false_with_int,
    );
    assert(
        false_with_int.some((item) => item.field2 === 2),
        false_with_int,
    );
} finally {
    ucfg.cleanup();
}

// Fields are only correlated to fields that call the correlation function with the same key
const tcg = new DataGenerator({db: db, module: "specs.test_correlations"});
try {
    const collName = "TwoCorrelations";
    const size = 40;

    tcg.execute({spec: collName, size: size});

    checkDifferentDocuments(collName);

    // True values in gXfield2 correlate to lower values of gXfield1.
    // False values in gXfield2 should still exist if gYfield2 has a lower value.
    // Omit documents where the gXfield1 is on the boundary between when true values become false
    // because the corresponding value of gXfield2 is arbitrary.
    const data = db[collName].find({"g1field1": {"$ne": 5}, "g2field1": {"$ne": 6}}, {"_id": 0}).toArray();

    assert(
        data.every((item) => (item.g1field1 < 5 ? item.g1field2 === true : item.g1field2 === false)),
        data,
    );

    assert(
        data.every((item) => (item.g2field1 < 6 ? item.g2field2 === true : item.g2field2 === false)),
        data,
    );

    assert(
        data.some((item) => (item.g2field1 < 6 ? item.g1field2 === false : item.g1field2 === true)),
        data,
    );

    assert(
        data.some((item) => (item.g1field1 < 5 ? item.g2field2 === false : item.g2field2 === true)),
        data,
    );
} finally {
    tcg.cleanup();
}

// Fields are only correlated to fields that call the correlation function with the same key
const cnpg = new DataGenerator({db: db, module: "specs.test_correlations"});
try {
    const collName = "CorrelationNotSupportedForNumpy";
    const size = 40;

    assert.throws(() => tcg.execute({spec: collName, size: size}));
} finally {
    cnpg.cleanup();
}

// All external RNGs use the same seed, regardless of correlation state.
const sg = new DataGenerator({db: db, module: "specs.test_seed", seed: 1});
try {
    const collName = "Seed";
    const size = 40;

    sg.execute({spec: collName, size: size});
    checkDifferentDocuments(collName);
    const data = db[collName].find({}, {"_id": 0}).toArray();

    sg.execute({spec: collName, size: size});
    checkDifferentDocuments(collName);
    const data2 = db[collName].find({}, {"_id": 0}).toArray();

    for (let i = 0; i < size; i++) {
        assert.eq(data[i], data2[i]);
    }
} finally {
    sg.cleanup();
}

// Ensure that all example specs run.
const exampleSpecs = [
    {"module": "specs.calibrations", "collName": "physical_scan"},
    {"module": "specs.calibrations", "collName": "index_scan"},
    {"module": "specs.calibrations", "collName": "c_int_05"},
    {"module": "specs.calibrations", "collName": "c_arr_01"},
    {"module": "specs.plan_stability2", "collName": "plan_stability2"},
    {"module": "specs.spm_2658_employee", "collName": "Employee"},
    {"module": "specs.spm_2658_type_coverage", "collName": "TypeCoverage"},
    {"module": "specs.spm_2658_distribution_coverage", "collName": "Distribution"},
    {"module": "specs.spm_2658_type_coverage", "collName": "TypeCoverage"},
    {"module": "specs.test", "collName": "Test"},
    {"module": "specs.test_types", "collName": "TypesTest"},
];

for (const spec of exampleSpecs) {
    const g = new DataGenerator({db: db, module: spec["module"], seed: 1});
    try {
        const collName = spec.collName;
        const size = 10;

        g.execute({spec: collName, size: size});
    } finally {
        g.cleanup();
    }
}
