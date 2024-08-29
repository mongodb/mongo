Server engineers working on search, changeStreams, or any code that interacts with these features on v8.1+ branches are required to run all mongot integration tests defined on 10gen/mongod and 10gen/mongot before committing.

The simplest way to do this is to use this command to create your evergreen patch:

    evergreen patch -p mongodb-mongo-master --trigger-alias search-integration --alias mongot_e2e_tests

This will auto select the e2e tests defined on both repos.

If your evergreen patch shows your changes failed a search e2e test defined on 10gen/mongod, you can follow [these instructions](https://github.com/10gen/mongo/blob/master/jstests/with_mongot/e2e/mongot_testing_instructions.md) for running that test locally on your VM.

If your evergreen patch shows your changes failed an e2e test defined on 10gen/mongot, please reach out in #search-query-engineering for assistance from mongot engineers in translating and addressing the failure.

### Didn't Find What You're Looking For?

Visit [the landing page](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/search/README.md) for all $search/$vectorSearch/$searchMeta related documentation for server contributors.
