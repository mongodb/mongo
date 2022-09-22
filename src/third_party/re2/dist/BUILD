# Copyright 2009 The RE2 Authors.  All Rights Reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# Bazel (http://bazel.io/) BUILD file for RE2.

licenses(["notice"])

exports_files(["LICENSE"])

config_setting(
    name = "macos",
    values = {"cpu": "darwin"},
)

config_setting(
    name = "wasm",
    values = {"cpu": "wasm32"},
)

config_setting(
    name = "windows",
    values = {"cpu": "x64_windows"},
)

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "re2",
    srcs = [
        "re2/bitmap256.h",
        "re2/bitstate.cc",
        "re2/compile.cc",
        "re2/dfa.cc",
        "re2/filtered_re2.cc",
        "re2/mimics_pcre.cc",
        "re2/nfa.cc",
        "re2/onepass.cc",
        "re2/parse.cc",
        "re2/perl_groups.cc",
        "re2/pod_array.h",
        "re2/prefilter.cc",
        "re2/prefilter.h",
        "re2/prefilter_tree.cc",
        "re2/prefilter_tree.h",
        "re2/prog.cc",
        "re2/prog.h",
        "re2/re2.cc",
        "re2/regexp.cc",
        "re2/regexp.h",
        "re2/set.cc",
        "re2/simplify.cc",
        "re2/sparse_array.h",
        "re2/sparse_set.h",
        "re2/stringpiece.cc",
        "re2/tostring.cc",
        "re2/unicode_casefold.cc",
        "re2/unicode_casefold.h",
        "re2/unicode_groups.cc",
        "re2/unicode_groups.h",
        "re2/walker-inl.h",
        "util/logging.h",
        "util/mix.h",
        "util/mutex.h",
        "util/rune.cc",
        "util/strutil.cc",
        "util/strutil.h",
        "util/utf.h",
        "util/util.h",
    ],
    hdrs = [
        "re2/filtered_re2.h",
        "re2/re2.h",
        "re2/set.h",
        "re2/stringpiece.h",
    ],
    copts = select({
        ":wasm": [],
        ":windows": [],
        "//conditions:default": ["-pthread"],
    }),
    linkopts = select({
        # macOS doesn't need `-pthread' when linking and it appears that
        # older versions of Clang will warn about the unused command line
        # argument, so just don't pass it.
        ":macos": [],
        ":wasm": [],
        ":windows": [],
        "//conditions:default": ["-pthread"],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "testing",
    testonly = 1,
    srcs = [
        "re2/testing/backtrack.cc",
        "re2/testing/dump.cc",
        "re2/testing/exhaustive_tester.cc",
        "re2/testing/null_walker.cc",
        "re2/testing/regexp_generator.cc",
        "re2/testing/string_generator.cc",
        "re2/testing/tester.cc",
        "util/pcre.cc",
    ],
    hdrs = [
        "re2/testing/exhaustive_tester.h",
        "re2/testing/regexp_generator.h",
        "re2/testing/string_generator.h",
        "re2/testing/tester.h",
        "util/benchmark.h",
        "util/flags.h",
        "util/malloc_counter.h",
        "util/pcre.h",
        "util/test.h",
    ],
    deps = [":re2"],
)

cc_library(
    name = "test",
    testonly = 1,
    srcs = ["util/test.cc"],
    deps = [":testing"],
)

cc_test(
    name = "charclass_test",
    size = "small",
    srcs = ["re2/testing/charclass_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "compile_test",
    size = "small",
    srcs = ["re2/testing/compile_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "filtered_re2_test",
    size = "small",
    srcs = ["re2/testing/filtered_re2_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "mimics_pcre_test",
    size = "small",
    srcs = ["re2/testing/mimics_pcre_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "parse_test",
    size = "small",
    srcs = ["re2/testing/parse_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "possible_match_test",
    size = "small",
    srcs = ["re2/testing/possible_match_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "re2_arg_test",
    size = "small",
    srcs = ["re2/testing/re2_arg_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "re2_test",
    size = "small",
    srcs = ["re2/testing/re2_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "regexp_test",
    size = "small",
    srcs = ["re2/testing/regexp_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "required_prefix_test",
    size = "small",
    srcs = ["re2/testing/required_prefix_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "search_test",
    size = "small",
    srcs = ["re2/testing/search_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "set_test",
    size = "small",
    srcs = ["re2/testing/set_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "simplify_test",
    size = "small",
    srcs = ["re2/testing/simplify_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "string_generator_test",
    size = "small",
    srcs = ["re2/testing/string_generator_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "dfa_test",
    size = "large",
    srcs = ["re2/testing/dfa_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "exhaustive1_test",
    size = "large",
    srcs = ["re2/testing/exhaustive1_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "exhaustive2_test",
    size = "large",
    srcs = ["re2/testing/exhaustive2_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "exhaustive3_test",
    size = "large",
    srcs = ["re2/testing/exhaustive3_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "exhaustive_test",
    size = "large",
    srcs = ["re2/testing/exhaustive_test.cc"],
    deps = [":test"],
)

cc_test(
    name = "random_test",
    size = "large",
    srcs = ["re2/testing/random_test.cc"],
    deps = [":test"],
)

cc_library(
    name = "benchmark",
    testonly = 1,
    srcs = ["util/benchmark.cc"],
    deps = [":testing"],
)

cc_binary(
    name = "regexp_benchmark",
    testonly = 1,
    srcs = ["re2/testing/regexp_benchmark.cc"],
    deps = [":benchmark"],
)
