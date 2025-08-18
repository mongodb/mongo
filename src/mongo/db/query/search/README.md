# Welcome to search - we're glad you're here!

This README serves as a landing page for all search\* related documentation to make these resources easier to discover for new engineers. As you contribute new documentation for search features, please don't forget to link it below and kindly add a reverse link to this README in your new doc. Happy knowledge sharing!

> [!NOTE]
> For the purposes of this README, search is a shorthand to refer to all mongot pipeline stages, so $search/$vectorSearch/$searchMeta.

## Technical Details

To read about the high-level technical implementation of $search and $searchMeta, please check out [search_technical_overview.md](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/search/search_technical_overview.md).

To read about a high-level overview of $vectorSearch, please check out [vectorSearch_technical_overview.md](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/vectorSearch_technical_overview.md)

## Testing

Testing search features is a bit different than other aggregation stages! You will fall into one of three categories:

1. If your search feature requires currently in-progress/incomplete changes to 10gen/mongot, you will need to test your mongod changes with mongot-mock. To find out more about writing a jstest that uses mongot-mock, please follow this [wiki](https://wiki.corp.mongodb.com/display/~zixuan.zhuang@mongodb.com/How+to+run+%24search+locally+using+Mongot+Mock). You can peruse more examples in [jstests/with_mongot/mongotmock/](https://github.com/mongodb/mongo/blob/master/jstests/with_mongot/mongotmock/).

2. If your search feature is fully supported on the release mongot binary (eg the mongot required changes have been merged to 10gen/mongot and it's been released), you can write a standard jstest for your search feature. However you will need a mongot binary on your machine to run your jstest locally. To learn how to acquire a mongot binary locally and get more details on end-to-end testing search features locally and on evergreen, please checkout [mongot_testing_instructions.md](https://github.com/mongodb/mongo/blob/master/jstests/with_mongot/e2e/mongot_testing_instructions.md).

3. If your search feature is supported on the latest mongot binary (from 10gen/mongot) but not on the released version, you can use tags to ensure your test only runs on the appropriate version. Mongot versions follow the format X.Y.Z, where X is the API version, Y updates for functionality changes, and Z updates for bug fixes. You can use the following tag formats to disallow tests from running on the released version of mongot.

   - `requires_mongot_X` for a future API version
   - `requires_mongot_X_Y` for feature updates
   - `requires_mongot_X_Y_Z` for specific bug fix versions

   For example:

   - If the latest version is `1.39.0-67-g935bf894a` and the release version is `1.38.1`, use `requires_mongot_1_39` as the tag for your test.
   - If the latest version is `1.39.3` and the release version is `1.39.2`, use `requires_mongot_1_39_3` as the tag for your test.

   Once the released version matches or exceeds the tag on the test, the test will also run for the released version.

Regardless of the category you find yourself in, you are required to run all e2e tests defined on both 10gen/mongod and 10gen/mongot repos. To learn how to run all cross-repo e2e tests, please check out [jstests/with_mongot/cross_repo_testing_requirements.md](https://github.com/mongodb/mongo/blob/master/jstests/with_mongot/cross_repo_testing_requirements.md).

## Hybrid Search

Hybrid Search encompasses two possible stages: `$rankFusion` and `$scoreFusion`. Both of these stages accept one or more "input pipelines" (each of which is its own valid query) that search for documents in a single collection (without modifying them). Then, the hybrid search stage combines the results from all the input pipelines into a single ordered results set, based on some ranking or scoring methodology that factors in the user-set weight (influence) of each input pipeline. $rankFusion uses the Reciprocal Rank Fusion algorithm while $scoreFusion relies on the user's custom score combination configuration/logic. See [this docs page](https://dochub.mongodb.org/core/rank-fusion) to learn more about hybrid search.

> [!NOTE]
> Hybrid Search stages with and without mongot input pipelines can run on views but not in view definitions. For more information about how mongot stages run on views, see this page.

### scoreDetails Technical Overview

The hybrid search stages ($rankFusion and $scoreFusion) allow a user to specify whether that stage's scoreDetails metadata should be set (note that the metadata is set at the document level). The two other stages that also support scoreDetails functionality are $score and $search. See the Phase 3 section below to understand the difference in scoreDetails structure between the hybrid search stages and non-hybrid search stages ($search and $score). scoreDetails, at a high level, functions like an $explain in that it provides information about how each document's score was calculated under the hood. This in turn helps the user understand the resulting order of documents. To learn more about the scoreDetails field and its subfields for hybrid search stages, please refer to either the $rankFusion or $scoreFusion docs page (coming soon).

The scoreDetails field is built up in 4 phases.

1. **Phase 1**: Add scoreDetails to each input pipeline's set of resulting documents.
   These scoreDetails will be added as a document field with the name **`<INTERNAL_FIELDS>`.`<input_pipeline_name>`\_scoreDetails** (ex: for a pipeline called **_searchPipe_**, the added field’s name would be **<`INTERNAL_FIELDS`>._searchPipe_\_scoreDetails**) where **`INTERNAL_FIELDS`** is the hybrid search stage's internal fields name. The input pipeline's scoreDetails field will be a BSONObj that generates one of the following values for scoreDetails:

   | Value                                    | Hybrid Search Stage       | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
   | ---------------------------------------- | ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `{$meta: "scoreDetails"}`                | $rankFusion, $scoreFusion | If the input pipeline has incoming scoreDetails (for example from a $search), then it is set to that value directly.                                                                                                                                                                                                                                                                                                                                                    |
   | `{value: {$meta: "score"}, details: []}` | $rankFusion               | If the input pipeline generates score metadata, but not scoreDetails, then the incoming score is set, and the details array is set to empty. Note that all $scoreFusion input pipelines generate score metadata so each input pipeline's score value will always be [saved under a field called `inputPipelineRawScore`](https://github.com/mongodb/mongo/blob/e781072e0060950728580cda91fa2eb9a6c67ca5/src/mongo/db/pipeline/document_source_score_fusion.cpp#L834-L836) |
   | `{details: []}`                          | $rankFusion, $scoreFusion | If the input pipeline generates no scoreDetails metadata (and no score metadata in the case of $rankFusion)                                                                                                                                                                                                                                                                                                                                                             |

   See [$rankFusion's addInputPipelineScoreDetails function](https://github.com/mongodb/mongo/blob/e781072e0060950728580cda91fa2eb9a6c67ca5/src/mongo/db/pipeline/document_source_rank_fusion.cpp#L258) and [$scoreFusion's addInputPipelineScoreDetails function](https://github.com/mongodb/mongo/blob/e781072e0060950728580cda91fa2eb9a6c67ca5/src/mongo/db/pipeline/document_source_score_fusion.cpp#L286C38-L286C66) for the exact implementations.

2. **Phase 2**: Combine all results into a total ranked ($rankFusion) or scored ($scoreFusion) set now that all input pipelines have executed.
   This consists of grouping the newly added scoreDetails fields (remember that there’s 1 for each input pipeline) across all the documents. The grouping is needed because after processing the N input pipelines, there can be up to N repeats of the same document. Each document will have its own fields and any added fields for that pipeline (ex: the **<`INTERNAL_FIELDS`>._searchPipe_\_scoreDetails** is an example of an added field specific to the **_searchPipe_** pipeline. Only 1 of the N repeated documents will have this field.) The result of this step is that each unique document should have a document field named **`<INTERNAL_FIELDS>`.`<input_pipeline_name>`\_scoreDetails**, for each input pipeline this document appeared in.

   See the [groupDocsByIdAcrossInputPipeline function](https://github.com/mongodb/mongo/blob/f794e5e8af84765edca38337ea7713964031a782/src/mongo/db/pipeline/document_source_hybrid_scoring_util.cpp#L445) for the exact implementation.

3. **Phase 3**
   The third phase calculates a new field called `calculatedScoreDetails` per document that combines all the input pipeline's scoreDetails into an array, with one scoreDetails entry per input pipeline. Each entry in the array contains the following scoreDetails subfields:

   | $rankFusion         | $scoreFusion            | Optional              | Description                                                                                                                                                                                                                     |
   | ------------------- | ----------------------- | --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `inputPipelineName` | `inputPipelineName`     | No                    | Ex: `inputPipelineName`: `searchPipe`.                                                                                                                                                                                          |
   | `rank`              | `inputPipelineRawScore` | No                    | $rankFusion: `rank` can have the value `"NA"` if the document wasn't output from that input pipeline. $scoreFusion: `inputPipelineRawScore` always represents the score metadata value set by the corresponding input pipeline. |
   | `weight`            | `weight`                | Yes, for $rankFusion. | Ex: `weight`: `5` (whatever the weight value set for that input pipeline was). $rankFusion: Note that if no document was ouput for that input pipeline (`rank` is `"NA"`), then the weight is omitted.                          |
   | `value`             | `value`                 | Yes, for $rankFusion. | $rankFusion: If the pipeline sets the score metadata field, then this is the value of `$meta: score`. $scoreFusion: this is the score value generated by the potentially normalized `inputPipelineRawScore`*`weight`.           |
   | `description`       | `description`           | Yes                   | If the input pipeline generates a `description` as part of `scoreDetails`, then `description` contains that value.                                                                                                              |
   | `details`           | `details`               | No                    | `details` is either empty `[]` or contains the value of the input pipeline's `scoreDetails` assuming `scoreDetails: true` for that input pipeline (ex: $search or $score).                                                      |

   The non-hybrid search stages ($search and $score) set the scoreDetails fields (`value`, `description`, and `details`) at a minimum. These fields and their values will be found under the `details` field for the given input pipeline, assuming $search/$score had scoreDetails enabled (`scoreDetails: true`).

   **Visual Structure of a Full scoreDetails with a $search Input Pipeline:**

   ```
      inputPipelineName: ...,
      rank or inputPipelineRawScore: ...,
      weight: ...,
      value: ...,
      description: ...,
      details: {
         value: ...,
         description: ...,
         details: ...
      }
   ```

   **Visual Structure of a Full scoreDetails with a $score Input Pipeline:**

   ```
      inputPipelineName: ...,
      rank or inputPipelineRawScore: ...,
      weight: ...,
      value: ...,
      description: ...,
      details: {
         value: ..., // The potentially normalized rawScore * weight.
         description: ...,
         rawScore: ..., // The unweighted raw (not normalized) score.
         normalization: ..., // One of the following normalization values: "none", "sigmoid", "minMaxScaler".
         weight: ...,
         expression: ..., // The stringified value of $score.score's expression.
         details: []
      }
   ```

   See the [constructCalculatedFinalScoreDetails function](https://github.com/mongodb/mongo/blob/f794e5e8af84765edca38337ea7713964031a782/src/mongo/db/pipeline/document_source_hybrid_scoring_util.cpp#L640) for the exact implementation.

4. **Phase 4**
   The fourth phase simply sets the `scoreDetails` metadata which represents the final scoreDetails value that gets returned to the user in the final results.

   | $rankFusion   | $scoreFusion                                                                                                            | Description                                                                                                                                                                                                                                                       |
   | ------------- | ----------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `value`       | `value`                                                                                                                 | The calculated score. $rankFusion: the sum of the weighted [Reciprocal Rank Fusion](coming soon) scores across the input pipelines. $scoreFusion: the average of or the user-specified combination expression for the weighted scores across the input pipelines. |
   | `description` | `description`                                                                                                           | The stage’s description for how the document's final score was calculated.                                                                                                                                                                                        |
   |               | `normalization`                                                                                                         | $scoreFusion: One of the following normalization options (_"none"_, _"sigmoid"_, _"minMaxScaler"_).                                                                                                                                                               |
   |               | `combination: {method: "avg"}` OR `combination: {method: "custom expression", expression: {$const: "{ string: {...}}}}` | $scoreFusion: If the combination method is _"avg"_, indicate that. If a custom combination expression was specified, indicate that and output the stringified expression.                                                                                         |
   | `details`     | `details`                                                                                                               | The fully assembled calculatedScoreDetails from the previous step.                                                                                                                                                                                                |

   See [$rankFusion's buildScoreAndMergeStages function](https://github.com/mongodb/mongo/blob/f794e5e8af84765edca38337ea7713964031a782/src/mongo/db/pipeline/document_source_rank_fusion.cpp#L504) and [$scoreFusion's buildScoreAndMergeStages function](https://github.com/mongodb/mongo/blob/f794e5e8af84765edca38337ea7713964031a782/src/mongo/db/pipeline/document_source_score_fusion.cpp#L5846) for the exact implementations.
