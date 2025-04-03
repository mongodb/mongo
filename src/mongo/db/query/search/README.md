# Welcome to search - we're glad you're here!

This README serves as a landing page for all search\* related documentation to make these resources easier to discover for new engineers. As you contribute new documentation for search features, please don't forget to link it below and kindly add a reverse link to this README in your new doc. Happy knowledge sharing!

\*For the purposes of this README, search is a shorthand to refer to all mongot pipeline stages, so $search/$vectorSearch/$searchMeta.

## Technical Details

To read about the high-level technical implementation of $search and $searchMeta, please check out [search_technical_overview.md](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/search/search_technical_overview.md).

To read about a high-level overview of $vectorSearch, please check out [vectorSearch_technical_overview.md](https://github.com/10gen/mongo/blob/master/src/mongo/db/pipeline/search/vectorSearch_technical_overview.md)

## Testing

Testing search features is a bit different than other aggregation stages! You will fall into one of three categories:

1. If your search feature requires currently in-progress/incomplete changes to 10gen/mongot, you will need to test your mongod changes with mongot-mock. To find out more about writing a jstest that uses mongot-mock, please follow this [wiki](https://wiki.corp.mongodb.com/display/~zixuan.zhuang@mongodb.com/How+to+run+%24search+locally+using+Mongot+Mock). You can peruse more examples in [jstests/with_mongot/mongotmock/](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/mongotmock/).

2. If your search feature is fully supported on the release mongot binary (eg the mongot required changes have been merged to 10gen/mongot and it's been released), you can write a standard jstest for your search feature. However you will need a mongot binary on your machine to run your jstest locally. To learn how to acquire a mongot binary locally and get more details on end-to-end testing search features locally and on evergreen, please checkout [mongot_testing_instructions.md](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/e2e/mongot_testing_instructions.md).

3. If your search feature is supported on the latest mongot binary (from 10gen/mongot) but not on the released version, you can use tags to ensure your test only runs on the appropriate version. Mongot versions follow the format X.Y.Z, where X is the API version, Y updates for functionality changes, and Z updates for bug fixes. You can use the following tag formats to disallow tests from running on the released version of mongot.

   - `requires_mongot_X` for a future API version
   - `requires_mongot_X_Y` for feature updates
   - `requires_mongot_X_Y_Z` for specific bug fix versions

   For example:

   - If the latest version is `1.39.0-67-g935bf894a` and the release version is `1.38.1`, use `requires_mongot_1_39` as the tag for your test.
   - If the latest version is `1.39.3` and the release version is `1.39.2`, use `requires_mongot_1_39_3` as the tag for your test.

   Once the released version matches or exceeds the tag on the test, the test will also run for the released version.

Regardless of the category you find yourself in, you are required to run all e2e tests defined on both 10gen/mongod and 10gen/mongot repos. To learn how to run all cross-repo e2e tests, please check out [jstests/with_mongot/cross_repo_testing_requirements.md](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/cross_repo_testing_requirements.md).

## Hybrid Search

Hybrid Search encompasses two possible stages: `$rankFusion` and `$scoreFusion`. Both of these stages accept one or more "input pipelines" (each of which is its own valid query) that search for documents in a single collection (without modifying them). Then, the hybrid search stage combines the results from all the input pipelines into a single ordered results set, based on some ranking or scoring methodology that factors in the user-set weight (influence) of each input pipeline. $rankFusion uses the Reciprocal Rank Fusion algorithm while $scoreFusion relies on the user's custom score combination configuration/logic. See [this docs page](https://dochub.mongodb.org/core/rank-fusion) to learn more about hybrid search.

### scoreDetails Technical Overview

The hybrid search stages ($rankFusion and $scoreFusion) allow a user to specify whether that stage's scoreDetails metadata should be set (note that the metadata is set at the document level). The two other stages that also support scoreDetails functionality are $score and $search. See the Phase 3 section below to understand the difference in scoreDetails structure between the hybrid search stages and non-hybrid search stages ($search and $score). scoreDetails, at a high level, functions like an $explain in that it provides information about how each document's score was calculated under the hood. This in turn helps the user understand the resulting order of documents. To learn more about the scoreDetails field and its subfields for hybrid search stages, please refer to either the $rankFusion or $scoreFusion docs page (coming soon. in the meantime, please use the [technical design document for hybrid search](https://docs.google.com/document/d/1WgwYPE64T0E-BChqLdGPw1wo5z0ZYrrcbEASOoFl81M/edit?tab=t.0#heading=h.2s9zdv6a57gn)).

The scoreDetails field is built up in 4 phases.

1. **Phase 1**
   The first phase consists of adding scoreDetails to each input pipeline's set of resulting documents. These scoreDetails will be added as a document field with the name "<input_pipeline_name>\_scoreDetails" (ex: for a pipeline called “searchPipe”, the added field’s name would be searchPipe_scoreDetails). The input pipeline's scoreDetails field will be a BSONObj that generates one of the three following values for scoreDetails:
   - `$meta: "scoreDetails` if the input pipeline has incoming scoreDetails (for example from a $search), then it is set to that value directly.
   - `{value: {$meta: "score"}, details: []}` if the input pipeline generates score metadata, but not scoreDetails, then the incoming score is set, and the details array is set to empty
   - `{details: []}` if the input pipeline generates neither score nor scoreDetails metadata

See the [addScoreDetails function](https://github.com/10gen/mongo/blob/f3eed1b0cc3860aae1d524de205f7fe3b28e2a3d/src/mongo/db/pipeline/document_source_hybrid_scoring_util.cpp#L765) for the exact implementation.

2. **Phase 2**
   The second phase starts after all input pipelines have executed and begins the process of combining all the results into a total ranked set. This consists of grouping the newly added scoreDetails fields (remember that there’s 1 for each input pipeline) across all the documents. The grouping is needed because after processing the N input pipelines, there can be up to N repeats of the same document. Each document will have its own fields and any added fields for that pipeline (ex: the searchPipe_scoreDetails is an example of an added field specific to the searchPipe pipeline. Only 1 of the N repeated documents will have this field.) The result of this step is that each unique document should have a document field named <input_pipeline_name>\_scoreDetails, for each input pipeline this document appeared in.

See the [groupEachScore function](https://github.com/10gen/mongo/blob/f3eed1b0cc3860aae1d524de205f7fe3b28e2a3d/src/mongo/db/pipeline/document_source_rank_fusion.cpp#L461) for the exact implementation.

3. **Phase 3**
   The third phase calculates a new field called `calculatedScoreDetails` per document that combines all the input pipeline's scoreDetails into an array, with one entry per input pipeline scoreDetails. Each entry in the array contains the following scoreDetails subfields: `input pipeline name`, `rank, weight`, and `details` (`details` contains the value of the input pipeline's `scoreDetails`). The two optional subfields are `value` (if the pipeline sets the score metadata field, then this is the value of `$meta: score`) and `description` (if the input pipeline generates a `description` as part of `scoreDetails`, `description` contains that value).

   The non-hybrid search stages ($search and $score) only set the outermost scoreDetails fields (`value`, `description`, and `details`);

See the [calculatedScoreDetails function](https://github.com/10gen/mongo/blob/f3eed1b0cc3860aae1d524de205f7fe3b28e2a3d/src/mongo/db/pipeline/document_source_hybrid_scoring_util.cpp#L120) for the exact implementation.

4. **Phase 4**
   The fourth phase simply sets the `scoreDetails` metadata which represents the final scoreDetails value that gets returned to the user in the final results. Its fields include the `value` field which is the calculated score (for example: in the case of $rankFusion, the Reciprocal Rank Fusion score), the `description` field which is the stage’s description for how the score was calculated, and the `details` field which is the fully assembled calculatedScoreDetails from the previous step.

See the [buildScoreAndMergeStages function](https://github.com/10gen/mongo/blob/f3eed1b0cc3860aae1d524de205f7fe3b28e2a3d/src/mongo/db/pipeline/document_source_rank_fusion.cpp#L565) for the exact implementation.
