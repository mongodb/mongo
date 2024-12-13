load("//bazel:mongo_src_rules.bzl", "mongo_cc_library", "mongo_cc_proto_library")

OTEL_COPTS = [
    "-D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING",
] + select({
    "//bazel/config:gcc_or_clang": [
        "-Wno-sign-compare",
        "-Wno-comment",
        "-Wno-implicit-fallthrough",
    ],
    "//conditions:default": [],
}) + select({
    "//bazel/config:compiler_type_gcc": [
        "-Wno-stringop-overread",  # false positive: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=98465#c30
        "-Wno-stringop-overflow",
        "-Wno-attributes",
        #"-Wno-class-memaccess",
        #"-Wno-overloaded-virtual",
        "-Wno-error",
    ],
    "//bazel/config:compiler_type_msvc": [
        "/wd4334",  # '<<': result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
        "/wd4116",  # unnamed type definition in parentheses
        "/wd4146",  # unary minus operator applied to unsigned type, result still unsigned
        "/wd4715",  # not all control paths return a value
        "/wd4200",  # nonstandard extension used: zero-sized array in struct/union
        "/wd4312",  # 'reinterpret_cast': conversion from 'unsigned int' to 'void *' of greater size
        "/wd4090",  # 'function': different 'const' qualifiers
    ],
    "//conditions:default": [],
})

OTEL_TARGET_COMPATIBLE_WITH = select({
    "//bazel/config:build_otel_enabled": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def mongo_cc_proto_lib(
        name,
        deps):
    proto_cc_name = name + "_raw_proto"
    mongo_cc_proto_library(
        name = proto_cc_name,
        deps = deps,
    )

    mongo_cc_library(
        name = name,
        cc_deps = [":" + proto_cc_name],
        copts = OTEL_COPTS,
        deps = [
            "//src/third_party/protobuf",
        ],
    )
