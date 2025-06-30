import {checkPauseAfterPopulate} from "jstests/libs/pause_after_populate.js";

/**
 * For security reasons, the Math.random() function can not be seeded to provide
 * reproducible data for tests. Therefore we need to provide our own replacement.
 */

function seededRandom(seed) {
    let m = 0x80000000;  // 2**31
    let a = 1664525;
    let c = 1013904223;

    let state = seed;

    return function() {
        state = (a * state + c) % m;
        return state / m;
    };
}

function generateZipfianList(size, s) {
    if (s <= 0)
        throw new Error("The skewness parameter 's' must be greater than 0.");

    // Step 1: Calculate the normalization constant
    let normalizationConstant = 0;
    for (let i = 1; i <= size; i++) {
        normalizationConstant += 1 / Math.pow(i, s);
    }

    // Step 2: Generate the Zipfian probabilities
    const probabilities = [];
    for (let i = 1; i <= size; i++) {
        probabilities.push((1 / Math.pow(i, s)) / normalizationConstant);
    }

    // Step 3: Generate the output list based on probabilities
    const zipfianValues = [];
    const random = seededRandom(1);
    for (let i = 0; i < size; i++) {
        let cumulativeProbability = 0;

        for (let j = 0; j < probabilities.length; j++) {
            cumulativeProbability += probabilities[j];
            if (random() < cumulativeProbability) {
                zipfianValues.push(j + 1);  // Rank starts from 1
                break;
            }
        }
    }

    return zipfianValues;
}

/**
 * Populates a collection with a simple dataset for plan stability testing.
 * The dataset consists of documents with various indexed and non-indexed fields.
 *
 * The prefix of the field name indicates the value distribution:
 * - 'i_' for integer values (sequential)
 * - 'z_' for Zipfian distribution values
 * - 'c_' for constant value (1)
 * - 'd_', 'h_', 'k_' for modular values (mod 10, 100, 1000 respectively)
 *   resulting in selectivity of 10%, 1%, and 0.1% respectively.
 * - 'a_' for an array containing all the above values.
 *
 * The suffix of the field name indicates the indexing strategy:
 * - '_idx' for indexed fields
 * - '_noidx' for non-indexed fields
 * - '_compound' for compound indexes, where the field is indexed separately
 *   bug participates in a compound index with other fields as well.
 */
export function populateSimplePlanStabilityDataset(collName, collSize) {
    assert(collSize > 0, "Size must be a positive integer");

    const coll = db[collName];

    coll.drop();

    jsTestLog("Generating " + collSize + " values with Zipfian distribution");
    const zipfianValues = generateZipfianList(collSize, 1.5);

    jsTestLog("Generated " + collSize + " documents");
    const documents = [];
    for (let i = 0; i < collSize; i++) {
        documents.push({
            i_idx: i,
            z_idx: zipfianValues[i],
            c_idx: 1,
            d_idx: i % 10,
            h_idx: i % 100,
            k_idx: i % 1000,
            a_idx: [i, zipfianValues[i], i % 10, i % 100, i % 1000],
            i_noidx: i,
            z_noidx: zipfianValues[i],
            c_noidx: 1,
            d_noidx: i % 10,
            h_noidx: i % 100,
            k_noidx: i % 1000,
            a_noidx: [i, zipfianValues[i], i % 10, i % 100, i % 1000],
            i_compound: i,
            z_compound: zipfianValues[i],
            c_compound: 1,
            d_compound: i % 10,
            h_compound: i % 100,
            k_compound: i % 1000,
            a_compound: [i, zipfianValues[i], i % 10, i % 100, i % 1000],
        });
    }

    jsTestLog("Inserting " + collSize + " documents into collection " + collName);
    coll.insertMany(documents);

    jsTestLog("Creating indexes on collection " + collName);
    const fields = ['i', 'z', 'c', 'd', 'h', 'k', 'a'];
    for (const field of fields) {
        assert.commandWorked(coll.createIndex({[field + "_idx"]: 1}));
        assert.commandWorked(coll.createIndex({[field + "_compound"]: 1}));

        for (const suffix of ['_idx', '_noidx', '_compound']) {
            db.runCommand({analyze: collName, key: field + suffix, numberBuckets: 1000});
        }
    }

    [{i_compound: 1, z_compound: 1},
     {z_compound: 1, c_compound: 1},
     {c_compound: 1, d_compound: 1},
     {d_compound: 1, k_compound: 1},
     {k_compound: 1, a_compound: 1},
     {a_compound: 1, i_compound: 1}]
        .forEach(compoundIndex => {
            assert.commandWorked(coll.createIndex(compoundIndex));
        });

    jsTestLog("Done creating indexes.");

    checkPauseAfterPopulate();
}
