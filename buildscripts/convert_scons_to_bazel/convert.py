# ruff: noqa

import argparse
import ast
import collections
import glob
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from tqdm import tqdm

project_root = Path(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))).as_posix()
sys.path.append(project_root)
import buildscripts.idl.idl.compiler as idlc_mod


@dataclass
class CurrentBazelFile:
    path: str


@dataclass
class EasyNode:
    type: str
    ast_node: ast.Attribute
    name: Optional[str]
    srcs: Optional[List]
    deps: Optional[List]
    has_custom_mainline: Optional[bool] = False
    mongo_api_name: Optional[str] = None


target_tags = {
    'abt_lowering_bm': [
        '//src/mongo/unittest:system_resource_canary_bm', '//src/mongo/db/exec/sbe:abt_lowering_bm'
    ], 'abt_path_lowering_bm': [
        '//src/mongo/unittest:system_resource_canary_bm',
        '//src/mongo/db/query/optimizer:path_lower_bm'
    ], 'abt_bm': [
        '//src/mongo/unittest:system_resource_canary_bm',
        '//src/mongo/db/pipeline:abt_translation_bm'
    ],
    'bsoncolumn_bm': ['//src/mongo/bson/util:bsoncolumn_bm', '//src/mongo/bson/util:simple8b_bm'],
    'expression_sbe_bm': [
        '//src/mongo/unittest:system_resource_canary_bm', '//src/mongo/db/query:sbe_expression_bm'
    ], 'expression_bm': [
        '//src/mongo/unittest:system_resource_canary_bm', '//src/mongo/db/pipeline:expression_bm'
    ], 'first_half_bm': [
        "//src/mongo/bson:bson_bm",
        "//src/mongo/util:base64_bm",
        "//src/mongo/db/concurrency:lock_manager_bm",
        "//src/mongo/util:uuid_bm",
        "//src/mongo/db/query/optimizer:path_lower_bm",
        "//src/mongo/db/storage:storage_record_id_bm",
        "//src/mongo/db/storage/wiredtiger:storage_wiredtiger_begin_transaction_block_bm",
        "//src/mongo/db:commands_bm",
        "//src/mongo/db/exec/sbe:abt_lowering_bm",
        "//src/mongo/logv2:logv2_bm",
        "//src/mongo/stdx:condition_variable_bm",
        "//src/mongo/db/exec/document_value:document_bm",
        "//src/mongo/util:decimal_counter_bm",
        "//src/mongo/platform:endian_bm",
        "//src/mongo/util:clock_source_bm",
        "//src/mongo/db/query/boolean_simplification:quine_mccluskey_bm",
        "//src/mongo/util:string_bm",
        "//src/mongo/util:stacktrace_bm",
        "//src/mongo/db/s:placement_history_bm",
        "//src/mongo/db/exec/sbe:sbe_vm_bm",
        "//src/mongo/util:tick_source_bm",
        "//src/mongo/crypto:crypto_bm",
        "//src/mongo/unittest:system_resource_canary_bm",
        "//src/mongo/util:future_bm",
    ], 'namespace_string_bm': [
        "//src/mongo/unittest:system_resource_canary_bm",
        "//src/mongo/db:namespace_string_bm",
    ], 'query_bm': [
        "//src/mongo/db/pipeline:percentile_algo_bm",
        "//src/mongo/db/pipeline:window_function_percentile_bm",
        "//src/mongo/db/query/query_stats:rate_limiting_bm",
        "//src/mongo/db/query/query_stats:shapifying_bm",
        "//src/mongo/db:profile_filter_bm",
        "//src/mongo/db/query/query_settings:query_settings_lookup_bm",
        "//src/mongo/db/query:plan_cache_key_encoding_bm",
        "//src/mongo/db/query:canonical_query_bm",
        "//src/mongo/db/query:query_planner_bm",
        "//src/mongo/db/query:plan_cache_classic_bm",
    ], 'repl_bm': [
        "//src/mongo/db/op_observer:op_observer_bm",
        "//src/mongo/db/repl:oplog_application_bm",
        "//src/mongo/db/repl:oplog_applier_utils_bm",
        "//src/mongo/db/repl:oplog_entry_bm",
        "//src/mongo/db/repl:replication_consistency_markers_impl_bm",
        "//src/mongo/db/repl:oplog_write_bm",
    ], "sep_bm": ["//src/mongo/db:service_entry_point_common_bm", ], "sharding_bm": [
        "//src/mongo/unittest:system_resource_canary_bm",
        "//src/mongo/db/s:chunk_manager_refresh_bm",
        "//src/mongo/db/s:sharding_write_router_bm",
    ], 'streams_bm': [
        "//src/mongo/unittest:system_resource_canary_bm",
        "//src/mongo/db/modules/enterprise/src/streams/exec:streams_operator_dag_bm",
        "//src/mongo/db/modules/enterprise/src/streams/exec:streams_window_operator_bm",
    ]
}
reversed_target_tags = {item: key for key, value_list in target_tags.items() for item in value_list}

interesting_nodes = {
    "BazelLibrary": "mongo_cc_library",
    "Library": "mongo_cc_library",
    "Program": "mongo_cc_binary",
    "CppUnitTest": "mongo_cc_unit_test",
    "Benchmark": "mongo_cc_benchmark",
    "CppIntegrationTest": "mongo_cc_integration_test",
    "CppLibFuzzerTest": "mongo_cc_fuzzer_test",
}

interesting_keywords = {
    "target": "name",
    "source": "srcs",
    "LIBDEPS": "deps",
    "LIBDEPS_PRIVATE": "deps",
    "LIBDEPS_INTERFACE": "deps",
    "LIBDEPS_DEPENDENTS": None,
    "AIB_COMPONENT": None,
    "LIBDEPS_TAGS": None,
    "UNITTEST_HAS_CUSTOM_MAINLINE": "has_custom_mainline",
    "MONGO_API_NAME": "mongo_api_name",
}

shim_map = {
    "//src/third_party:shim_abseil": [],
    "//src/third_party:shim_yaml": ["//src/third_party/yaml-cpp:yaml"],
    "//src/third_party:shim_pcre2": ["//src/third_party/pcre2"],
    "//src/third_party:shim_libmongocrypt": ["//src/third_party/libmongocrypt:mongocrypt"],
    "//src/third_party:shim_zstd": ["//src/third_party/zstandard:zstd"],
    "//src/third_party:shim_zlib": ["//src/third_party/zlib"],
    "//src/third_party:shim_snappy": ["//src/third_party/snappy"],
    "//src/third_party:shim_icu": ["//src/third_party/icu4c-57.1/source:icu_i18n"],
    "//src/third_party:shim_timelib": ["//src/third_party/timelib"],
    "//src/third_party:shim_asio": ["//src/third_party/asio-master:asio"],
    "//src/third_party:shim_stemmer": [],
    "//src/third_party:shim_wiredtiger": ["//src/third_party/wiredtiger"],
    "//src/third_party:shim_libbson": ["//src/third_party/libbson:bson"],
    "//src/third_party:shim_grpc": ["//src/third_party/grpc:grpc++_reflection"],
    "//src/third_party:shim_benchmark": ["//src/third_party/benchmark"],
}

duplicate_idls = {
    "cursor_response_idl": "cursor_response_idl_only",
    "shutdown_idl": "shutdown_idl_only",
}

scons_name_changes = {
    ':import_collection_oplog_entry"': ':import_collection_oplog_entry_idl"',
    '/resharding:resume_token_idl"': '/resharding:resharding_resume_token_idl"',
    '/resharding:common_types_idl"': '/resharding:resharding_common_types_idl"',
}

skip_build_files = [
    "/db/test_output/",
    "/bson_reader_test_data",
    "/third_party/",
]

extra_deps = {
    "//src/mongo/db/process_health:fault_base_classes_test": [
        "//src/mongo/db:service_context_test_fixture",
    ],
    "//src/mongo/client/sdam:sdam_test": ["//src/mongo/db:service_context_test_fixture", ],
    "//src/mongo/db/modules/enterprise/src/ldap:enterprise_ldap_test": [
        "//src/mongo/db:service_context_test_fixture",
    ],
}

ignored_deps = ['"//src/third_party/croaring:croaring"']

idl_files_generated = set()
idl_files_discover = set()
unit_tests_converted = []
dependents = collections.defaultdict(list)
idl_deps = dict()

easy_targets = 0
complex_targets = 0
callback_targets = 0

header_groups = set()
current_node = None
current_bazel_file = None
load_statements = None


def write_to_current_bazel_file(dir_path, data, prepend=False):
    global current_bazel_file, load_statements, header_groups
    bazel_file = Path(os.path.join(dir_path, "BUILD.bazel")).as_posix()
    if current_bazel_file is None or current_bazel_file.path != bazel_file:
        current_bazel_file = CurrentBazelFile(path=bazel_file)

        with open(current_bazel_file.path, "w") as f:
            f.write(load_statements)
            f.write("""
package(default_visibility = ["//visibility:public"])
                    
exports_files(glob(["*.cpp", "*.h", "*.inl", "*.hpp", "*.py", "*.in"]))
                    
filegroup(name = "%s_global_hdrs", srcs = glob(["*.h", "*.inl", "*.hpp"]))
                    
""" % os.path.basename(os.path.dirname(bazel_file)))
            f.write(data + "\n")
            dir_name = os.path.dirname(bazel_file)
            group_name = os.path.basename(dir_name)
            header_groups.add(f"//{dir_name}:{group_name}_global_hdrs")
    else:
        if prepend:
            with open(current_bazel_file.path, 'r+') as f:
                content = f.read()
                f.seek(0, 0)
                f.write(data + "\n" + content)
        else:
            with open(current_bazel_file.path, "a") as f:
                f.write(data + "\n")


def run_git_command(cmd, ignore_error=True):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        if "did not match any file(s) known to git" in proc.stderr and ignore_error:
            return
        print(
            f"git failed: {cmd} failed with exit code {proc.returncode}:\n{proc.stderr}\n{proc.stdout}"
        )
        sys.exit(1)


def process_third_parties(clean):

    if clean:
        repo = "HEAD"
        run_git_command(["git", "reset"])
        run_git_command(["git", "clean", "-fd"])
        run_git_command([
            "git", "checkout", repo, "--",
            "src/mongo/db/modules/enterprise/src/fle/lib/mongo_crypt.cpp"
        ])
        run_git_command(["git", "checkout", repo, "--", "src/mongo/db/s/migration_util_test.cpp"])

    else:
        repo = "origin/master"

    run_git_command(["git", "checkout", repo, "--", "bazel"])
    run_git_command(["git", "checkout", repo, "--", "tools"])
    run_git_command(["git", "checkout", repo, "--", "BUILD.bazel"])
    run_git_command(["git", "checkout", repo, "--", "WORKSPACE.bazel"])
    run_git_command(["git", "checkout", repo, "--", "MODULE.bazel"])
    run_git_command(["git", "checkout", repo, "--", ".bazelrc"])
    run_git_command(["git", "checkout", repo, "--", ".bazeliskrc"])
    run_git_command(["git", "checkout", repo, "--", ".bazelversion"])
    run_git_command(["git", "checkout", repo, "--", ".npmrc"])
    run_git_command(["git", "checkout", repo, "--", "eslint.config.mjs"])
    run_git_command(["git", "checkout", repo, "--", "package.json"])
    run_git_command(["git", "checkout", repo, "--", "buildscripts/bazel_rules_mongo"])
    run_git_command(["git", "checkout", repo, "--", "buildscripts/s3_binary"])
    run_git_command(["git", "checkout", repo, "--", "buildscripts/unittest_grouper.py"])
    run_git_command(["git", "checkout", repo, "--", "buildscripts/install_bazel.py"])
    run_git_command(["git", "checkout", repo, "--", "bazel/config"])
    run_git_command(["git", "checkout", repo, "--", "src/mongo/_bypass_header_example.h"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/bazel_test.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/bazel_compile.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/bazel_run.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/bazel_utility_functions.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/bazel_RBE_supported.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/run_python_script_with_report.sh"])
    run_git_command(["git", "checkout", repo, "--", "evergreen/generate_evergreen_bazelrc.sh"])

    run_git_command(
        ["git", "checkout", repo, "--", "evergreen/generate_evergreen_engflow_bazelrc.sh"])

    if not clean:
        with open(".bazelrc", "a") as f:
            f.write("\ntest --test_timeout=900\n")
            f.write("\ncommon:macos --copt=-DBOOST_NO_CXX98_FUNCTION_BASE\n")
            f.write(
                "\ncommon --flag_alias=use_diagnostic_latches=//bazel/config:use_diagnostic_latches\n"
            )

    partial_3p = {
        "wiredtiger": [  # different versions
            "build_linux/wiredtiger_config.h.in",
            "build_win/wiredtiger_config.h.in",
            "build_darwin/wiredtiger_config.h.in",
            "wt_config_header.py",
            "wt_version_header.py",
        ],
        "cares": ["BUILD.bazel"],  # SERVER-95368 Upgrade c-ares library to 1.27.0 (#31344)
        "mozjs": ["BUILD.bazel"],
        "croaring": ["BUILD.bazel"],
        "asio-master": ["BUILD.bazel"],
        "tomcrypt-1.18.2": ["BUILD.bazel"],
        "libmongocrypt": [],
        "immer": ["BUILD.bazel"],
        "zlib": ["BUILD.bazel"],
        "fmt": ["BUILD.bazel"],
        "libbson": [],
        "timelib": ["BUILD.bazel"],
        "librdkafka": [],
    }

    # during the conversion project some third parties upgraded so we capture the version from master
    # we want to use
    static_3p = {
        "boost": "01e9685d59f31bd63732904b185b41c44020b0ad",
    }

    allowed_3p_diffs = {
        "s2": "single mongo specific header cleanup",
        "grpc": "not used in 8.0 and not version change",
        "tcmalloc": "not a version change, extra mongo debugging",
        "libstemmer_c": "just moved files around",
        "boost": "same version some mongo customizations",
        "_bypass_header_example.h": "placeholder header file not used",
        "icu4c-57.1": "SERVER-101178 Apply security fixes to our vendored ICU library (#35090)",
        "db": "Backported version header change",
    }

    third_partys = [
        name for name in os.listdir("src/third_party") if os.path.isdir(f"src/third_party/{name}")
    ]
    third_partys += ["scons-3.1.2", "sasl"]
    third_partys_with_diffs = set()
    for third_party in tqdm(third_partys, ascii=' >',
                            desc="Copying master" if not clean else "Cleaning",
                            bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):
        if third_party in partial_3p:
            for file in partial_3p[third_party]:
                run_git_command(
                    ["git", "checkout", repo, "--", f"src/third_party/{third_party}/{file}"],
                    ignore_error=clean)
        elif third_party in static_3p:

            if not clean:
                static_repo = static_3p[third_party]
            else:
                static_repo = repo

            shutil.rmtree(f"src/third_party/{third_party}", ignore_errors=True)
            run_git_command(
                ["git", "checkout", static_repo, "--", f"src/third_party/{third_party}"])
        else:
            shutil.rmtree(f"src/third_party/{third_party}", ignore_errors=True)
            run_git_command(["git", "checkout", repo, "--", f"src/third_party/{third_party}"])

    # special sauce for wiredtiger
    filelist_script = Path(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "s_bazel.py")).as_posix()
    proc = subprocess.run([sys.executable, filelist_script], capture_output=True, text=True,
                          check=True)

    if clean:
        if os.path.exists("src/BUILD.bazel"):
            os.unlink("src/BUILD.bazel")
        run_git_command(["git", "reset"])
        run_git_command(["git", "clean", "-fd"])

    proc = subprocess.run(["git", "status"], capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.endswith((".cpp", ".c", ".cc", ".h", "hpp")):
            path = line.split(" ")[-1]
            name = path.split("/")[2]
            third_partys_with_diffs.add(name)

    if third_partys_with_diffs:
        for third_party in third_partys_with_diffs:
            if third_party in allowed_3p_diffs:
                print(f"INFO: {third_party} diff: {allowed_3p_diffs[third_party]}")
            else:
                print(f"WARNING: unexcused diffs for {third_party}")


def write_global_header_bazel():
    global header_groups

    header_groups.add("//src/mongo/base:error_codes_header")
    header_groups.add("//src/mongo/util/version:releases_header")
    header_groups.add("//src/mongo/util/net/ssl:ssl")
    header_groups.add("//src/mongo/util/net/ssl/impl:ssl")
    header_groups.add("//src/mongo/util/net/ssl/detail:ssl")
    header_groups.add("//src/mongo/util/net/ssl/detail/impl:ssl")
    header_groups.add("//src/mongo/scripting/mozjs:internedstring.defs")
    header_groups.add("//src/mongo/db/sorter:sorter.cpp")
    header_groups.add("//src/mongo/db/fts:stop_words_list_cpp_gen")
    header_groups.add("//src/mongo/db:feature_flag_test_gen")
    header_groups.add("//src/third_party/wiredtiger:wt_version_header")
    header_groups.add("//src/third_party/wiredtiger:wt_config_header")
    header_groups.add("//src/mongo/util:version_constants_gen")
    header_groups |= idl_files_generated

    with open("src/BUILD.bazel", "w") as f:
        f.write("""
load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
                
exports_files([
    "*.h",
    "*.cpp",
])

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "core_headers_library",
    hdrs = [
        %s
    ],
)  
""" % ",\n        ".join(['"' + hdr_group + '"' for hdr_group in sorted(header_groups)]))


def WriteComplexNode(msg, dir_path):
    global current_node, complex_targets

    callbacks = get_callbacks(dir_path)
    if callbacks and current_node and current_node.name in callbacks:
        current_node = None
        return
    commented_msg = '\n'.join(["# " + line for line in msg.splitlines()]) + "\n\n"
    write_to_current_bazel_file(dir_path, commented_msg)
    current_node = None
    complex_targets += 1


def WriteIdlNode(idl_path):
    global easy_targets, current_bazel_file
    node_name = os.path.basename(idl_path).replace(".idl", "_idl")
    callbacks = get_callbacks(os.path.dirname(idl_path))
    if callbacks and node_name in callbacks:
        return

    callback_appends = get_callback_appends(os.path.dirname(idl_path))
    if node_name in callback_appends:
        callback_appends = callback_appends[node_name]
    else:
        callback_appends = {}
    include_paths = ["src"]
    if "/modules/enterprise/" in idl_path:
        include_paths += ["src/mongo/db/modules/enterprise/src"]
    resolver = idlc_mod.CompilerImportResolver(include_paths)

    idl_deps_list = []
    with open(idl_path, encoding="utf-8") as file_stream:
        parsed_doc = idlc_mod.parser.parse(file_stream, idl_path, resolver)

        if not parsed_doc.errors and parsed_doc.spec.imports is not None:
            for idl_dep in parsed_doc.spec.imports.dependencies:
                idl_dep = Path(idl_dep).as_posix()

                if idl_dep.startswith(project_root):
                    idl_dep = idl_dep[len(project_root) + 1:]
                idl_files_discover.add(idl_dep)
                idl_dep = bazilify_path(idl_dep)
                if idl_dep.startswith("src/"):
                    idl_dep = "//" + idl_dep
                idl_dep = idl_dep.replace(".idl", "_idl")
                for dup_idl in duplicate_idls:
                    if idl_dep.endswith(dup_idl):
                        idl_dep = idl_dep.replace(dup_idl, duplicate_idls[dup_idl])
                for name_change in scons_name_changes:
                    unquoted_name_change = name_change[:-1]
                    if idl_dep.endswith(unquoted_name_change):
                        idl_dep = idl_dep.replace(unquoted_name_change,
                                                  scons_name_changes[name_change][:-1])
                idl_deps_list.append(idl_dep)

    idl_file = os.path.normpath(idl_path)

    if os.path.dirname(idl_file) == os.path.dirname(current_bazel_file.path):
        src_idl_file = os.path.basename(idl_file)
    else:
        src_idl_file = "//" + bazilify_path(idl_file)
    idl_file = "//" + bazilify_path(idl_file)
    idl_node = idl_file.replace(".idl", "_idl")
    for dup_idl in duplicate_idls:
        if idl_node.endswith(dup_idl):
            idl_node = idl_node.replace(dup_idl, duplicate_idls[dup_idl])
    idl_files_generated.add(idl_node + "_gen")

    stanza = """\
mongo_idl_library(
    name = "%s",
    src = "%s",""" % (node_name, src_idl_file)

    if idl_deps_list:
        stanza += """\

    idl_deps = [
        %s
    ],
    deps = [
        %s
    ],""" % (',\n        '.join(
            ['"' + dep + '_gen"'
             for dep in idl_deps_list + callback_appends.get("idl_deps", [])]), ',\n        '.join(
                 ['"' + dep + '"' for dep in idl_deps_list + callback_appends.get("deps", [])]))

    for field, appends in callback_appends.items():
        if field in ["deps", "idl_deps"]:
            continue
        if isinstance(appends, list):
            stanza += """\
    %s = [
        %s
    ],""" % (field, ',\n        '.join(appends))
        else:
            stanza += """\
    %s = %s,""" % (field, appends)

    stanza += """
)

"""

    write_to_current_bazel_file(os.path.dirname(idl_path), stanza)

    easy_targets += 1


def WriteEasyNode(dir_path):
    global current_node, easy_targets, unit_tests_converted
    callbacks = get_callbacks(dir_path)
    if callbacks and current_node and current_node.name in callbacks:
        current_node = None
        return
    callback_appends = get_callback_appends(dir_path)
    target_node = "//" + bazilify_path(os.path.join(dir_path, current_node.name))
    if current_node.name in callback_appends:
        callback_appends = callback_appends[current_node.name]
    else:
        callback_appends = {}
    updated_deps = []
    for dep in current_node.deps:
        for change in scons_name_changes:
            if dep.endswith(change):
                dep = dep[:-len(change)] + scons_name_changes[change]
                break

        if dep not in ignored_deps:
            updated_deps.append(dep)

    target_node = "//" + bazilify_path(os.path.join(dir_path, current_node.name))
    if target_node in extra_deps:
        updated_deps.extend(['"' + dep + '"' for dep in extra_deps[target_node]])

    stanza = """\
%s(
    name = "%s",
    srcs = [
        %s
    ],""" % (current_node.type, current_node.name,
             ',\n        '.join(current_node.srcs + callback_appends.get("srcs", [])))

    if current_node.deps:
        stanza += """
    deps = [
        %s
    ],""" % (',\n        '.join(updated_deps + callback_appends.get("deps", [])))

    if current_node.has_custom_mainline:
        stanza += f"\n    has_custom_mainline = {current_node.has_custom_mainline},"
    if current_node.mongo_api_name:
        stanza += f'\n    mongo_api_name = "{current_node.mongo_api_name}",'

    for field, appends in callback_appends.items():
        if field in ["deps", "idl_deps"]:
            continue
        if isinstance(appends, list):
            stanza += """\
    %s = [
        %s
    ],""" % (field, ',\n        '.join(appends))
        else:
            stanza += """\
    %s = %s,""" % (field, appends)

    stanza += "\n)\n"

    write_to_current_bazel_file(dir_path, stanza)

    if current_node.type in ["mongo_cc_unit_test", "mongo_cc_integration_test"]:
        unit_tests_converted.append("//" + bazilify_path(os.path.join(dir_path, current_node.name)))

    current_node = None
    easy_targets += 1


def WriteCallbackNode(dir_path, stanza):
    global current_node, callback_targets

    write_to_current_bazel_file(dir_path, stanza)

    current_node = None
    callback_targets += 1


def process_node(current_node, dir_path):
    if current_node.name and current_node.srcs:
        current_node.name = current_node.name.replace("_${MONGO_CRYPTO}", "")
        WriteEasyNode(dir_path)
    else:
        WriteComplexNode(
            f"Found complex target {current_node.name}: {dir_path + '/SConscript'}:{ast.dump(current_node.ast_node, include_attributes=True, indent=4)}",
            dir_path)


def get_callbacks(dir_path):
    generator_callbacks_file = Path(os.path.join(dir_path, "converter_callbacks.py")).as_posix()
    if os.path.exists(generator_callbacks_file):
        localdict = {}
        exec(open(generator_callbacks_file).read(), globals(), localdict)
        try:
            converter_callbacks = localdict["converter_callbacks"]
            if not isinstance(converter_callbacks, dict):
                raise TypeError
        except (KeyError, TypeError):
            print(f'{generator_callbacks_file} did not define "converter_callbacks" dictionary.')
            sys.exit(1)
        else:
            return converter_callbacks


def get_callback_appends(dir_path):
    generator_callbacks_file = Path(os.path.join(dir_path, "converter_callbacks.py")).as_posix()
    if os.path.exists(generator_callbacks_file):
        localdict = {}
        exec(open(generator_callbacks_file).read(), globals(), localdict)
        try:
            converter_callbacks = localdict["converter_appends"]
            if not isinstance(converter_callbacks, dict):
                raise TypeError
        except KeyError:
            return dict()
        except (TypeError):
            print(f'{generator_callbacks_file} did not define "converter_appends" as a dictionary.')
            sys.exit(1)
        else:
            return converter_callbacks
    return dict()


def apply_patches():
    patches = glob.glob("buildscripts/convert_scons_to_bazel/patches/*.patch")
    for patch in tqdm(
            sorted(patches), ascii=' >', desc="Applying patches",
            bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):
        run_git_command(["git", "apply", patch])


def apply_idl_deps(buildozer):
    global idl_deps
    for old, new in tqdm(idl_deps.items(), ascii=' >', desc="Applying idl dependencies",
                         bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):
        cmd = [buildozer, f"replace deps {old} {new}", "//src/...:*"]
        proc = subprocess.run(cmd, capture_output=True, text=True)


def apply_tags(buildozer):
    for target, tag in tqdm(reversed_target_tags.items(), ascii=' >', desc="Applying tags",
                            bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):
        cmd = [buildozer, f"add tags {tag}", target]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)


def bazilify_path(path):
    return Path(os.path.dirname(path)).as_posix().replace(".", "") + ":" + os.path.basename(path)


def main():

    global current_node, current_bazel_file, load_statements, unit_tests_converted, dependents

    start_time = time.time()
    parser = argparse.ArgumentParser()
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--noclean", action="store_true")
    parser.add_argument("--sconscript")
    parser.add_argument("--unit-test-check", action="store_true")
    args = parser.parse_args()

    if not args.clean and not args.noclean and not args.sconscript:
        process_third_parties(True)

    if not args.sconscript:
        process_third_parties(args.clean)

    if args.clean and not args.noclean:
        sys.exit(0)

    from buildscripts.install_bazel import install_buildozer
    buildozer = install_buildozer()

    with open("buildscripts/convert_scons_to_bazel/load_statements.txt") as f:
        load_statements = f.read()

    if not args.sconscript:
        walk_dir = "src/mongo"
        total_dirs = 1
        stack = [walk_dir]
        while stack:
            current = stack.pop()
            try:
                with os.scandir(current) as it:
                    for entry in it:
                        if entry.is_dir(follow_symlinks=False):
                            total_dirs += 1
                            stack.append(entry.path)
            except PermissionError:
                continue
    else:
        walk_dir = os.path.dirname(args.sconscript)
        total_dirs = 1

    first_walk = True
    for root, _, files in tqdm(
            os.walk(walk_dir), ascii=' >', total=total_dirs, desc="Walking repo",
            bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):

        if args.sconscript:
            root = walk_dir
            files = []
            if not first_walk:
                break
            first_walk = False

        bazel_file = (Path(root) / "BUILD.bazel").as_posix()
        sconscript = (Path(root) / "SConscript").as_posix()

        if args.sconscript and os.path.exists(bazel_file):
            os.unlink(bazel_file)

        if os.path.exists(bazel_file):
            current_bazel_file = CurrentBazelFile(path=bazel_file)
            # Add all our load statements to existing BUILD.bazel file
            write_to_current_bazel_file(root, load_statements, prepend=True)
            #print(f"{bazel_file} already exists, skipping SConsfile conversion.")
        elif os.path.exists(sconscript):

            with open(sconscript) as f:

                for node in ast.walk(ast.parse(f.read())):

                    # process new node
                    if isinstance(node, ast.Attribute):

                        if current_node:
                            process_node(current_node, root)

                        if node.attr in interesting_nodes:
                            current_node = EasyNode(interesting_nodes[node.attr], node, None, None,
                                                    [])
                        continue

                    if current_node:
                        if isinstance(node, ast.keyword):
                            try:
                                if node.arg in interesting_keywords:
                                    value = node.value
                                    if isinstance(value, ast.List):
                                        if node.arg == "source":
                                            current_node.srcs = []

                                            for src in value.elts:

                                                if not isinstance(src.value, str):
                                                    raise TypeError
                                                src_str = src.value
                                                if not src_str.endswith(
                                                    (".cpp", ".c", ".cc", ".idl")):
                                                    print(f"ERROR: unsupported src ext {src_str}")
                                                    raise TypeError
                                                if "$BUILD_DIR" in src_str:
                                                    src_str = src_str.replace("$BUILD_DIR", "src")
                                                else:
                                                    src_str = os.path.join(root, src_str)
                                                    src_str = Path(
                                                        os.path.normpath(src_str)).as_posix()
                                                src_str = "//" + bazilify_path(src_str)
                                                if src_str.endswith(".idl"):
                                                    idl_lib = src_str.replace(".idl", "_idl")
                                                    if idl_lib is not None:
                                                        current_node.deps += ['"' + idl_lib + '"']
                                                    #Ignore node with only idl src
                                                    if len(value.elts) == 1:
                                                        scons_target_name = "//" + bazilify_path(
                                                            os.path.join(root, current_node.name))
                                                        idl_deps[scons_target_name] = idl_lib
                                                        current_node = None
                                                    continue
                                                current_node.srcs += ['"' + src_str + '"']
                                        elif node.arg == "target" and len(value.elts) == 1:
                                            current_node.name = value.elts[0].value
                                        elif node.arg == "LIBDEPS" or node.arg == "LIBDEPS_PRIVATE" or node.arg == "LIBDEPS_INTERFACE":
                                            for dep in value.elts:
                                                dep_str = dep.value

                                                if dep_str.startswith(
                                                        "src/") and bazel_file.startswith(
                                                            "src/mongo/db/modules/enterprise"):
                                                    dep_str = "src/mongo/db/modules/enterprise/" + dep_str

                                                dep_str = dep_str.replace("$BUILD_DIR", "src")
                                                dep_str = dep_str.replace("_${MONGO_CRYPTO}", "")
                                                if not dep_str.startswith("src/"):
                                                    dep_str = Path(
                                                        os.path.normpath(
                                                            os.path.join(root,
                                                                         dep_str))).as_posix()
                                                dep_str = "//" + bazilify_path(dep_str)

                                                if "//src/mongo/db/modules/enterprise" in dep_str and not bazel_file.startswith(
                                                        "src/mongo/db/modules/enterprise"):
                                                    raise TypeError
                                                if dep_str in shim_map:
                                                    for shim_dep in shim_map[dep_str]:
                                                        current_node.deps += ['"' + shim_dep + '"']
                                                else:
                                                    current_node.deps += ['"' + dep_str + '"']
                                        elif node.arg == "LIBDEPS_DEPENDENTS":
                                            for dep in value.elts:
                                                tgt_str = dep.value
                                                if tgt_str.startswith(
                                                        "src/") and bazel_file.startswith(
                                                            "src/mongo/db/modules/enterprise/"):
                                                    tgt_str = "src/mongo/db/modules/enterprise/" + tgt_str
                                                tgt_str = tgt_str.replace("$BUILD_DIR", "src")
                                                if not tgt_str.startswith("src/"):
                                                    tgt_str = Path(
                                                        os.path.normpath(
                                                            os.path.join(root,
                                                                         tgt_str))).as_posix()
                                                tgt_str = "//" + bazilify_path(tgt_str)
                                                dep_str = "//" + bazilify_path(
                                                    os.path.join(root, current_node.name))
                                                dependents[tgt_str].append(dep_str)

                                    elif isinstance(value, ast.Constant):
                                        if node.arg == "target":
                                            current_node.name = value.value
                                        if node.arg == "UNITTEST_HAS_CUSTOM_MAINLINE":
                                            current_node.has_custom_mainline = value.value
                                        if node.arg == "MONGO_API_NAME":
                                            current_node.mongo_api_name = value.value
                                        if node.arg == "source":
                                            if not isinstance(value.value, str):
                                                raise TypeError
                                            src_str = value.value
                                            if not src_str.endswith((".cpp", ".c", ".cc", ".idl")):
                                                print(f"ERROR: unsupported src ext {src_str}")
                                                raise TypeError
                                            if "$BUILD_DIR" in src_str:
                                                src_str = src_str.replace("$BUILD_DIR", "src")
                                            else:
                                                src_str = os.path.join(root, src_str)
                                                src_str = Path(os.path.normpath(src_str)).as_posix()
                                            src_str = "//" + bazilify_path(src_str)
                                            if src_str.endswith(".idl"):
                                                idl_lib = src_str.replace(".idl", "_idl")
                                                if idl_lib is not None:
                                                    current_node.deps += ['"' + idl_lib + '"']
                                                #Ignore node with only idl src
                                                scons_target_name = "//" + bazilify_path(
                                                    os.path.join(root, current_node.name))
                                                idl_deps[scons_target_name] = idl_lib
                                                current_node = None
                                            else:
                                                current_node.srcs = ['"' + src_str + '"']

                                else:
                                    WriteComplexNode(
                                        f"Found target {current_node.name} with complex inputs: {sconscript}:{ast.dump(current_node.ast_node, include_attributes=True, indent=4)}",
                                        root)
                            except (AttributeError, TypeError):
                                WriteComplexNode(
                                    f"Found target {current_node.name} with complex inputs: {sconscript}:{ast.dump(current_node.ast_node, include_attributes=True, indent=4)}",
                                    root)
                        else:
                            continue

                if current_node:
                    process_node(current_node, root)

        if not os.path.exists(bazel_file):
            # Create BUILD files everywhere mwuahahahaha
            if all([token not in bazel_file for token in skip_build_files]):

                with open(bazel_file, 'w') as f:
                    f.write("""
package(default_visibility = ["//visibility:public"])

exports_files(glob([
    "*.cpp",
    "*.h",
]))
                        
filegroup(name = "%s_global_hdrs", srcs = glob(["*.h"]))
                        
""" % os.path.basename(os.path.dirname(bazel_file)))
                    dir_name = os.path.dirname(bazel_file)
                    group_name = os.path.basename(dir_name)
                    header_groups.add(f"//{dir_name}:{group_name}_global_hdrs")

        callbacks = get_callbacks(root)
        if callbacks:
            for scons_name, entry in callbacks.items():
                WriteCallbackNode(root, entry)
                if scons_name.endswith("_test"):
                    unit_tests_converted.append("//" +
                                                bazilify_path(os.path.join(root, scons_name)))

        for f in files:
            if f.endswith(".idl"):
                idl_path = Path(os.path.join(root, f)).as_posix()
                WriteIdlNode(idl_path)

        if current_bazel_file:
            # Fix attempt to reduce load set after every modification
            pkg_target = "//" + current_bazel_file.path.replace("/BUILD.bazel", ":__pkg__")
            cmd = [buildozer, 'fix unusedLoads', f"{pkg_target}"]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode not in [0, 3]:
                print(
                    f"Buildozer failed {cmd} failed with exit code {proc.returncode}:\n{proc.stderr}\n{proc.stdout}"
                )
                sys.exit(1)

    apply_idl_deps(buildozer)

    apply_tags(buildozer)

    if args.sconscript:
        sys.exit(0)
    apply_patches()

    write_global_header_bazel()

    if os.name != "nt":
        subprocess.run([sys.executable, "buildscripts/unittest_grouper.py", "--fix"])

    if args.unit_test_check:
        passing_tests = []
        failing_tests = []
        with open("buildscripts/convert_scons_to_bazel/converted_unittests.txt", "r+") as f:
            building_tests = set([l.rstrip() for l in f])
            for test in tqdm(unit_tests_converted, ascii=' >', desc="Building unit tests",
                             bar_format='{l_bar}{bar:40}{r_bar}{bar:-40b}'):
                if test in building_tests:
                    passing_tests.append(test)
                    continue
                cmd = ["bazel", "build", test]
                proc = subprocess.run(cmd, capture_output=True, text=True)
                if proc.returncode == 0:
                    passing_tests.append(test)
                else:
                    failing_tests.append(test)

            print("The following tests are failing:")
            print("\n".join(failing_tests))
            f.seek(0)
            f.write("\n".join(sorted(passing_tests)))
            f.truncate()

    tests_to_tag = []
    windows_tests = []
    with open("buildscripts/convert_scons_to_bazel/unittest_dep_orders.txt") as f_base_list:
        total = len(f_base_list.readlines())
    with open("buildscripts/convert_scons_to_bazel/converted_unittests.txt") as f_conv_list:
        lines = f_conv_list.readlines()
        completed = len(lines)
        for line in lines:
            line = line.strip()
            if line.startswith("//"):
                tests_to_tag.append(line)

    with open("buildscripts/convert_scons_to_bazel/windows_excluded_tests.txt") as f_win_ex:
        lines = f_win_ex.readlines()
        windows_tests = tests_to_tag.copy()
        for line in lines:
            windows_tests.remove(line.strip())

    for test in tests_to_tag:
        cmd = [buildozer, 'add tags convert_target', f"{test}"]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode not in [0, 3]:
            print(
                f"Buildozer failed {cmd} failed with exit code {proc.returncode}:\n{proc.stderr}\n{proc.stdout}"
            )
            sys.exit(1)

    if os.name == "nt":

        with open("BUILD.bazel", "a") as f:
            f.write("""
mongo_install(
    name = "windows_unittests",
    srcs = [
        %s
    ] + select({
        "@platforms//os:windows": ["@windows_sasl//:bins"],
        "//conditions:default": [],
    }),
    testonly = True,
)
""" % "\n        ".join(['"' + test + '",' for test in windows_tests]))

    percent = completed / total * 100

    print(f"total targets = {complex_targets + easy_targets + callback_targets}")
    print(f"targets automatically converted = {easy_targets}")
    print(f"targets manually converted = {callback_targets}")
    print(f"targets not converted = {complex_targets}")
    print(f"time to convert: {(time.time() - start_time):0.1f} seconds")
    print(f"unittests completed: {percent:0.2f}%")


main()
