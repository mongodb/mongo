# Welcome to search - we're glad you're here!

This README serves as a landing page for all search\* related documentation to make these resources easier to discover for new engineers. As you contribute new documentation for search features, please don't forget to link it below and kindly add a reverse link to this README in your new doc. Happy knowledge sharing!

\*For the purposes of this README, search is a shorthand to refer to all mongot pipeline stages, so $search/$vectorSearch/$searchMeta.

## Technical Details

To read about the high-level technical implementation of $search and $searchMeta, please check out [search_technical_overview.md](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/search/search_technical_overview.md).

To read about a high-level overview of $vectorSearch, please check out [vectorSearch_technical_overview.md](https://github.com/10gen/mongo/master/src/mongo/db/pipeline/search/vectorSearch_technical_overview.md)

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
