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
    // TODO SERVER-124350. Drop the samples collection without this hack.
    // This is needed because system collections are special and need to be whitelisted for dropping individually.
    // Not whitelisting it here since we're expecting this to happen at SERVER-124350.
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

// Returns a sample document in system.stats.samples for the test collection.
export function getSampleDoc(samplesColl, expectedId) {
    const results = samplesColl.find({_id: expectedId}).toArray();
    // TODO SERVER-130448: allow more than one doc here once a sample can span multiple pages.
    assert.eq(
        results.length,
        1,
        `Expected exactly 1 sample doc with _id=${tojson(expectedId)}; got ${results.length}`,
    );
    return results[0];
}

// Asserts exactly one sample doc exists for this collection with the correct _id, size, and docs
// array length. Returns the doc for additional assertions by the caller.
// sampledCollName: name of collection analyze was run on
// mode: the analyze command mode (expected to be "sample").
// samplingMethod: the method used to generate the sample.
// requestedSampleSize: expected sample size encoded in the _id.
// actualSampleSize: expected doc.sampleSize value and length of doc.docs array.
// expectedSchemaVersion: what version document we expect to find
// numChunks: expected number of chunks encoded in _id and numChunks field. Expected null if samplingMethod != chunk
// expectedFields: optional list of field names every sampled doc must have. Cursory shape check
//                 that the sampling pipeline preserved fields from the source docs.
export function verifySampleDoc(
    db,
    {
        sampledCollName,
        mode,
        samplingMethod,
        requestedSampleSize,
        actualSampleSize,
        expectedSchemaVersion = kPersistentSampleSchemaVersion,
        numChunks = null,
        expectedFields = [],
    },
) {
    assert.eq("sample", mode, "verifySampleDoc only applies to mode 'sample'");

    const samplesColl = getSamplesColl(db);
    const sampledCollUuid = getCollUUID(db, sampledCollName);

    const expectedId = getExpectedId(
        sampledCollUuid,
        samplingMethod,
        requestedSampleSize,
        expectedSchemaVersion,
        numChunks,
    );

    const doc = getSampleDoc(samplesColl, expectedId);
    assert.neq(null, doc, "Expected to find a sample doc, got null");

    // Check that _id, samplingMethod, sampleSize, and schemaVersion all match expected values.
    assert.docEq(
        expectedId,
        doc[sampleDocFieldNames.idField],
        `Expected: ${sampleDocFieldNames.idField} = ${tojson(expectedId)}. Sample doc: ${tojson(doc)}`,
    );
    assert.eq(
        samplingMethod,
        doc[sampleDocFieldNames.samplingMethodField],
        `Expected: ${sampleDocFieldNames.samplingMethodField} = ${samplingMethod}. Sample doc: ${tojson(doc)}`,
    );
    assert.eq(
        requestedSampleSize,
        doc[sampleDocFieldNames.sampleSizeField],
        `Expected: ${sampleDocFieldNames.sampleSizeField} = ${requestedSampleSize}. Sample doc: ${tojson(doc)}`,
    );
    assert.eq(
        expectedSchemaVersion,
        doc[sampleDocFieldNames.schemaVersionField],
        `Expected: ${sampleDocFieldNames.schemaVersionField} = ${expectedSchemaVersion}. Sample doc: ${tojson(doc)}`,
    );

    // The number of persisted docs depends on the method used to generate the sample.
    if (samplingMethod == "random" || samplingMethod == "seqScan") {
        // These techniques persist an exact, deterministic count of documents.
        assert.eq(
            actualSampleSize,
            doc[sampleDocFieldNames.docsField].length,
            `Value of size field and length of docs array don't match. Sample doc: ${tojson(doc)}`,
        );
    } else if (samplingMethod == "chunk") {
        // When using chunk sampling, actual num docs sampled might be lower than the parameter passed to `analyze`
        // depending on whether sampleSize divides evenly into the specified number of chunks,
        // and whether the random cursors fall close to the end of the collection. In the worst case, every random
        // cursor falls on the last document in the collection which means every chunk only has 1 document, so the
        // entire sample only has numChunks documents
        assert.between(
            doc[sampleDocFieldNames.numChunksField],
            doc[sampleDocFieldNames.docsField].length,
            actualSampleSize,
            `Expected: ${sampleDocFieldNames.sampleSizeField} <= ${actualSampleSize}. Sample doc: ${tojson(doc)}`,
        );

        assert.eq(
            numChunks,
            doc[sampleDocFieldNames.numChunksField],
            `Expected ${sampleDocFieldNames.numChunksField} = ${numChunks}. Sample doc: ${tojson(doc)}`,
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
            doc[sampleDocFieldNames.docsField].length,
            actualSampleSize,
            `Expected docs array length in [0, ${actualSampleSize}]. Sample doc: ${tojson(doc)}`,
        );
    }

    // Verify that every sampled doc contains the expected fields from the source collection
    for (const sampledDoc of doc[sampleDocFieldNames.docsField]) {
        for (const field of expectedFields) {
            assert(
                sampledDoc.hasOwnProperty(field),
                `Sampled doc missing expected field '${field}'. Sampled doc: ${tojson(sampledDoc)}`,
            );
        }
    }
    return doc;
}

/**
 * Private helpers
 */

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
