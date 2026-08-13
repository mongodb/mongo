load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load(":install_rules.bzl", "mongo_install_rule")

def _transitive_source_input_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "MongoInstallRule"
    ]
    asserts.equals(env, 1, len(actions))
    input_basenames = [file.basename for file in actions[0].inputs.to_list()]
    asserts.true(
        env,
        ctx.attr.expected_source in input_basenames,
        "a parent install action must declare original files named by child depfiles",
    )
    return analysistest.end(env)

_transitive_source_input_test = analysistest.make(
    _transitive_source_input_test_impl,
    attrs = {
        "expected_source": attr.string(mandatory = True),
    },
)

def install_rules_test_suite(name):
    source_name = name + "_source"
    native.genrule(
        name = source_name + "_gen",
        outs = [source_name],
        cmd = "touch $@",
        testonly = True,
    )
    mongo_install_rule(
        name = name + "_leaf",
        srcs = [":" + source_name + "_gen"],
        deps = [],
        debug = "",
        root_files = {},
        include_files = {},
        publish_debug_in_stripped = False,
        create_dwp = False,
        testonly = True,
    )
    mongo_install_rule(
        name = name + "_root",
        srcs = [],
        deps = [":" + name + "_leaf"],
        debug = "",
        root_files = {},
        include_files = {},
        publish_debug_in_stripped = False,
        create_dwp = False,
        testonly = True,
    )
    _transitive_source_input_test(
        name = name,
        target_under_test = ":" + name + "_root",
        expected_source = source_name,
    )
