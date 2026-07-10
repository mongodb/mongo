/**
 * Integration tests for $_internalDocumentResultsAndMetadata (DRM) inside $rankFusion / $scoreFusion
 * input pipelines.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 *  featureFlagRankFusionFull,
 *  featureFlagSearchHybridScoringFull,
 *  requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

// Marker embedded in the $scoreDetails metadata the multi_stream source emits when
// advertiseScoreDetails is set (matches kScoreDetailsMarker in multi_stream.cpp). It only survives
// into the fused output if the DRM source is recognized as scoreDetails-generating.
const kScoreDetailsMarker = "extensionMultiStreamScoreDetails";

// A ranked + selection DRM source.
function rankedSelectionSource(numDocs) {
    return {$extensionMultiStream: {numDocs, advertiseSortKey: true, isSelectionStage: true}};
}

// A scored + selection DRM source.
function scoredSelectionSource(numDocs) {
    return {$extensionMultiStream: {numDocs, isSelectionStage: true}};
}

// Builds a $rankFusion whose pipeline "a" is the given DRM source stage; "b" is a plain ranked
// selection pipeline so the fusion itself is well-formed.
function rankFusion(sourceStage) {
    return {$rankFusion: {input: {pipelines: {a: [sourceStage], b: [{$sort: {y: 1}}]}}}};
}

// Builds a $scoreFusion whose pipeline "a" is the given DRM source stage; "b" is a plain scored
// selection pipeline.
function scoreFusion(sourceStage) {
    return {
        $scoreFusion: {
            input: {
                pipelines: {
                    a: [sourceStage],
                    b: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    };
}

describe("$_internalDocumentResultsAndMetadata inside hybrid fusion input pipelines", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 100, y: 1},
                {_id: 101, y: 2},
                {_id: 102, y: 3},
            ]),
        );
    });

    // The DRM source synthesizes numSourceDocs docs with _id 0..numSourceDocs-1. Those are the ids
    // we expect to survive fusion.
    const numSourceDocs = 3;
    const expectedSourceIds = Array.from({length: numSourceDocs}, (_, i) => i);

    function assertFusedResultHasDrmSourceDocs(res) {
        assert.gt(res.length, 0, "expected fused documents from the DRM source", {res});
        const ids = new Set(res.map((d) => d._id));
        for (const id of expectedSourceIds) {
            assert(ids.has(id), `expected DRM source doc _id:${id} in fused result`, {res});
        }
    }

    it("accepts a ranked+selection DRM source in a $rankFusion input pipeline", function () {
        const res = coll.aggregate([rankFusion(rankedSelectionSource(numSourceDocs))]).toArray();
        assertFusedResultHasDrmSourceDocs(res);
    });

    it("accepts a scored+selection DRM source in a $scoreFusion input pipeline", function () {
        const res = coll.aggregate([scoreFusion(scoredSelectionSource(numSourceDocs))]).toArray();
        assertFusedResultHasDrmSourceDocs(res);
    });

    it("rejects a non-selection DRM source in a $rankFusion input pipeline", function () {
        // Ranked but not selection (isSelectionStage omitted) -> forwarded selection=false.
        const pipeline = [
            rankFusion({$extensionMultiStream: {numDocs: numSourceDocs, advertiseSortKey: true}}),
        ];
        assert.commandFailedWithCode(
            coll.runCommand("aggregate", {pipeline, cursor: {}}),
            12108704,
        );
    });

    it("rejects a non-selection DRM source in a $scoreFusion input pipeline", function () {
        // Scored but not selection (isSelectionStage omitted) -> forwarded selection=false.
        const pipeline = [scoreFusion({$extensionMultiStream: {numDocs: numSourceDocs}})];
        assert.commandFailedWithCode(
            coll.runCommand("aggregate", {pipeline, cursor: {}}),
            12108713,
        );
    });

    it("rejects a selection but non-ranked DRM source in a $rankFusion input pipeline", function () {
        // Selection, but advertiseSortKey omitted -> forwarded ranked=false.
        const pipeline = [
            rankFusion({$extensionMultiStream: {numDocs: numSourceDocs, isSelectionStage: true}}),
        ];
        assert.commandFailedWithCode(
            coll.runCommand("aggregate", {pipeline, cursor: {}}),
            12108702,
        );
    });

    it("rejects a selection but non-scored DRM source in a $scoreFusion input pipeline", function () {
        // Selection, but suppressScore drops searchScore -> forwarded scored=false.
        const pipeline = [
            scoreFusion({
                $extensionMultiStream: {
                    numDocs: numSourceDocs,
                    isSelectionStage: true,
                    suppressScore: true,
                },
            }),
        ];
        assert.commandFailedWithCode(
            coll.runCommand("aggregate", {pipeline, cursor: {}}),
            12108712,
        );
    });

    it("accepts a DRM source in $rankFusion nested in a $unionWith subpipeline", function () {
        const res = coll
            .aggregate([
                {$limit: 1},
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [rankFusion(rankedSelectionSource(numSourceDocs))],
                    },
                },
            ])
            .toArray();
        assertFusedResultHasDrmSourceDocs(res);
    });

    it("accepts a DRM source in $rankFusion nested in a $lookup subpipeline", function () {
        const res = coll
            .aggregate([
                {$limit: 1},
                {
                    $lookup: {
                        from: coll.getName(),
                        pipeline: [rankFusion(rankedSelectionSource(numSourceDocs))],
                        as: "fused",
                    },
                },
            ])
            .toArray();
        assert.eq(res.length, 1, {res});
        assertFusedResultHasDrmSourceDocs(res[0].fused);
    });

    it("propagates scoreDetails from a DRM source through $scoreFusion", function () {
        const source = {
            $extensionMultiStream: {
                numDocs: numSourceDocs,
                isSelectionStage: true,
                advertiseScoreDetails: true,
            },
        };
        const res = coll
            .aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [source],
                                b: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                            },
                            normalization: "none",
                        },
                        combination: {method: "avg"},
                        scoreDetails: true,
                    },
                },
                {$project: {_id: 1, scoreDetails: {$meta: "scoreDetails"}}},
            ])
            .toArray();
        assertFusedResultHasDrmSourceDocs(res);
        // The DRM source docs (_id 0..n-1) must carry the source's scoreDetails marker; had DRM not
        // forwarded isScoreDetailsStage, fusion would use the empty-details fallback and drop it.
        // $scoreFusion nests each input pipeline's scoreDetails under a per-pipeline entry keyed by
        // inputPipelineName, so look for the marker specifically under the DRM source pipeline "a"
        // rather than anywhere in the blob.
        const withMarker = res.filter((d) => {
            const sourceEntry = (d.scoreDetails?.details ?? []).find(
                (entry) => entry.inputPipelineName === "a",
            );
            return JSON.stringify(sourceEntry ?? {}).includes(kScoreDetailsMarker);
        });
        assert.gte(
            withMarker.length,
            expectedSourceIds.length,
            "expected DRM source scoreDetails marker in fused output",
            {res},
        );
    });
});
