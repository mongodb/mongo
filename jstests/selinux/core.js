
import {getPython3Binary} from "jstests/libs/python.js";
import {SelinuxBaseTest} from "jstests/selinux/lib/selinux_base_test.js";

export class TestDefinition extends SelinuxBaseTest {
    async run() {
        const python = getPython3Binary();

        const dirs = ["jstests/core", "jstests/core_standalone"];

        const TestData = {isHintsToQuerySettingsSuite: false};

        for (let dir of dirs) {
            jsTest.log("Running tests in " + dir);

            const all_tests = ls(dir).filter(d => d.endsWith(".js")).sort();
            assert(all_tests);
            assert(all_tests.length);

            for (let t of all_tests) {
                // Tests in jstests/core weren't specifically made to pass in this very scenario, so
                // we will not be fixing what is not working, and instead exclude them from running
                // as "known" to not work. This is done by the means of "no_selinux" tag
                const HAS_TAG = 0;
                const NO_TAG = 1;
                let checkTagRc = runNonMongoProgram(
                    python, "buildscripts/resmokelib/utils/check_has_tag.py", t, "^no_selinux$");
                if (HAS_TAG == checkTagRc) {
                    jsTest.log("Skipping test due to no_selinux tag: " + t);
                    continue;
                }
                if (NO_TAG != checkTagRc) {
                    throw ("Failure occurred while checking tags of test: " + t);
                }

                // Tests relying on featureFlagXXX will not work
                checkTagRc = runNonMongoProgram(
                    python, "buildscripts/resmokelib/utils/check_has_tag.py", t, "^featureFlag.+$");
                if (HAS_TAG == checkTagRc) {
                    jsTest.log("Skipping test due to feature flag tag: " + t);
                    continue;
                }
                if (NO_TAG != checkTagRc) {
                    throw ("Failure occurred while checking tags of test: " + t);
                }

                TestData.testName = t.substring(t.lastIndexOf('/') + 1, t.length - ".js".length);

                jsTest.log("Running test: " + t);
                try {
                    let evalString = `TestData = ${tojson(TestData)}; load(${tojson(t)});`;
                    let handle = startParallelShell(evalString, db.getMongo().port);
                    let rc = handle();
                    assert.eq(rc, 0);
                } catch (e) {
                    print(tojson(e));
                    throw ("failed to load test " + t);
                }

                jsTest.log("Successful test: " + t);
            }
        }

        jsTest.log("code test suite ran successfully");
    }
}
