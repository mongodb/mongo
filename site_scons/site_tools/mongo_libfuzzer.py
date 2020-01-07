"""Pseudo-builders for building and registering libfuzzer tests.
"""
from SCons.Script import Action


def exists(env):
    return True


def libfuzzer_test_list_builder_action(env, target, source):
    with open(str(target[0]), "w") as ofile:
        for s in _libfuzzer_tests:
            print("\t" + str(s))
            ofile.write("%s\n" % s)


def build_cpp_libfuzzer_test(env, target, source, **kwargs):
    myenv = env.Clone()
    if not myenv.IsSanitizerEnabled("fuzzer"):
        return []

    libdeps = kwargs.get("LIBDEPS", [])
    kwargs["LIBDEPS"] = libdeps
    kwargs["INSTALL_ALIAS"] = ["tests"]
    sanitizer_option = "-fsanitize=fuzzer"
    myenv.Prepend(LINKFLAGS=[sanitizer_option])

    libfuzzer_test_components = {"tests", "fuzzertests"}
    if "AIB_COMPONENT" in kwargs and not kwargs["AIB_COMPONENTS"].endswith(
        "-fuzzertest"
    ):
        kwargs["AIB_COMPONENT"] += "-fuzzertest"

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        libfuzzer_test_components = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            libfuzzer_test_components
        )

    kwargs["AIB_COMPONENTS_EXTRA"] = libfuzzer_test_components

    result = myenv.Program(target, source, **kwargs)
    myenv.RegisterTest("$LIBFUZZER_TEST_LIST", result[0])
    myenv.Alias("$LIBFUZZER_TEST_ALIAS", result)

    # TODO: remove when hygienic is default
    hygienic = myenv.GetOption("install-mode") == "hygienic"
    if not hygienic:
        myenv.Install("#/build/libfuzzer_tests/", result[0])

    return result


def generate(env):
    env.TestList("$LIBFUZZER_TEST_LIST", source=[])
    env.AddMethod(build_cpp_libfuzzer_test, "CppLibfuzzerTest")
    env.Alias("$LIBFUZZER_TEST_ALIAS", "$LIBFUZZER_TEST_LIST")
