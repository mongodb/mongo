(function() {
    'use strict';

    load("jstests/replsets/libs/tags.js");

    var nodes = [{}, {}, {}, {}, {}];
    new TagsTest({nodes: nodes}).run();
}());
