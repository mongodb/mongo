/**
 * TODO SERVER-52599: to remove this tag.
 * @tags: [requires_wiredtiger]
 */
(function() {
'use strict';

load("jstests/replsets/libs/tags.js");

let nodes = [{}, {}, {}, {}, {}];
new TagsTest({nodes: nodes}).run();
}());
