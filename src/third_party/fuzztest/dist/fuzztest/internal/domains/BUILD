# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

package(default_visibility = ["@com_google_fuzztest//:__subpackages__"])

licenses(["notice"])

cc_library(
    name = "absl_helpers",
    hdrs = ["absl_helpers.h"],
    deps = [
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest/internal:logging",
    ],
)

cc_library(
    name = "core_domains_impl",
    hdrs = [
        "aggregate_of_impl.h",
        "arbitrary_impl.h",
        "bit_flag_combination_of_impl.h",
        "bit_gen_ref.h",
        "container_mutation_helpers.h",
        "container_of_impl.h",
        "domain.h",
        "domain_base.h",
        "domain_type_erasure.h",
        "element_of_impl.h",
        "filter_impl.h",
        "flat_map_impl.h",
        "in_range_impl.h",
        "map_impl.h",
        "mutation_metadata.h",
        "one_of_impl.h",
        "optional_of_impl.h",
        "overlap_of_impl.h",
        "serialization_helpers.h",
        "smart_pointer_of_impl.h",
        "special_values.h",
        "unique_elements_container_of_impl.h",
        "value_mutation_helpers.h",
        "variant_of_impl.h",
    ],
    deps = [
        ":absl_helpers",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/log:absl_log",
        "@abseil-cpp//absl/numeric:bits",
        "@abseil-cpp//absl/numeric:int128",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest:fuzzing_bit_gen",
        "@com_google_fuzztest//fuzztest/internal:any",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:meta",
        "@com_google_fuzztest//fuzztest/internal:printer",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:status",
        "@com_google_fuzztest//fuzztest/internal:table_of_recent_compares",
        "@com_google_fuzztest//fuzztest/internal:type_support",
    ],
)

cc_library(
    name = "domains_impl",
    hdrs = [
        "domain_base.h",
        "domain_type_erasure.h",
    ],
    deps = [
        ":core_domains_impl",
        ":in_grammar_impl",
        ":in_regexp_impl",
        ":protobuf_domain_impl",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@com_google_fuzztest//fuzztest/internal:any",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:meta",
        "@com_google_fuzztest//fuzztest/internal:printer",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:status",
        "@com_google_fuzztest//fuzztest/internal:type_support",
    ],
)

cc_library(
    name = "in_grammar_impl",
    srcs = ["in_grammar_impl.cc"],
    hdrs = ["in_grammar_impl.h"],
    # Public for users of the `cc_fuzztest_grammar_library`.
    visibility = ["//visibility:public"],
    deps = [
        ":core_domains_impl",
        ":in_regexp_impl",
        ":regexp_dfa",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:meta",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:type_support",
    ],
)

cc_library(
    name = "in_regexp_impl",
    srcs = ["in_regexp_impl.cc"],
    hdrs = ["in_regexp_impl.h"],
    deps = [
        ":core_domains_impl",
        ":regexp_dfa",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:status",
        "@com_google_fuzztest//fuzztest/internal:type_support",
    ],
)

cc_library(
    name = "protobuf_domain_impl",
    hdrs = ["protobuf_domain_impl.h"],
    deps = [
        ":core_domains_impl",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@com_google_fuzztest//fuzztest/internal:any",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:meta",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:status",
        "@com_google_fuzztest//fuzztest/internal:type_support",
    ],
)

cc_library(
    name = "regexp_dfa",
    srcs = ["regexp_dfa.cc"],
    hdrs = ["regexp_dfa.h"],
    deps = [
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@re2",
    ],
)

cc_library(
    name = "utf",
    srcs = [
        "rune.cc",
        "rune.h",
        "utf.cc",
    ],
    hdrs = ["utf.h"],
    deps = ["@abseil-cpp//absl/strings:string_view"],
)
