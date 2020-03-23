// @tags: [fix_for_fcv_46]
(function() {
'use strict';

load("jstests/replsets/libs/tags.js");

var oldVersion = "last-stable";
var newVersion = "latest";
let nodes = [
    {binVersion: oldVersion},
    {binVersion: newVersion},
    {binVersion: oldVersion},
    {binVersion: newVersion},
    {binVersion: oldVersion}
];
new TagsTest({nodes: nodes, forceWriteMode: 'commands'}).run();
}());
