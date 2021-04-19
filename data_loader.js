(function() {
"use strict";

load("skunk_shared.js");  // For 'SharedSkunkState.'

const collNaive = db.naive;
const collAttributePattern = db.attribute_pattern;
const collEnhancedAttributePattern = db.enhanced_attribute_pattern;
const collSingleWildCard = db.wildcard;
const collCompoundWildCard = db.compound_wildcard;

const kIndexesForNaive = [
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr1": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr2": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr3": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr4": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr5": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr6": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr7": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr8": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr9": 1},
    {"field1": 1, "field2": 1, "field3": 1, "attributes.attr10": 1}
];

const kIndexesForAttributes =
    [{"field1": 1, "field2": 1, "field3": 1, "attributes.k": 1, "attributes.v": 1}];
const kIndexesForEnhancedAttributes = [{"field1": 1, "field2": 1, "field3": 1, "attributes": 1}];

const kIndexesForWildcard = [{"attributes.$**": 1}];
const kIndexesForCompoundWildcard = [{field1: 1, field2: 1, field3: 1, "attributes.$**": 1}];

/**
 * Generates a document based on a template. It will take a template document, it will take
 * field names and data types.
 *
 * 'template':
 * The document must have a list of scalar fields, the name and data types of these fields will
 * be the same in the output document with random values. The document must have a field called
 * attribute that should be a subdocument and all its fields should be scalar (no arrays, no
 * subdocuments).
 *
 * If 'add_id' is true, generates a new ObjectId for the _id.
 *
 * Returns an object with random values based on the template
 */
function getBaseDocument(template, add_id) {
    let output = {}
    // iterates thru each field
    for (let [key, value] of Object.entries(template)) {
        if (key != SharedSkunkState.kAttributesField) {
            // If the field is not attribute get a random value based on the current type
            output[key] = SharedSkunkState.getRandomValue(value)
        } else {
            // If this is the attribute field it needs to be a subdocument or sub-array
            if (value instanceof Array) {
                let attributes = [];
                for (let entry of value) {
                    attributes.push({k: entry.k, v: SharedSkunkState.getRandomValue(entry.v)});
                }
                output[key] = attributes;
            } else {
                let attributes = {};
                for (let [attrKey, attrValue] of Object.entries(value)) {
                    attributes[attrKey] = SharedSkunkState.getRandomValue(attrValue);
                }
                output[key] = attributes;
            }
        }
        // overrides _id
        if (add_id) {
            output["_id"] = new ObjectId();
        }
    }
    return output;
}

function loadData(collection, indexes, template) {
    const kNumDocs = 100 * 1000;
    jsTestLog("Building indexes: " + tojson(indexes));
    for (let index of indexes) {
        collection.createIndex(index);
    }
    jsTestLog("Building bulk op for insert... Example doc: " + tojson(getBaseDocument(template)));
    const bulkOp = collection.initializeUnorderedBulkOp();
    for (let docId = 0; docId < kNumDocs; ++docId) {
        bulkOp.insert(getBaseDocument(template));
    }
    jsTestLog("Starting clock for insert...");
    let elapsed = Date.timeFunc(() => bulkOp.execute());
    jsTestLog(`Loading data done: ${elapsed}ms`);
    const indexStats = collection.aggregate([{$collStats: {storageStats: {scale: 1024 * 1024}}}])
                           .toArray()[0]
                           .storageStats.indexSizes;
    jsTestLog(`Index stats: ${tojson(indexStats)}`);
    return [elapsed, indexStats];
}

let allStats = {};
jsTestLog("Loading data for naive configuration...");
let [elapsed, indexStats] = loadData(collNaive, kIndexesForNaive, SharedSkunkState.kTemplateDoc);
allStats.naive = {
    loadingTime: elapsed,
    indexStats: indexStats,
};

jsTestLog("Loading data for attribute configuration...");
[elapsed, indexStats] =
    loadData(collAttributePattern, kIndexesForAttributes, SharedSkunkState.kAttributeTemplateDoc);
allStats.attributePattern = {
    loadingTime: elapsed,
    indexStats: indexStats,
};

jsTestLog("Loading data for enhanced attribute configuration...");
[elapsed, indexStats] = loadData(collEnhancedAttributePattern,
                                 kIndexesForEnhancedAttributes,
                                 SharedSkunkState.kAttributeTemplateDoc);
allStats.enhancedAttributePattern = {
    loadingTime: elapsed,
    indexStats: indexStats
};

/*
jsTestLog("Loading data for wildcard configuration...");
[elapsed, indexStats] =
    loadData(collSingleWildCard, kIndexesForWildcard, SharedSkunkState.kTemplateDoc);
allStats.singleWildcard = {
    loadingTime: elapsed,
    indexStats: indexStats,
};
*/

jsTestLog("Loading data for compound wildcard configuration...");
[elapsed, indexStats] =
    loadData(collCompoundWildCard, kIndexesForCompoundWildcard, SharedSkunkState.kTemplateDoc);
allStats.compoundWildcard = {
    loadingTime: elapsed,
    indexStats: indexStats,
};

jsTestLog("Finished! " + tojson(allStats));
}());
