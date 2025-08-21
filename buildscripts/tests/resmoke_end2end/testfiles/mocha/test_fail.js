import {describe, it} from "jstests/libs/mochalite.js";

const pass = () => assert(true, "pass");
const fail = () => assert(false, "fail");

it("test1", pass);
it("test2", pass);
describe("describe", function () {
    it("test3", pass);
    it("test4", fail); // This test will fail
    it("test5", pass);
});
