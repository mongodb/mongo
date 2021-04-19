(function() {
"use strict";

load("skunk_shared.js");  // For 'SharedSkunkState.'

const collNaive = db.naive;
const collAttributePattern = db.attribute_pattern;
const collEnhancedAttributePattern = db.enhanced_attribute_pattern;
const collSingleWildCard = db.wildcard;
const collCompoundWildCard = db.compound_wildcard;

// Maximum value for the integer fields, minimum is 0
const kMaxInt = 20;

// Minimum and maximum values for the DateTime fields
const kBaseDate = new ISODate("1970-01-01T00:00:00Z");
const kMaxDate = new ISODate("2070-01-01T00:00:00Z");
const kMaximumSeconds = (kMaxDate.getTime() - kBaseDate.getTime()) / 1000;

function addAttributesToQuery(andClauses, attributes, numAttrs, useEnhancedAttributePattern) {
    // If this is the attribute field it needs to be a subdocument or sub-array
    if (attributes instanceof Array) {
        assert.lte(numAttrs, attributes.length);
        // Be sure to avoid selecting 'numAttrs' with replacement. If we allow specifying the same
        // attribute twice, we are likely to specify a query which is tautalogically false such as
        // attr1 = 4 and attr1 = 7.
        attributes = attributes.slice();  // Make a copy to avoid modifying the template.
        for (let i = 0; i < numAttrs; i++) {
            let selectedEntryIndex = Random.randInt(attributes.length);
            let attrEntry = attributes[selectedEntryIndex];
            attributes.splice(selectedEntryIndex, 1);  // Take it out to avoid selecting it again.
            if (useEnhancedAttributePattern) {
                andClauses.push({
                    [SharedSkunkState.kAttributesField]:
                        {k: attrEntry.k, v: SharedSkunkState.getRandomValue(attrEntry.v)}
                });
            } else {
                andClauses.push({
                    [SharedSkunkState.kAttributesField]: {
                        $elemMatch:
                            {k: attrEntry.k, v: SharedSkunkState.getRandomValue(attrEntry.v)}
                    }
                });
            }
        }
    } else {
        let attrEntries = Object.entries(attributes);
        assert.lte(numAttrs, attrEntries.length);
        // Be sure to avoid selecting 'numAttrs' with replacement. If we allow specifying the same
        // attribute twice, we are likely to specify a query which is tautalogically false such as
        // attr1 = 4 and attr1 = 7.
        for (let i = 0; i < numAttrs; i++) {
            let selectedEntryIndex = Random.randInt(attrEntries.length);
            let [attrKey, attrVal] = attrEntries[selectedEntryIndex];
            attrEntries.splice(selectedEntryIndex, 1);  // Take it out to avoid selecting it again.
            andClauses.push({
                [`${SharedSkunkState.kAttributesField}.${attrKey}`]:
                    SharedSkunkState.getRandomValue(attrVal)
            });
        }
    }
}

// Builds a compound equality query based off the template doc querying 'numField' top level fields
// and 'numAttr' attributes stored within 'SharedSkunkState.kAttributesField'. The fields at the top
// level matter for which index may be applicable, so they will be applied in order given in the
// template document. The attributes will be added randomly - so a query on two attributes may be on
// the 4th and 10th attribute for example.
function buildQuery(template, numFields, numAttrs, useEnhancedAttributePattern) {
    let andClauses = [];
    let fieldsAdded = 0;
    for (let [topKey, topValue] of Object.entries(template)) {
        if (fieldsAdded == numFields) {
            break;
        }
        assert(
            topKey != SharedSkunkState.kAttributesField,
            "Added too many top-level fields. Ran out of non-attribute fields in the template (expected non-attribute fields to come first).");
        andClauses.push({[topKey]: SharedSkunkState.getRandomValue(topValue)});
        ++fieldsAdded;
    }
    const attributes = template[SharedSkunkState.kAttributesField];
    addAttributesToQuery(andClauses, attributes, numAttrs, useEnhancedAttributePattern);
    return {$and: andClauses};
}

function avgTime(func, runs) {
    let a = [];
    runs = runs || 10;

    for (var i = 0; i < runs; i++) {
        a.push(Date.timeFunc(func))
    }

    let out = {avg: Array.avg(a), stdDev: Array.stdDev(a)};
    out.sampStdDev = Math.sqrt((1 / (runs - 1)) * (out.stdDev * out.stdDev));
    return out;
}

function buildBatchOfQueries(template, numFields, numAttrs, useEnhancedAttributePattern) {
    const kNumUniqueQueries = 1000;

    let allQueries = [];
    for (let i = 0; i < kNumUniqueQueries; i++) {
        allQueries.push(buildQuery(template, numFields, numAttrs, useEnhancedAttributePattern));
    }
    return allQueries;
}

function runQueries(collection, queryBatch) {
    let numResults = [];
    for (let query of queryBatch) {
        numResults.push(collection.find(query).itcount());
    }
    print("Average number of results: " + Array.avg(numResults));
}

function testAllNumAttrs(collection, templateDoc, maxNumAttrs, useEnhancedAttributePattern) {
    let allTimingInfo = [];
    for (let numAttrs = 1; numAttrs <= maxNumAttrs; ++numAttrs) {
        let queries = buildBatchOfQueries(templateDoc, 3, numAttrs, useEnhancedAttributePattern);
        jsTestLog("Example query: " + tojson(queries[0]));
        jsTestLog(`About to benchmark with ${numAttrs} attributes...`);
        let timingInfo = avgTime(() => runQueries(collection, queries), 5);
        jsTestLog("Avg time: " + tojson(timingInfo));
        allTimingInfo.push(timingInfo);
    }
    return allTimingInfo;
}

const kMaxNumAttrs = 5;
let allStats = {};
jsTestLog("Testing compound wildcard configuration...");
let allTimingInfo =
    testAllNumAttrs(collCompoundWildCard, SharedSkunkState.kTemplateDoc, kMaxNumAttrs);
allStats.compoundWildcard = {
    timingInfo: allTimingInfo
};

jsTestLog("Testing enhanced attribute configuration...");
allTimingInfo = testAllNumAttrs(
    collEnhancedAttributePattern, SharedSkunkState.kAttributeTemplateDoc, kMaxNumAttrs, true);
allStats.enhancedAttributePattern = {
    timingInfo: allTimingInfo
};

jsTestLog("Testing attribute configuration...");
allTimingInfo =
    testAllNumAttrs(collAttributePattern, SharedSkunkState.kAttributeTemplateDoc, kMaxNumAttrs);
allStats.attributePattern = {
    timingInfo: allTimingInfo
};

jsTestLog("Testing naive configuration...");
allTimingInfo = testAllNumAttrs(collNaive, SharedSkunkState.kTemplateDoc, kMaxNumAttrs);
allStats.naive = {
    timingInfo: allTimingInfo
};

/*
jsTestLog("Testing wildcard configuration...");
allTimingInfo = testAllNumAttrs(collSingleWildCard, SharedSkunkState.kTemplateDoc, kMaxNumAttrs);
allStats.singleWildcard = {
    timingInfo: allTimingInfo
};
*/

jsTestLog("Finished! " + tojson(allStats));
}());
