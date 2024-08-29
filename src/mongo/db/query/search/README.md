# Welcome to search - we're glad you're here!

This README serves as a landing page for all search\* related documentation to make these resources easier to discover for new engineers. As you contribute new documentation for search features, please don't forget to link it below and kindly add a reverse link to this README in your new doc. Happy knowledge sharing!

\*For the purposes of this README, search is a shorthand to refer to all mongot pipeline stages, so $search/$vectorSearch/$searchMeta.

## Technical Details

To read about the high-level technical implementation of $search and $searchMeta, please check out [search_technical_overview.md](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/search/search_technical_overview.md).

To read about a high-level overview of $vectorSearch, please check out [vectorSearch_technical_overview.md](https://github.com/10gen/mongo/master/src/mongo/db/pipeline/search/vectorSearch_technical_overview.md)

## Testing

Testing search features is a bit different than other aggregation stages! You will fall into one of three categories:

1. If your search feature requires currently in-progress/incomplete changes to 10gen/mongot, you will need to test your mongod changes with mongot-mock. To find out more about writing a jstest that uses mongot-mock, please follow this [wiki](https://wiki.corp.mongodb.com/display/~zixuan.zhuang@mongodb.com/How+to+run+%24search+locally+using+Mongot+Mock). You can peruse more examples in [jstests/with_mongot/mongotmock/](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/mongotmock/).

2. If your search feature is fully supported on the relaease mongot binary (eg the mongot required changes have been merged to 10gen/mongot), you can write a standard jstest for your search feature. However you will need a mongot binary on your machine to run your jstest locally. To learn how to acquire a mongot binary locally and get more details on end-to-end testing search features locally and on evergreen, please checkout [mongot_testing_instructions.md](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/e2e/mongot_testing_instructions.md).

3. If your search feature is supported on the latest mongot binary but not the release mongot binary, you are currently in a spooky no-man's land. You cannot currently merge e2e tests in this state. But have no fear, SERVER-93738 will address this and soon you will be able to tag your test so it is never run with an incompatible mongot release binary on evergreen.

TODO SERVER-93738: clarify steps in third bullet once this ticket has been implemented.

Regardless of the category you find yourself in, you are required to run all e2e tests defined on both 10gen/mongod and 10gen/mongot repos. To learn how to run all cross-repo e2e tests, please check out [jstests/with_mongot/cross_repo_testing_requirements.md](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/e2e/cross_repo_testing_requirements.md).
