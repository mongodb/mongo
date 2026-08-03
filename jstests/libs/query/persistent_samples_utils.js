/**
 * Utils for writing tests for the persistent samples collection/analyze command in sample mode
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

export const samplesCollName = "system.stats.samples";

// Mirrors ce::kPersistentSampleSchemaVersion.
export const kPersistentSampleSchemaVersion = 1;

// Field names mirroring persistent_sample.idl — update here if the IDL field names change.
export const sampleDocFieldNames = {
    idField: "_id",
    uuidField: "collectionUuid",
    samplingMethodField: "samplingMethod",
    sampleSizeField: "sampleSize",
    numChunksField: "numChunks",
    createdAtField: "createdAt",
    docsField: "docs",
    schemaVersionField: "schemaVersion",
    pageNoField: "pageNo",
};

export function getExpectedSamplingMethod(db, requestedSamplingMethod) {
    const {internalQuerySamplingBySequentialScan, internalQuerySamplingByStrides} =
        assert.commandWorked(
            db.adminCommand({
                getParameter: 1,
                internalQuerySamplingBySequentialScan: 1,
                internalQuerySamplingByStrides: 1,
            }),
        );
    if (internalQuerySamplingBySequentialScan) {
        return "seqScan";
    }
    if (internalQuerySamplingByStrides) {
        return "strides";
    }
    return requestedSamplingMethod;
}

export function getPersistentSamplesConfig(db) {
    const config = assert.commandWorked(
        db.adminCommand({
            getParameter: 1,
            internalQueryDisablePlanCache: 1,
            internalQuerySamplingCEMethod: 1,
            internalQuerySamplingBySequentialScan: 1,
        }),
    );

    return {
        internalQueryDisablePlanCache: config.internalQueryDisablePlanCache,
        internalQuerySamplingCEMethod: config.internalQuerySamplingCEMethod,
        internalQuerySamplingBySequentialScan: config.internalQuerySamplingBySequentialScan,
    };
}

export function setPersistentSamplesConfig(
    db,
    {
        internalQueryDisablePlanCache,
        internalQuerySamplingCEMethod,
        internalQuerySamplingBySequentialScan,
    },
) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryDisablePlanCache,
            internalQuerySamplingCEMethod,
            internalQuerySamplingBySequentialScan,
        }),
    );
}

// Get the default sample size based on query knobs so the test stays correct if knob values change
export function defaultSampleSize(db) {
    const knobValues = getSampleSizeRelatedKnobs(db);
    return calculateSampleSize(
        knobValues.kSamplingConfidenceInterval,
        knobValues.kSamplingMarginOfError,
    );
}

// Get the default num chunks based on query knobs so the test stays correct if knob values change
export function defaultNumChunks(db) {
    const knobValues = getSampleSizeRelatedKnobs(db);
    return knobValues.kInternalQueryNumChunksForChunkBasedSampling;
}

// Returns the UUID string for a given collection
export function getCollUUID(db, collName) {
    return extractUUIDFromObject(db.getCollectionInfos({name: collName})[0].info.uuid);
}

export function getSamplesColl(db) {
    return db[samplesCollName];
}

export function dropSamplesColl(db) {
    // This is needed because system collections are special and need to be whitelisted for dropping individually.
    // Not whitelisting it since we don't expect customers to ever drop the collection themselves.
    assert.commandWorked(
        db.adminCommand({
            applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {drop: samplesCollName}}],
        }),
    );
}

// Asserts that the persistent samples collection exists and is clustered on _id.
export function assertSamplesCollClustered(db) {
    const collInfos = db.getCollectionInfos({name: samplesCollName});
    assert.eq(1, collInfos.length, `Expected exactly one ${samplesCollName} collection to exist`, {
        collInfos,
    });
    const clusteredIndex = collInfos[0].options.clusteredIndex;
    assert(clusteredIndex, `Expected ${samplesCollName} to be clustered`, {collInfos});
    assert.eq(
        {[sampleDocFieldNames.idField]: 1},
        clusteredIndex.key,
        `Expected ${samplesCollName} to be clustered on _id`,
        {collInfos},
    );
}

// Create a BSON object of exactly the given size
export function makeDocOfSize(targetBytes, id = 0) {
    let doc = {_id: id, pad: ""};
    const overhead = Object.bsonsize(doc); // size with empty pad string
    doc.pad = "x".repeat(targetBytes - overhead);
    assert.eq(Object.bsonsize(doc), targetBytes);
    return doc;
}

// Returns an array of `numDocs` documents whose BSON sizes sum to exactly `totalBytes`.
export function makeDocsOfTotalSize(numDocs, totalBytes) {
    let docSize = Math.floor(totalBytes / numDocs);
    const docs = [];
    for (let i = 0; i < numDocs; ++i) {
        if (i === numDocs - 1) {
            // Ensure the cumulative size is exactly totalBytes.
            docSize += totalBytes % numDocs;
        }
        docs.push(makeDocOfSize(docSize, /*id*/ i));
    }
    return docs;
}

// Returns the expected _id object for a sample document. The field order here must mirror
// ce::PersistentSampleId in persistent_sample.idl in order to successfully match the _id object.
// samplingType is "random" or "chunk"; sampleSize is the sample count encoded in the _id.
// numChunks is included in the _id only for chunk mode.
// pageNo defaults to 0 (expected when only 1 page exists) and is always present in the _id.
export function getExpectedId(
    uuid,
    samplingType,
    sampleSize,
    expectedSchemaVersion = kPersistentSampleSchemaVersion,
    numChunks = null,
    pageNo = 0,
) {
    const id = {
        [sampleDocFieldNames.schemaVersionField]: NumberInt(expectedSchemaVersion),
        [sampleDocFieldNames.uuidField]: UUID(uuid),
        [sampleDocFieldNames.samplingMethodField]: samplingType,
        [sampleDocFieldNames.sampleSizeField]: NumberLong(sampleSize),
    };
    if (numChunks !== null) {
        assert.eq(
            "chunk",
            samplingType,
            `numChunks should only be passed for chunk sampling; got ${samplingType}`,
        );
        id[sampleDocFieldNames.numChunksField] = NumberInt(numChunks);
    }
    id[sampleDocFieldNames.pageNoField] = NumberInt(pageNo);
    return id;
}

// Returns the single sample page document matching the given _id.
export function getSampleDoc(samplesColl, expectedId) {
    const results = samplesColl.find({_id: expectedId}).toArray();
    assert.eq(
        results.length,
        1,
        `Expected exactly 1 sample doc with _id=${tojson(expectedId)}; got ${results.length}`,
    );
    return results[0];
}

// Returns a query filter that matches every page of a single sample
export function getSampleLookupFilter(
    uuid,
    samplingType,
    sampleSize,
    expectedSchemaVersion = kPersistentSampleSchemaVersion,
    numChunks = null,
) {
    const id = getExpectedId(uuid, samplingType, sampleSize, expectedSchemaVersion, numChunks);
    return idToLookupFilter(id);
}

// Validates a full persisted sample, which may be split across multiple pages.
export function validatePersistentSample(
    db,
    {
        sampledCollName,
        samplingMethod,
        requestedSampleSize,
        actualSampleSize,
        expectedSchemaVersion = kPersistentSampleSchemaVersion,
        numChunks = null,
        expectedFields = [],
        expectedNumPages = 1,
    },
) {
    // The sampling method specified in the analyze command may be overridden by test-only knobs.
    samplingMethod = getExpectedSamplingMethod(db, samplingMethod);

    const samplesColl = getSamplesColl(db);
    const sampledCollUuid = getCollUUID(db, sampledCollName);

    const filter = getSampleLookupFilter(
        sampledCollUuid,
        samplingMethod,
        requestedSampleSize,
        expectedSchemaVersion,
        numChunks,
    );

    const pages = samplesColl.find(filter).toArray();

    assert.eq(pages.length, expectedNumPages, "unexpected number of sample pages", {
        filter,
        numPages: pages.length,
    });

    let totalDocs = 0;
    for (let i = 0; i < pages.length; ++i) {
        const page = pages[i];
        validateSamplePage(page, {
            sampledCollUuid,
            samplingMethod,
            requestedSampleSize,
            actualSampleSize,
            expectedSchemaVersion,
            numChunks,
            expectedFields,
            pageNo: i,
        });
        totalDocs += page[sampleDocFieldNames.docsField].length;
    }

    assertPagesShareMetadata(pages);

    validateSampledDocCount(samplingMethod, totalDocs, actualSampleSize, numChunks, {filter});

    return pages;
}

/**
 * Private helpers
 */

// Converts _id object into dotted-path query filter to match all pages of a sample
function idToLookupFilter(id) {
    const idField = sampleDocFieldNames.idField;
    const filter = {};
    for (const subField of Object.keys(id)) {
        if (subField === sampleDocFieldNames.pageNoField) {
            continue;
        }
        filter[`${idField}.${subField}`] = id[subField];
    }
    return filter;
}

function validateSamplePage(
    page,
    {
        sampledCollUuid,
        samplingMethod,
        requestedSampleSize,
        actualSampleSize,
        expectedSchemaVersion,
        numChunks = null,
        expectedFields = [],
        pageNo,
    },
) {
    assert.neq(null, page, "Expected to find a sample page, got null");

    const expectedId = getExpectedId(
        sampledCollUuid,
        samplingMethod,
        requestedSampleSize,
        expectedSchemaVersion,
        numChunks,
        pageNo,
    );
    const sampleId = page[sampleDocFieldNames.idField];

    assert.docEq(expectedId, sampleId, `Unexpected ${sampleDocFieldNames.idField}`, {expectedId});
    assert.eq(
        samplingMethod,
        page[sampleDocFieldNames.samplingMethodField],
        "Unexpected samplingMethod",
        {sampleId},
    );
    assert.eq(
        requestedSampleSize,
        page[sampleDocFieldNames.sampleSizeField],
        "Unexpected sampleSize",
        {sampleId},
    );
    assert.eq(
        expectedSchemaVersion,
        page[sampleDocFieldNames.schemaVersionField],
        "Unexpected schemaVersion",
        {sampleId},
    );
    if (numChunks !== null) {
        assert.eq(numChunks, page[sampleDocFieldNames.numChunksField], "Unexpected numChunks", {
            sampleId,
        });
    }

    // Verify that every sampled doc contains the expected fields from the source collection.
    const pageDocs = page[sampleDocFieldNames.docsField];
    for (const sampledDoc of pageDocs) {
        for (const field of expectedFields) {
            assert(
                sampledDoc.hasOwnProperty(field),
                `Sampled doc missing expected field '${field}'`,
                {sampleId, field},
            );
        }
    }

    // A single page can never hold more docs than the whole sample, and a persisted page should
    // never be empty when the sample itself is non-empty.
    assert.lte(
        pageDocs.length,
        actualSampleSize,
        "a page cannot contain more docs than the total sample size",
        {
            sampleId,
            pageDocs: pageDocs.length,
            actualSampleSize,
        },
    );
    if (actualSampleSize > 0) {
        assert.gte(pageDocs.length, 1, "persisted sample page is unexpectedly empty", {sampleId});
    }
}

// Checks that the total number of docs in a sample is reasonable given the sampling method.
function validateSampledDocCount(samplingMethod, docsCount, actualSampleSize, numChunks, attr) {
    if (samplingMethod == "random" || samplingMethod == "seqScan") {
        // These techniques persist an exact, deterministic count of documents.
        assert.eq(
            actualSampleSize,
            docsCount,
            "sampleSize and sampled docs count don't match",
            attr,
        );
    } else if (samplingMethod == "chunk") {
        // When using chunk sampling, actual num docs sampled might be lower than the parameter passed to `analyze`
        // depending on whether sampleSize divides evenly into the specified number of chunks,
        // and whether the random cursors fall close to the end of the collection. In the worst case, every random
        // cursor falls on the last document in the collection which means every chunk only has 1 document, so the
        // entire sample only has numChunks documents
        assert.neq(
            null,
            numChunks,
            "numChunks must be provided to validate a chunk-sampled count",
            attr,
        );
        assert.between(
            numChunks,
            docsCount,
            actualSampleSize,
            `Expected sampled docs count in [${numChunks}, ${actualSampleSize}]`,
        );
    } else {
        assert.eq(
            "strides",
            samplingMethod,
            `Expected samplingMethod="strides", got: ${samplingMethod}`,
        );
        // For strides sampling, a variable number of docs is kept (those whose hash matches the stride), capped
        // at the requested sample size, so only an upper bound can be asserted.
        assert.between(
            0,
            docsCount,
            actualSampleSize,
            `Expected sampled docs count in [0, ${actualSampleSize}]`,
        );
    }
}

// Checks that every page of a sample carries identical metadata
function assertPagesShareMetadata(pages) {
    if (pages.length <= 1) {
        return;
    }

    const metaFields = [
        sampleDocFieldNames.uuidField,
        sampleDocFieldNames.samplingMethodField,
        sampleDocFieldNames.sampleSizeField,
        sampleDocFieldNames.numChunksField,
        sampleDocFieldNames.schemaVersionField,
        sampleDocFieldNames.createdAtField,
        // Only compare fields present on the first page (e.g. numChunks is absent for non-chunk
        // samples).
    ].filter((field) => pages[0][field] !== undefined);

    const firstId = pages[0][sampleDocFieldNames.idField];
    const firstIdSansPageNo = idWithoutPageNo(firstId);
    for (let i = 1; i < pages.length; ++i) {
        const page = pages[i];
        const pageId = page[sampleDocFieldNames.idField];

        assert.docEq(
            firstIdSansPageNo,
            idWithoutPageNo(pageId),
            "page _id differs across pages (ignoring pageNo)",
            {firstId, pageId},
        );
        for (const field of metaFields) {
            assert.docEq(
                {[field]: pages[0][field]},
                {[field]: page[field]},
                `page metadata field '${field}' differs across pages`,
                {pageNo: pageId[sampleDocFieldNames.pageNoField]},
            );
        }
    }
}

function idWithoutPageNo(id) {
    const copy = Object.assign({}, id);
    delete copy[sampleDocFieldNames.pageNoField];
    return copy;
}

// Mirror of C++ getZScore() in sampling_estimator_impl.cpp.
function getZScore(ci) {
    if (ci === "90") return 1.645;
    if (ci === "95") return 1.96;
    if (ci === "99") return 2.576;
    throw new Error(`Unknown confidence interval: ${ci}`);
}

// Mirror of C++ SamplingEstimatorImpl::calculateSampleSize().
function calculateSampleSize(ci, marginOfError) {
    const ciWidth = (2 * marginOfError) / 100.0;
    return Math.round(getZScore(ci) ** 2 / ciWidth ** 2);
}

// Read the knobs that drive sample size and chunk count
function getSampleSizeRelatedKnobs(db) {
    const {
        samplingConfidenceInterval: kCI,
        samplingMarginOfError: kMoE,
        internalQueryNumChunksForChunkBasedSampling: kNumChunks,
    } = assert.commandWorked(
        db.adminCommand({
            getParameter: 1,
            samplingConfidenceInterval: 1,
            samplingMarginOfError: 1,
            internalQueryNumChunksForChunkBasedSampling: 1,
        }),
    );

    return {
        kSamplingConfidenceInterval: kCI,
        kSamplingMarginOfError: kMoE,
        kInternalQueryNumChunksForChunkBasedSampling: kNumChunks,
    };
}
