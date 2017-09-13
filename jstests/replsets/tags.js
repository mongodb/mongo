(function() {
    'use strict';

    load("jstests/replsets/libs/tags.js");

    let nodes = [{}, {}, {}, {}, {}];
    new TagsTest({nodes: nodes}).run();
}());
