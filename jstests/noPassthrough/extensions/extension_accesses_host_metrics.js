/**
 * Tests that an extension stage can access host OpDebug metrics via getHostMetrics(). This is validated by having
 * the extension stage report the host metrics as extension operation metrics visible in the profiler.
 *
 * $readNDocuments desugars to [$produceIds, $_internalSearchIdLookup]. At EOF $produceIds
 * reads the host's accumulated idLookup counters via getHostMetrics() and records
 * them as its own extension operation metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

withExtensions(
    {"libread_n_documents_mongo_extension.so": {}},
    (conn) => {
        const db = conn.getDB("test");
        const collName = jsTestName();
        const coll = db[collName];
        coll.drop();

        // Use profiling level 2 so every operation is profiled.
        assert.commandWorked(db.runCommand({profile: 2}));

        // Insert documents with _ids 0 and 1. $readNDocuments with numDocs:4 will produce IDs
        // 0-3, so idLookup will find 2 of 4: success rate 0.5.
        const docs = [
            {_id: 0, val: "zero"},
            {_id: 1, val: "one"},
        ];
        assert.commandWorked(coll.insertMany(docs));

        const comment = "extension_accesses_host_metrics_test";
        const results = coll.aggregate([{$readNDocuments: {numDocs: 4}}], {comment}).toArray();
        assert.sameMembers(results, docs);

        // Find the profiler entry for this aggregation.
        const profileEntries = db.system.profile
            .find({
                "command.comment": comment,
            })
            .toArray();
        assert.eq(profileEntries.length, 1, `Expected 1 profiler entry, got: ${tojson(profileEntries)}`);

        const entry = profileEntries[0];
        assert(entry.extensionMetrics !== undefined, entry);

        const produceIdsMetrics = entry.extensionMetrics["$produceIds"];
        assert(produceIdsMetrics !== undefined, entry);

        // 2/4 IDs sent through IdLookup had matches.
        assert.eq(produceIdsMetrics.docsSeenByIdLookup, 4, produceIdsMetrics);
        assert.eq(produceIdsMetrics.docsReturnedByIdLookup, 2, produceIdsMetrics);
        assert.eq(produceIdsMetrics.idLookupSuccessRate, 0.5, produceIdsMetrics);
    },
    ["standalone"],
);
