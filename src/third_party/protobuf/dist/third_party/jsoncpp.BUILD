licenses(["unencumbered"])  # Public Domain or MIT

exports_files(["LICENSE"])

cc_library(
    name = "jsoncpp",
    srcs = [
        "src/lib_json/json_reader.cpp",
        "src/lib_json/json_tool.h",
        "src/lib_json/json_value.cpp",
        "src/lib_json/json_writer.cpp",
    ],
    hdrs = [
        "include/json/allocator.h",
        "include/json/assertions.h",
        "include/json/config.h",
        "include/json/forwards.h",
        "include/json/json.h",
        "include/json/json_features.h",
        "include/json/reader.h",
        "include/json/value.h",
        "include/json/version.h",
        "include/json/writer.h",
    ],
    copts = [
        "-DJSON_USE_EXCEPTION=0",
        "-DJSON_HAS_INT64",
    ],
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":private"],
)

cc_library(
    name = "private",
    textual_hdrs = ["src/lib_json/json_valueiterator.inl"],
)
