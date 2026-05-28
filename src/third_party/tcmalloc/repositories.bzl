"""Repository rules for vendored tcmalloc."""

_MONGO_TEST_EXPORTS_BZL = """\"\"\"Exports upstream tcmalloc test targets for Mongo wrappers.\"\"\"

def _is_exported_test_rule(rule_name, rule):
    if rule_name.startswith("mongo_export_"):
        return False
    if "actual" in rule:
        return False
    if "tests" in rule:
        return False
    if rule_name.endswith("_fuzz_test") or "_fuzz_test_" in rule_name:
        return False
    if rule_name.endswith("_benchmark") or "_benchmark_" in rule_name:
        return False
    return rule_name.endswith("_test") or "_test_" in rule_name

def mongo_export_current_package_tests(name = None):
    \"\"\"Publishes public aliases for the current package's upstream test executables.

    Args:
      name: Unused compatibility parameter required by buildifier for public macros.
    \"\"\"
    if name != None:
        fail("mongo_export_current_package_tests() does not take a name")

    existing_rules = dict(native.existing_rules())
    for rule_name in sorted(existing_rules):
        rule = existing_rules[rule_name]
        if not _is_exported_test_rule(rule_name, rule):
            continue
        native.alias(
            name = "mongo_export_" + rule_name,
            actual = ":" + rule_name,
            visibility = ["//visibility:public"],
        )
"""

def _source_path(root, rel):
    path = root
    for part in rel.split("/"):
        path = path.get_child(part)
    return path

def _replace_once(content, old, new, path):
    if old not in content:
        fail("Failed to patch " + path + ": expected snippet not found")
    return content.replace(old, new, 1)

def _append_export_macro(content):
    return content.rstrip() + "\n\nmongo_export_current_package_tests()\n"

def _patch_top_build(content):
    content = _replace_once(
        content,
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\n',
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\nload("//tcmalloc:mongo_test_exports.bzl", "mongo_export_current_package_tests")\n',
        "tcmalloc/BUILD",
    )
    content = content.replace(
        'copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,',
        'copts = TCMALLOC_DEFAULT_COPTS,',
    )
    return _append_export_macro(content)

def _patch_internal_build(content):
    content = _replace_once(
        content,
        'load("@rules_cc//cc:defs.bzl", "cc_proto_library")\n',
        'load("@com_google_protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")\n',
        "tcmalloc/internal/BUILD",
    )
    content = _replace_once(
        content,
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\n',
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\nload("//tcmalloc:mongo_test_exports.bzl", "mongo_export_current_package_tests")\n',
        "tcmalloc/internal/BUILD",
    )
    return _append_export_macro(content)

def _patch_testing_build(content):
    content = _replace_once(
        content,
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\n',
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\nload("//tcmalloc:mongo_test_exports.bzl", "mongo_export_current_package_tests")\n',
        "tcmalloc/testing/BUILD",
    )
    return _append_export_macro(content)

def _patch_selsan_build(content):
    content = _replace_once(
        content,
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\n',
        'load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")\nload("//tcmalloc:mongo_test_exports.bzl", "mongo_export_current_package_tests")\n',
        "tcmalloc/selsan/BUILD",
    )
    return _append_export_macro(content)

def _patch_copts(content):
    content = _replace_once(
        content,
        '    "-Wno-sign-compare",\n    "-Wno-uninitialized",\n',
        '    "-Wno-sign-compare",\n    "-Wno-thread-safety-analysis",\n    "-Wno-macro-redefined",\n    "-Wno-uninitialized",\n',
        "tcmalloc/copts.bzl",
    )
    content = _replace_once(
        content,
        '    "-Wno-unused-variable",\n]\n',
        '    "-Wno-unused-variable",\n    "-std=c++17",\n]\n',
        "tcmalloc/copts.bzl",
    )
    content = _replace_once(
        content,
        '    "-Wno-unused-variable",\n]\n',
        '    "-Wno-unused-variable",\n    "-std=c++17",\n]\n',
        "tcmalloc/copts.bzl",
    )
    return content

_PATCHED_FILES = {
    "tcmalloc/BUILD": _patch_top_build,
    "tcmalloc/internal/BUILD": _patch_internal_build,
    "tcmalloc/testing/BUILD": _patch_testing_build,
    "tcmalloc/selsan/BUILD": _patch_selsan_build,
    "tcmalloc/copts.bzl": _patch_copts,
}

def _mirror_tree(ctx, src_root):
    for path in src_root.readdir():
        if path.basename in [
            "MODULE.bazel",
            "WORKSPACE",
            "tcmalloc",
        ]:
            continue
        ctx.symlink(path, path.basename)

    tcmalloc_root = src_root.get_child("tcmalloc")
    for path in tcmalloc_root.readdir():
        rel = "tcmalloc/" + path.basename
        if rel in _PATCHED_FILES or path.basename in [
            "internal",
            "selsan",
            "testing",
        ]:
            continue
        ctx.symlink(path, rel)

    for package_name in ["internal", "selsan", "testing"]:
        package_root = tcmalloc_root.get_child(package_name)
        for path in package_root.readdir():
            rel = "tcmalloc/" + package_name + "/" + path.basename
            if rel in _PATCHED_FILES:
                continue
            ctx.symlink(path, rel)

def _mongo_tcmalloc_repository_impl(ctx):
    src_root = ctx.path(ctx.workspace_root).get_child("src").get_child("third_party").get_child("tcmalloc").get_child("dist")

    _mirror_tree(ctx, src_root)

    for rel, patcher in _PATCHED_FILES.items():
        content = ctx.read(_source_path(src_root, rel))
        ctx.file(rel, patcher(content))

    ctx.file("tcmalloc/mongo_test_exports.bzl", _MONGO_TEST_EXPORTS_BZL)

mongo_tcmalloc_repository = repository_rule(
    implementation = _mongo_tcmalloc_repository_impl,
    local = True,
    configure = True,
)
