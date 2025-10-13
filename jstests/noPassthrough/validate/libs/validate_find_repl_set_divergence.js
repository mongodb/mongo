function setUpNodes(colls, numDocs) {
    colls.forEach((coll) => {
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({_id: i, field1: i, field2: "a string"});
        }
        assert.commandWorked(bulk.execute());
    });
}

function sameAllHashes(allHashes) {
    return new Set(allHashes).size == 1;
}

function clearCollections(colls) {
    colls.forEach((coll) => {
        coll.drop();
    });
}

function getInconsistentHashBuckets(partialHashes) {
    let partialHashBucketCounts = {};
    let inconsistentBuckets = [];
    // Construct a frequency map of all partial hash buckets from all nodes.
    for (let i = 0; i < partialHashes.length; ++i) {
        assert(partialHashes[i]);
        const hashBuckets = partialHashes[i];
        for (let bucketKey in hashBuckets) {
            const bucket = [bucketKey, hashBuckets[bucketKey]["hash"], hashBuckets[bucketKey]["count"]];
            partialHashBucketCounts[bucket] = (partialHashBucketCounts[bucket] || 0) + 1;
        }
    }

    // Collect any hash buckets that are not on all nodes.
    for (const [partialHashBucket, count] of Object.entries(partialHashBucketCounts)) {
        if (count != partialHashes.length) {
            inconsistentBuckets.push(partialHashBucket);
        }
    }

    return inconsistentBuckets;
}

function drillDownToDoc(getValidateResults) {
    let hashPrefixes = [];
    let missingOrExtraDocHashes = [];
    let inconsistentDocHashes = [];
    while (true) {
        jsTest.log.info(`Drilling down with [${hashPrefixes}]`);
        const partialHashes = getValidateResults({collHash: true, hashPrefixes: hashPrefixes}).map(
            (res) => res.partial,
        );
        jsTest.log.info(`partialHashes from all nodes: ${tojson(partialHashes)}`);

        let inconsistentBuckets = getInconsistentHashBuckets(partialHashes);
        let drillDownPrefixesCounts = {};
        for (let i = 0; i < inconsistentBuckets.length; ++i) {
            const inconsistentBucket = inconsistentBuckets[i].split(",");
            const hashPrefix = inconsistentBucket[0];
            const docCount = parseInt(inconsistentBucket[2]);
            if (!drillDownPrefixesCounts[hashPrefix]) {
                // Initialize the counts for 'hashPrefix'.
                drillDownPrefixesCounts[hashPrefix] = [docCount, 1];
            } else {
                // Update 'docCount' if it changes to be larger than 1, which means the prefix needs to be drilled down more.
                drillDownPrefixesCounts[hashPrefix] = [
                    drillDownPrefixesCounts[hashPrefix][0] === 1 ? docCount : drillDownPrefixesCounts[hashPrefix][0],
                    drillDownPrefixesCounts[hashPrefix][1] + 1,
                ];
            }
        }

        // Clear 'hashPrefixes' for next iteration.
        hashPrefixes = [];
        for (const [drillDownPrefix, count] of Object.entries(drillDownPrefixesCounts)) {
            const [docCount, bucketCount] = count;
            if (bucketCount === 1) {
                // If the bucket only appears on some nodes and is missing on the others, the current hashPrefix can be used to return revealed _ids.
                missingOrExtraDocHashes.push(drillDownPrefix);
            } else if (docCount === 1) {
                // If the bucket only contains one document on all nodes, no need to drill down furthermore.
                inconsistentDocHashes.push(drillDownPrefix);
            } else {
                hashPrefixes.push(drillDownPrefix);
            }
        }

        if (hashPrefixes.length === 0) {
            return [missingOrExtraDocHashes, inconsistentDocHashes];
        }
    }
}

function getRevealedHashedIds(hashedIds, getValidateResults) {
    let revealedIds = new Set();
    const revealedIdsAllNodes = getValidateResults({collHash: true, revealHashedIds: hashedIds}).map(
        (res) => res.revealedIds,
    );
    for (let i = 0; i < revealedIdsAllNodes.length; ++i) {
        for (const [_, ids] of Object.entries(revealedIdsAllNodes[i])) {
            ids.forEach((doc) => revealedIds.add(doc["_id"]));
        }
    }
    return [...revealedIds];
}

function sameIds(revealed, expected) {
    if (revealed.size !== expected.size) {
        return false;
    }

    for (const item of revealed) {
        if (!expected.has(item)) {
            return false;
        }
    }

    return true;
}

function logInconsistentDocs(colls, inconsistentIds) {
    for (const id of inconsistentIds) {
        let inconsistentDocs = [];
        colls.forEach((coll) => {
            const doc = coll.findOne({_id: id});
            inconsistentDocs.push(doc);
        });

        jsTest.log.info(`Inconsistent documents on all nodes: ${tojson(inconsistentDocs)}`);

        let sameDocument = true;
        if (inconsistentDocs.length == colls.length) {
            for (let i = 0; i < inconsistentDocs.length - 1; ++i) {
                sameDocument &= bsonBinaryEqual(inconsistentDocs[i], inconsistentDocs[i + 1]);
            }
        }
        assert(!sameDocument, inconsistentDocs);
    }
}

export function runValidateFindDivergenceTests(colls, numDocs, getValidateResults) {
    (function runEmptyCollectionTest() {
        jsTest.log.info("Testing extended validate returns the same results on an empty collection");
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        assert(sameAllHashes(allHashes), allHashes);
    })();

    (function allNodesConsistent() {
        jsTest.log.info("Testing extended validate returns the same results on the same collection");
        setUpNodes(colls, numDocs);
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        assert(sameAllHashes(allHashes), allHashes);
        clearCollections(colls);
    })();

    (function differentNumberOfDocuments() {
        jsTest.log.info("Testing extended validate identifies the missing or extra documents in the cluster");
        setUpNodes(colls, numDocs);

        const missingDocIdNode1 = Math.floor(Math.random() * numDocs);
        const missingDocIdNode2 = numDocs - 1 - missingDocIdNode1;

        jsTest.log.info(`Removing docs with '_id' [${missingDocIdNode1}, ${missingDocIdNode2}]`);

        assert.commandWorked(colls[1].deleteOne({_id: missingDocIdNode1}));
        assert.commandWorked(colls[2].deleteOne({_id: missingDocIdNode2}));

        // 1. Check if 'all' hashes are different.
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        jsTest.log.info(`allHashes from all nodes: [${allHashes}]`);
        assert(!sameAllHashes(allHashes), allHashes);

        // 2. Drill down iteratively until the hashes of the inconsistent documents are identified.
        const [missingOrExtraDocHashes, inconsistentDocIdHashes] = drillDownToDoc(getValidateResults);
        jsTest.log.info(
            `Hash prefixes of missing or extra documents: [${missingOrExtraDocHashes}].\nHash prefixes of inconsistent documents: [${inconsistentDocIdHashes}]`,
        );
        assert.eq(missingOrExtraDocHashes.length, 2, missingOrExtraDocHashes);
        assert.eq(inconsistentDocIdHashes.length, 0, inconsistentDocIdHashes);

        // 3. Get the _id field of the inconsistent documents.
        const revealedIds = getRevealedHashedIds(missingOrExtraDocHashes, getValidateResults);
        jsTest.log.info(`'_id' fields of inconsistent documents: [${revealedIds}]`);

        assert(sameIds(new Set(revealedIds), new Set([missingDocIdNode1, missingDocIdNode2])), revealedIds);
        logInconsistentDocs(colls, revealedIds);
        clearCollections(colls);
    })();

    (function differentDocumentValues() {
        jsTest.log.info("Testing extended validate identifies documents with different values in the cluster");
        setUpNodes(colls, numDocs);

        let diffDocIds = [];
        const numDiffDocs = 10;

        const diffDocIdNode1 = Math.floor(Math.random() * (numDocs - numDiffDocs));
        for (let i = 0; i < numDiffDocs; ++i) {
            diffDocIds.push(diffDocIdNode1 + i);
            assert.commandWorked(colls[1].updateOne({_id: diffDocIdNode1 + i}, {$inc: {field1: 1}}));
        }
        jsTest.log.info(`Modify documents with ids [${diffDocIds}] on node1`);

        // 1. Check if 'all' hashes are different.
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        jsTest.log.info(`allHashes from all nodes: [${allHashes}]`);
        assert(!sameAllHashes(allHashes), allHashes);

        // 2. Drill down iteratively until the hashes of the inconsistent documents are identified.
        const [missingOrExtraDocHashes, inconsistentDocIdHashes] = drillDownToDoc(getValidateResults);
        jsTest.log.info(
            `Hash prefixes of missing or extra documents: [${missingOrExtraDocHashes}].\nHash prefixes of inconsistent documents: [${inconsistentDocIdHashes}]`,
        );
        assert.eq(missingOrExtraDocHashes.length, 0, missingOrExtraDocHashes);
        assert.eq(inconsistentDocIdHashes.length, numDiffDocs, inconsistentDocIdHashes);

        // 3. Get the _id field of the inconsistent documents.
        const revealedIds = getRevealedHashedIds(inconsistentDocIdHashes, getValidateResults);
        jsTest.log.info(`'_id' fields of inconsistent documents: [${revealedIds}]`);

        assert(sameIds(new Set(revealedIds), new Set(diffDocIds)), revealedIds);
        logInconsistentDocs(colls, revealedIds);
        clearCollections(colls);
    })();

    (function differentNumOfDocumentFields() {
        jsTest.log.info(
            "Testing extended validate identifies documents with different number of fields in the cluster",
        );
        setUpNodes(colls, numDocs);

        let diffDocIds = [];
        const numDiffDocs = 10;

        const diffDocIdNode2 = Math.floor(Math.random() * (numDocs - numDiffDocs));
        for (let i = 0; i < numDiffDocs; ++i) {
            diffDocIds.push(diffDocIdNode2 + i);
            assert.commandWorked(colls[2].updateOne({_id: diffDocIdNode2 + i}, {$set: {extraField: null}}));
        }
        jsTest.log.info(`Modify documents with ids [${diffDocIds}] on node2`);

        // 1. Check if 'all' hashes are different.
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        jsTest.log.info(`allHashes from all nodes: [${allHashes}]`);
        assert(!sameAllHashes(allHashes), allHashes);

        // 2. Drill down iteratively until the hashes of the inconsistent documents are identified.
        const [missingOrExtraDocHashes, inconsistentDocIdHashes] = drillDownToDoc(getValidateResults);
        jsTest.log.info(
            `Hash prefixes of missing or extra documents: [${missingOrExtraDocHashes}].\nHash prefixes of inconsistent documents: [${inconsistentDocIdHashes}]`,
        );
        assert.eq(missingOrExtraDocHashes.length, 0, missingOrExtraDocHashes);
        assert.eq(inconsistentDocIdHashes.length, numDiffDocs, inconsistentDocIdHashes);

        // 3. Get the _id field of the inconsistent documents.
        const revealedIds = getRevealedHashedIds(inconsistentDocIdHashes, getValidateResults);
        jsTest.log.info(`'_id' fields of inconsistent documents: [${revealedIds}]`);

        assert(sameIds(new Set(revealedIds), new Set(diffDocIds)), revealedIds);
        logInconsistentDocs(colls, revealedIds);
        clearCollections(colls);
    })();

    (function differentDocumentFieldType() {
        jsTest.log.info("Testing extended validate identifies documents with different field types in the cluster");
        setUpNodes(colls, numDocs);

        let diffDocIds = [];
        const numDiffDocs = 10;

        const diffDocIdNode2 = Math.floor(Math.random() * (numDocs - numDiffDocs));
        for (let i = 0; i < numDiffDocs; ++i) {
            diffDocIds.push(diffDocIdNode2 + i);
            assert.commandWorked(
                colls[2].updateOne({_id: diffDocIdNode2 + i}, {$set: {field1: NumberDecimal(diffDocIdNode2 + i)}}),
            );
        }
        jsTest.log.info(`Modify documents with ids [${diffDocIds}] on node2`);

        // 1. Check if 'all' hashes are different.
        const allHashes = getValidateResults({collHash: true}).map((res) => res.all);
        jsTest.log.info(`allHashes from all nodes: [${allHashes}]`);
        assert(!sameAllHashes(allHashes), allHashes);

        // 2. Drill down iteratively until the hashes of the inconsistent documents are identified.
        const [missingOrExtraDocHashes, inconsistentDocIdHashes] = drillDownToDoc(getValidateResults);
        jsTest.log.info(
            `Hash prefixes of missing or extra documents: [${missingOrExtraDocHashes}].\nHash prefixes of inconsistent documents: [${inconsistentDocIdHashes}]`,
        );
        assert.eq(missingOrExtraDocHashes.length, 0, missingOrExtraDocHashes);
        assert.eq(inconsistentDocIdHashes.length, numDiffDocs, inconsistentDocIdHashes);

        // 3. Get the _id field of the inconsistent documents.
        const revealedIds = getRevealedHashedIds(inconsistentDocIdHashes, getValidateResults);
        jsTest.log.info(`'_id' fields of inconsistent documents: [${revealedIds}]`);

        assert(sameIds(new Set(revealedIds), new Set(diffDocIds)), revealedIds);
        logInconsistentDocs(colls, revealedIds);
        clearCollections(colls);
    })();
}
