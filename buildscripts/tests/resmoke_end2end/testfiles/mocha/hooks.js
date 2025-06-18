
import {
    after,
    afterEach,
    before,
    beforeEach,
    describe,
    it,
} from "jstests/libs/mochalite.js";

// validate test execution and ordering

const log = jsTest.log.info;

before(() => log("before1"));
before(() => log("before2"));

beforeEach(() => log("--beforeEach1"));
beforeEach(() => log("--beforeEach2"));

afterEach(() => log("--afterEach1"));
afterEach(() => log("--afterEach2"));

after(() => log("after1"));
after(() => log("after2"));

it("test1", () => log("----test1"));
it("test2", () => log("----test2"));

describe("describe", function() {
    before(() => log("----describe before1"));
    before(() => log("----describe before2"));

    beforeEach(() => log("------describe beforeEach1"));
    beforeEach(() => log("------describe beforeEach2"));

    afterEach(() => log("------describe afterEach1"));
    afterEach(() => log("------describe afterEach2"));

    after(() => log("----describe after1"));
    after(() => log("----describe after2"));

    it("test3", () => log("--------test3"));
    it("test4", () => log("--------test4"));
});

it("test5", () => log("----test5"));
it("test6", () => log("----test6"));
