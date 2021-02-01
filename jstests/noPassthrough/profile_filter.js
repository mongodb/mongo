/*
 * Test the usage and behavior of the 'filter' profiler option.
 *
 * @tags: [requires_sharding, requires_replication]
 */
(function() {

load("jstests/libs/log.js");              // For findMatchingLogLine.
load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

function runTest(conn) {
    const db = conn.getDB("test");
    const coll = db.profile_filter;
    coll.drop();
    // mongoS supports slow-query log lines but not profiling. So if we are talking to mongoS, we
    // will avoid enabling profiling, or making assertions about profiling.
    const isMongos = FixtureHelpers.isMongos(db);

    // When you set a filter it appears in getProfilingLevel.
    const exampleFilter = {millis: {$gt: 100, $lt: 300}};
    const oldResponse = assert.commandWorked(db.setProfilingLevel(0, {filter: exampleFilter}));
    const response = assert.commandWorked(db.setProfilingLevel(0));
    assert.eq(response.filter, {$and: [{millis: {$gt: 100}}, {millis: {$lt: 300}}]});
    // But, slowms / sampleRate still appears in getProfilingLevel.
    assert.eq(response.slowms, oldResponse.slowms);
    assert.eq(response.sampleRate, oldResponse.sampleRate);
    // Since this may be confusing, a note also appears.
    assert.eq(
        oldResponse.note,
        'When a filter expression is set, slowms and sampleRate are not used for profiling and slow-query log lines.');
    assert.eq(
        response.note,
        'When a filter expression is set, slowms and sampleRate are not used for profiling and slow-query log lines.');

    // When you unset the filter it no longer appears in the response.
    assert.commandWorked(db.setProfilingLevel(0, {filter: 'unset'}));
    const response2 = assert.commandWorked(db.setProfilingLevel(0));
    assert(!response2.hasOwnProperty('filter'), response2);
    // The note is also gone.
    assert(!response2.hasOwnProperty('note'), response2);

    // Setting the filter to null is an error: we don't want people to assume that {filter: null}
    // unsets the filter, and we don't want people to assume {filter: null} is equivalent to not
    // specifying a new filter.
    assert.throws(() => db.setProfilingLevel(level, {filter: null}),
                  [],
                  "Expected an object, or the string 'unset'.");

    // While the filter is set, slow-query log lines ignore slowms and sampleRate.
    // For example:
    // 1. An always-true filter of {} will always log.
    //    This shows we don't AND all the settings together.
    assert.commandWorked(
        db.setProfilingLevel(isMongos ? 0 : 1, {filter: {}, slowms: 9999, sampleRate: 0}));
    const comment1 = 'profile_filter_example_1';
    assert.eq(0, coll.find().comment(comment1).itcount());

    let log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    assert(findMatchingLogLine(log, {msg: "Slow query", comment: comment1}),
           `No log line for comment: ${comment1}`);
    if (!isMongos) {
        assert(db.system.profile.findOne({'command.comment': comment1}),
               `No profiler entry for comment: ${comment1}`);
    }

    // 2. An always-false filter of {$expr: false} will never log.
    //    This shows we don't OR all the settings together.
    assert.commandWorked(db.setProfilingLevel(isMongos ? 0 : 1,
                                              {filter: {$expr: false}, slowms: -1, sampleRate: 1}));
    const comment2 = 'profile_filter_example_2';
    assert.eq(0, coll.find().comment(comment2).itcount());

    log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    assert.eq(null, findMatchingLogLine(log, {msg: "Slow query", comment: comment2}));
    if (!isMongos) {
        assert.eq(null, db.system.profile.findOne({'command.comment': comment2}));
    }

    coll.drop();
    assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({a: i}))));
    assert.commandWorked(coll.createIndex({a: 1}));
    if (!isMongos) {
        // The filter lets you express interesting predicates, such as SERVER-37308.
        assert.commandWorked(db.setProfilingLevel(
            isMongos ? 0 : 1,
            // This filter looks for queries with a high ratio of scanned to returned,
            // as an attempt to find query plans that aren't using indexes well.
            {filter: {$expr: {$lt: [10, {$divide: ["$docsExamined", "$nreturned"]}]}}}));
        const collscanComment = 'profile_filter_example_collscan';
        coll.find({a: 42}).hint({$natural: 1}).comment(collscanComment).itcount();
        const ixscanComment = 'profile_filter_example_ixscan';
        coll.find({a: 42}).hint({a: 1}).comment(ixscanComment).itcount();
        db.setProfilingLevel(0, {filter: 'unset'});

        // Only the collscan plan should be logged and profiled. The ixscan plan has a low ratio of
        // docsExamined/nreturned, so the filter does not select it.
        log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        assert(findMatchingLogLine(log, {msg: "Slow query", comment: collscanComment}));
        assert.eq(null, findMatchingLogLine(log, {msg: "Slow query", comment: ixscanComment}));
        assert(db.system.profile.findOne({'command.comment': collscanComment}));
        assert.eq(null, db.system.profile.findOne({'command.comment': ixscanComment}));
    }

    // The input to the filter has the same schema as system.profile, with a few exceptions.
    if (!isMongos) {
        const exampleProfileDoc = db.system.profile.findOne();
        const profilerFields = Object.keys(exampleProfileDoc);
        const expectedMissingFields = [
            // None for now!
        ];
        for (const field of profilerFields) {
            if (expectedMissingFields.includes(field))
                continue;

            // Set a filter that requires `field` to exist.
            assert.commandWorked(
                db.setProfilingLevel(isMongos ? 0 : 1, {filter: {[field]: {$exists: true}}}));
            const comment = 'profile_filter_input_has_field_' + field;
            assert.eq(1, coll.find().limit(1).comment(comment).itcount());
            // If the profile filter's input didn't contain `field`, then this operation wouldn't be
            // profiled.
            assert(db.system.profile.findOne({'command.comment': comment}),
                   `Profile filter doc was missing field: ${field}`);
        }
    }
    assert.commandWorked(db.setProfilingLevel(0, {filter: 'unset'}));
    assert.eq(0, db.getProfilingLevel());

    // To catch mistakes, it's not allowed for the filter to reference fields that don't exist.
    // setProfilingLevel throws an error, and prevents you from setting an invalid filter like this.
    assert.throws(() => db.setProfilingLevel(isMongos ? 0 : 1, {filter: {no_such_field: 23}}));
    assert.eq(0, db.getProfilingLevel());
    const response3 = db.setProfilingLevel(0);
    let expectedFields = ['was', 'slowms', 'sampleRate', 'ok'];
    if (isMongos) {
        expectedFields = [...expectedFields, '$clusterTime', 'operationTime'];
    }
    assert.eq(Object.keys(response3), expectedFields);

    // However, we can't catch every run-time error statically. For example, we make no attempt to
    // catch a type error statically. This kind of invalid filter will fail silently, because
    // logging the error would be too expensive, since the filter runs so frequently.
    assert.commandWorked(db.setProfilingLevel(isMongos ? 0 : 1, {filter: {$expr: {$add: [""]}}}));
    const invalidFilterComment = 'profile_filter_fails_at_runtime';
    assert.eq(1, coll.find().limit(1).comment(invalidFilterComment).itcount());
    if (!isMongos) {
        assert.eq(null, db.system.profile.findOne({'command.comment': invalidFilterComment}));
    }

    // It is allowed for the filter to depend on the entire document.
    assert.commandWorked(db.setProfilingLevel(
        isMongos ? 0 : 1, {filter: {$expr: {$gt: [3, {$size: {$objectToArray: "$$ROOT"}}]}}}));
    const entireDocFilterComment = 'profile_filter_depends_on_entire_document';
    assert.eq(1, coll.find().limit(1).comment(entireDocFilterComment).itcount());
    if (!isMongos) {
        assert.eq(null, db.system.profile.findOne({'command.comment': entireDocFilterComment}));
    }
}

{
    jsTest.log('Test standalone');
    const conn = MongoRunner.runMongod({});
    runTest(conn);
    MongoRunner.stopMongod(conn);
}

{
    jsTest.log('Test mongos');
    const st = ShardingTest({shards: 1, rs: {nodes: 1}, config: 1});
    runTest(st);
    st.stop();
}
})();
