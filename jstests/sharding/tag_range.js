// tests to make sure that tag ranges are added/removed/updated successfully
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.tag_range', key: {_id: 1}}));

    function countTags(num, message) {
        assert.eq(st.config.tags.count(), num, message);
    }

    assert.eq(1, st.config.chunks.count());

    st.addShardTag('shard0000', 'a');
    st.addShardTag('shard0000', 'b');

    // add two ranges, verify the additions

    st.addTagRange('test.tag_range', {_id: 5}, {_id: 10}, 'a');
    st.addTagRange('test.tag_range', {_id: 10}, {_id: 15}, 'b');

    countTags(2, 'tag ranges were not successfully added');

    // remove the second range, should be left with one

    st.removeTagRange('test.tag_range', {_id: 10}, {_id: 15}, 'b');

    countTags(1, 'tag range not removed successfully');

    // add range min=max, verify the additions

    try {
        st.addTagRange('test.tag_range', {_id: 20}, {_id: 20}, 'a');
    } catch (e) {
        countTags(1, 'tag range should not have been added');
    }

    // removeTagRange tests for tag ranges that do not exist

    // Bad namespace
    st.removeTagRange('badns', {_id: 5}, {_id: 11}, 'a');
    countTags(1, 'Bad namespace: tag range does not exist');

    // Bad tag
    st.removeTagRange('test.tag_range', {_id: 5}, {_id: 11}, 'badtag');
    countTags(1, 'Bad tag: tag range does not exist');

    // Bad min
    st.removeTagRange('test.tag_range', {_id: 0}, {_id: 11}, 'a');
    countTags(1, 'Bad min: tag range does not exist');

    // Bad max
    st.removeTagRange('test.tag_range', {_id: 5}, {_id: 12}, 'a');
    countTags(1, 'Bad max: tag range does not exist');

    // Invalid namesapce
    st.removeTagRange(35, {_id: 5}, {_id: 11}, 'a');
    countTags(1, 'Invalid namespace: tag range does not exist');

    // Invalid tag
    st.removeTagRange('test.tag_range', {_id: 5}, {_id: 11}, 35);
    countTags(1, 'Invalid tag: tag range does not exist');

    // Invalid min
    st.removeTagRange('test.tag_range', 35, {_id: 11}, 'a');
    countTags(1, 'Invalid min: tag range does not exist');

    // Invalid max
    st.removeTagRange('test.tag_range', {_id: 5}, 35, 'a');
    countTags(1, 'Invalid max: tag range does not exist');

    st.stop();
})();
