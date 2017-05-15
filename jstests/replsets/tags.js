load("jstests/replsets/libs/tags.js");

(function() {
    'use strict';

    let nodes = [{}, {}, {}, {}, {}];
    new TagsTest({nodes: nodes}).run();
}());
