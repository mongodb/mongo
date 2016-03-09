// tests to make sure that tag ranges are added/removed/updated successfully

function countTags(num, message) {
    assert.eq(s.config.tags.count(), num, message);
}

var s = new ShardingTest({name: "tag_range", shards: 2, mongos: 1});

// this set up is not required but prevents warnings in the remove
db = s.getDB("tag_range");

s.adminCommand({enableSharding: "test"});
s.ensurePrimaryShard('test', 'shard0001');
s.adminCommand({shardCollection: "test.tag_range", key: {_id: 1}});

assert.eq(1, s.config.chunks.count());

sh.addShardTag("shard0000", "a");

// add two ranges, verify the additions

sh.addTagRange("test.tag_range", {_id: 5}, {_id: 10}, "a");
sh.addTagRange("test.tag_range", {_id: 10}, {_id: 15}, "b");

countTags(2, "tag ranges were not successfully added");

// remove the second range, should be left with one

sh.removeTagRange("test.tag_range", {_id: 10}, {_id: 15}, "b");

countTags(1, "tag range not removed successfully");

// the additions are actually updates, so you can alter a range's max
sh.addTagRange("test.tag_range", {_id: 5}, {_id: 11}, "a");

assert.eq(11, s.config.tags.findOne().max._id, "tag range not updated successfully");

// add range min=max, verify the additions

try {
    sh.addTagRange("test.tag_range", {_id: 20}, {_id: 20}, "a");
} catch (e) {
    countTags(1, "tag range should not have been added");
}

// removeTagRange tests for tag ranges that do not exist

// Bad namespace
sh.removeTagRange("badns", {_id: 5}, {_id: 11}, "a");
countTags(1, "Bad namespace: tag range does not exist");

// Bad tag
sh.removeTagRange("test.tag_range", {_id: 5}, {_id: 11}, "badtag");
countTags(1, "Bad tag: tag range does not exist");

// Bad min
sh.removeTagRange("test.tag_range", {_id: 0}, {_id: 11}, "a");
countTags(1, "Bad min: tag range does not exist");

// Bad max
sh.removeTagRange("test.tag_range", {_id: 5}, {_id: 12}, "a");
countTags(1, "Bad max: tag range does not exist");

// Invalid namesapce
sh.removeTagRange(35, {_id: 5}, {_id: 11}, "a");
countTags(1, "Invalid namespace: tag range does not exist");

// Invalid tag
sh.removeTagRange("test.tag_range", {_id: 5}, {_id: 11}, 35);
countTags(1, "Invalid tag: tag range does not exist");

// Invalid min
sh.removeTagRange("test.tag_range", 35, {_id: 11}, "a");
countTags(1, "Invalid min: tag range does not exist");

// Invalid max
sh.removeTagRange("test.tag_range", {_id: 5}, 35, "a");
countTags(1, "Invalid max: tag range does not exist");

s.stop();
