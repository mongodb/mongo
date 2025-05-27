/**
 * Utility functions for creating and querying a generic vector search index.
 */

import {createSearchIndex} from "jstests/libs/search.js";
import {checkForExistingIndex} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

/**
 * The number of dimensions for a generic vector search index, set to avoid OOM errors.
 */
const kGenericNumDimensions = 512;
const kGenericNumDocs = 10000;
const kGenericDbName = "vector_search_shared_db";
const kGenericCollName = "genericVectorSearch";
const kGenericIndexName = "vector_search";

/**
 * Generates an array of random values from [-1, 1). Useful for explain when the actual documents
 * and scoring for $vectorSearch does not matter.
 *
 * @param {*} n length of array
 * @returns array of random values from [-1, 1]
 */
export function generateRandomVectorEmbedding(n) {
    // Generates array of random values from [-1, 1)
    let embedding = Array.from({length: n}, function() {
        return Math.random() * 2 - 1;
    });
    return embedding;
}

function getGenericVectorSearchColl() {
    return db.getSiblingDB(kGenericDbName).getCollection(kGenericCollName);
}

/**
 * Creates a generic vector search index with 10,000 docs with a dimensionality of 512.
 */
export function createGenericVectorSearchIndex() {
    const coll = getGenericVectorSearchColl();

    // Returning the existing collection if it already has an index.
    if (checkForExistingIndex(coll, kGenericIndexName)) {
        return coll;
    };

    let docs = [];
    for (let i = 0; i < kGenericNumDocs; i++) {
        docs.push({
            _id: i,
            a: i % kGenericNumDimensions,
            embedding: generateRandomVectorEmbedding(kGenericNumDimensions)
        });
    }
    assert.commandWorked(coll.insertMany(docs));
    let index = {
        name: kGenericIndexName,
        type: "vectorSearch",
        definition: {
            "fields": [{
                "type": "vector",
                "numDimensions": kGenericNumDimensions,
                "path": "embedding",
                "similarity": "euclidean"
            }]
        }
    };
    createSearchIndex(coll, index);
    return coll;
}

/**
 * Get a query with a random vector embedding for the generic vector search index. Note that while
 * the docs officially recommend setting the numCandidates value to 20x the limit, this primarily
 * affects result set quality and not correctness. We purposely keep it at 2x here to avoid slowing
 * down patch builds.
 */
export function getGenericVectorSearchQuery(limit) {
    return {
        $vectorSearch: {
            queryVector: generateRandomVectorEmbedding(kGenericNumDimensions),
            path: "embedding",
            numCandidates: limit * 2,
            index: kGenericIndexName,
            limit: limit,
        }
    };
}
