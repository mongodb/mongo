// tests to make sure that tag ranges are added/removed/updated successfully
(function() {
    'use strict';

    const st = new ShardingTest({shards: 2, mongos: 1});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.tag_range', key: {_id: 1}}));

    function countTags(num, message) {
        assert.eq(st.config.tags.count(), num, message);
    }

    assert.eq(1, st.config.chunks.count({"ns": "test.tag_range"}));

    st.addShardTag('shard0000', 'a');
    st.addShardTag('shard0000', 'b');
    st.addShardTag('shard0000', 'c');

    // add two ranges, verify the additions

    assert.commandWorked(st.addTagRange('test.tag_range', {_id: 5}, {_id: 10}, 'a'));
    assert.commandWorked(st.addTagRange('test.tag_range', {_id: 10}, {_id: 15}, 'b'));

    countTags(2, 'tag ranges were not successfully added');

    // remove the second range, should be left with one

    assert.commandWorked(st.removeTagRange('test.tag_range', {_id: 10}, {_id: 15}, 'b'));

    countTags(1, 'tag range not removed successfully');

    // add range min=max, verify the additions

    try {
        st.addTagRange('test.tag_range', {_id: 20}, {_id: 20}, 'a');
    } catch (e) {
        countTags(1, 'tag range should not have been added');
    }

    // Test that a dotted path is allowed in a tag range if it includes the shard key.
    assert.commandWorked(
        st.s0.adminCommand({shardCollection: 'test.tag_range_dotted', key: {"_id.a": 1}}));
    assert.commandWorked(st.addTagRange('test.tag_range_dotted', {"_id.a": 5}, {"_id.a": 10}, 'c'));
    countTags(2, 'Dotted path tag range not successfully added.');

    assert.commandWorked(
        st.removeTagRange('test.tag_range_dotted', {"_id.a": 5}, {"_id.a": 10}, 'c'));
    assert.commandFailed(st.addTagRange('test.tag_range_dotted', {"_id.b": 5}, {"_id.b": 10}, 'c'));
    countTags(1, 'Incorrectly added tag range.');

    // Test that ranges on embedded fields of the shard key are not allowed.
    assert.commandFailed(
        st.addTagRange('test.tag_range_dotted', {_id: {a: 5}}, {_id: {a: 10}}, 'c'));
    countTags(1, 'Incorrectly added embedded field tag range');

    // removeTagRange tests for tag ranges that do not exist

    // Bad namespace
    assert.commandFailed(st.removeTagRange('badns', {_id: 5}, {_id: 11}, 'a'));
    countTags(1, 'Bad namespace: tag range does not exist');

    // Bad tag
    assert.commandWorked(st.removeTagRange('test.tag_range', {_id: 5}, {_id: 11}, 'badtag'));
    countTags(1, 'Bad tag: tag range does not exist');

    // Bad min
    assert.commandWorked(st.removeTagRange('test.tag_range', {_id: 0}, {_id: 11}, 'a'));
    countTags(1, 'Bad min: tag range does not exist');

    // Bad max
    assert.commandWorked(st.removeTagRange('test.tag_range', {_id: 5}, {_id: 12}, 'a'));
    countTags(1, 'Bad max: tag range does not exist');

    // Invalid namesapce
    assert.commandFailed(st.removeTagRange(35, {_id: 5}, {_id: 11}, 'a'));
    countTags(1, 'Invalid namespace: tag range does not exist');

    // Invalid tag
    assert.commandWorked(st.removeTagRange('test.tag_range', {_id: 5}, {_id: 11}, 35));
    countTags(1, 'Invalid tag: tag range does not exist');

    // Invalid min
    assert.commandFailed(st.removeTagRange('test.tag_range', 35, {_id: 11}, 'a'));
    countTags(1, 'Invalid min: tag range does not exist');

    // Invalid max
    assert.commandFailed(st.removeTagRange('test.tag_range', {_id: 5}, 35, 'a'));
    countTags(1, 'Invalid max: tag range does not exist');

    st.stop();
})();
