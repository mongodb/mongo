/**
 * There is no limit for the total size or the number of unique terms for text index.
 *
 * Each insert involves creating big text index which runs slowly enough to get
 * killed by a stepdown before any attempt can finish on slower variants.
 * @tags: [does_not_support_stepdowns]
 */
(function() {
"use strict";

var t = db.text_index_limits;
t.drop();

assert.commandWorked(t.createIndex({comments: "text"}));

// 1. Test number of unique terms exceeds 400,000
let commentsWithALotOfUniqueWords = "";
// 26^4 = 456,976 > 400,000
for (let ch1 = 97; ch1 < 123; ch1++) {
    for (let ch2 = 97; ch2 < 123; ch2++) {
        for (let ch3 = 97; ch3 < 123; ch3++) {
            for (let ch4 = 97; ch4 < 123; ch4++) {
                let word = String.fromCharCode(ch1, ch2, ch3, ch4);
                commentsWithALotOfUniqueWords += word + " ";
            }
        }
    }
}
assert.commandWorked(db.runCommand(
    {insert: t.getName(), documents: [{_id: 1, comments: commentsWithALotOfUniqueWords}]}));

// 2. Test total size of index keys for unique terms exceeds 4MB

// 26^3 = 17576 < 400,000
let prefix = "a".repeat(400);
let commentsWithWordsOfLargeSize = "";
for (let ch1 = 97; ch1 < 123; ch1++) {
    for (let ch2 = 97; ch2 < 123; ch2++) {
        for (let ch3 = 97; ch3 < 123; ch3++) {
            let word = String.fromCharCode(ch1, ch2, ch3);
            commentsWithWordsOfLargeSize += prefix + word + " ";
        }
    }
}
assert.commandWorked(db.runCommand(
    {insert: t.getName(), documents: [{_id: 2, comments: commentsWithWordsOfLargeSize}]}));
}());
