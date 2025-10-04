import {TagsTest} from "jstests/replsets/libs/tags.js";

let oldVersion = "last-lts";
let newVersion = "latest";
let nodes = [
    {binVersion: oldVersion},
    {binVersion: newVersion},
    {binVersion: oldVersion},
    {binVersion: newVersion},
    {binVersion: oldVersion},
];
new TagsTest({nodes: nodes}).run();
