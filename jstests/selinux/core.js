
'use strict';

load('jstests/selinux/lib/selinux_base_test.js');

class TestDefinition extends SelinuxBaseTest {
    get config() {
        return {
            "systemLog":
                {"destination": "file", "logAppend": true, "path": "/var/log/mongodb/mongod.log"},
            "storage": {"dbPath": "/var/lib/mongo", "journal": {"enabled": true}},
            "processManagement": {
                "fork": true,
                "pidFilePath": "/var/run/mongodb/mongod.pid",
                "timeZoneInfo": "/usr/share/zoneinfo"
            },
            "net": {"port": 27017, "bindIp": "127.0.0.1"}
        };
    }

    run() {
        let dirs = ["jstests/core", "jstests/core_standalone"];

        // Tests in jstests/core weren't specifically made to pass in this very scenario, so we
        // will not be fixing what is not working, and instead exclude them from running as
        // "known" to not work
        const exclude = new Set([
            "jstests/core/api_version_parameters.js",
            "jstests/core/api_version_test_expression.js",
            "jstests/core/basic6.js",
            "jstests/core/capped_empty.js",
            "jstests/core/capped_update.js",
            "jstests/core/check_shard_index.js",
            "jstests/core/collection_truncate.js",
            "jstests/core/commands_namespace_parsing.js",
            "jstests/core/comment_field.js",
            "jstests/core/compound_index_max_fields.js",
            "jstests/core/crud_ops_do_not_throw_locktimeout.js",
            "jstests/core/currentop_cursors.js",
            "jstests/core/currentop_shell.js",
            "jstests/core/currentop_waiting_for_latch.js",
            "jstests/core/datasize2.js",
            "jstests/core/doc_validation_options.js",
            "jstests/core/double_decimal_compare.js",
            "jstests/core/drop_collection.js",
            "jstests/core/explain_uuid.js",
            "jstests/core/failcommand_failpoint.js",
            "jstests/core/geo_near_point_query.js",
            "jstests/core/getlog2.js",
            "jstests/core/hash.js",
            "jstests/core/indexj.js",
            "jstests/core/jssymbol.js",
            "jstests/core/latch_analyzer.js",
            "jstests/core/list_all_sessions.js",
            "jstests/core/list_sessions.js",
            "jstests/core/logprocessdetails.js",
            "jstests/core/mr_killop.js",
            "jstests/core/profile_hide_index.js",
            "jstests/core/rename_collection_capped.js",
            "jstests/core/resume_query.js",
            "jstests/core/splitvector.js",
            "jstests/core/sort_with_update_between_getmores.js",
            "jstests/core/stages_and_hash.js",
            "jstests/core/stages_and_sorted.js",
            "jstests/core/stages_collection_scan.js",
            "jstests/core/stages_delete.js",
            "jstests/core/stages_fetch.js",
            "jstests/core/stages_ixscan.js",
            "jstests/core/stages_limit_skip.js",
            "jstests/core/stages_mergesort.js",
            "jstests/core/stages_or.js",
            "jstests/core/type8.js",
            "jstests/core/validate_db_metadata_command.js",
            "jstests/core/version_api_list_commands_verification.js",
            "jstests/core/wildcard_index_distinct_scan.js",
            "jstests/core/wildcard_index_projection.js"
        ]);

        for (let id = 0; id < dirs.length; ++id) {
            const dir = dirs[id];
            jsTest.log("Running tests in " + dir);

            const all_tests = ls(dir).filter(d => !d.endsWith("/") && !exclude.has(d)).sort();
            assert(all_tests);
            assert(all_tests.length);

            for (let i = 0; i < all_tests.length; ++i) {
                let t = all_tests[i];
                if (t.endsWith("/")) {
                    continue;
                }
                jsTest.log("Running test: " + t);
                if (!load(t)) {
                    throw ("failed to load test " + t);
                }
                jsTest.log("Successful test: " + t);
            }
        }

        jsTest.log("code test suite ran successfully");
    }
}
