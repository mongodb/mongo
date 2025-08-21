import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

// validate test execution and ordering among sync and async content

const log = jsTest.log.info;

before(async () => log("before1"));
before(() => log("before2"));

beforeEach(() => log("--beforeEach1"));
beforeEach(async () => log("--beforeEach2"));

afterEach(async () => log("--afterEach1"));
afterEach(() => log("--afterEach2"));

after(() => log("after1"));
after(async () => log("after2"));

it("test1", () => log("----test1"));
it("test2", async () => log("----test2"));

describe("describe", function () {
    before(() => log("----describe before1"));
    before(async () => log("----describe before2"));

    beforeEach(async () => log("------describe beforeEach1"));
    beforeEach(() => log("------describe beforeEach2"));

    afterEach(() => log("------describe afterEach1"));
    afterEach(async () => log("------describe afterEach2"));

    after(async () => log("----describe after1"));
    after(() => log("----describe after2"));

    it("test3", async () => log("--------test3"));
    it("test4", () => log("--------test4"));
});

it("test5", () => log("----test5"));
it("test6", async () => log("----test6"));
