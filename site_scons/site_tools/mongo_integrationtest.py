"""
Pseudo-builders for building and registering integration tests.
"""
from SCons.Script import Action


def exists(env):
    return True


def build_cpp_integration_test(env, target, source, **kwargs):
    libdeps = kwargs.get("LIBDEPS", [])
    libdeps.append("$BUILD_DIR/mongo/unittest/integration_test_main")

    kwargs["LIBDEPS"] = libdeps
    integration_test_components = {"tests", "integration-tests"}

    if "AIB_COMPONENT" in kwargs and not kwargs["AIB_COMPONENT"].endswith("-test"):
        kwargs["AIB_COMPONENT"] += "-test"

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        kwargs["AIB_COMPONENTS_EXTRA"] = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            integration_test_components
        )
    else:
        kwargs["AIB_COMPONENTS_EXTRA"] = integration_test_components

    result = env.Program(target, source, **kwargs)
    env.RegisterTest("$INTEGRATION_TEST_LIST", result[0])
    env.Alias("$INTEGRATION_TEST_ALIAS", result[0])

    return result


def generate(env):
    env.TestList("$INTEGRATION_TEST_LIST", source=[])
    env.AddMethod(build_cpp_integration_test, "CppIntegrationTest")
    env.Alias("$INTEGRATION_TEST_ALIAS", "$INTEGRATION_TEST_LIST")
