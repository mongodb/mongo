"""Pseudo-builders for building and registering integration tests.
"""
from SCons.Script import Action

def exists(env):
    return True

_integration_tests = []
def register_integration_test(env, test):
    installed_test = env.Install("#/build/integration_tests/", test)
    _integration_tests.append(installed_test[0].path)
    env.Alias('$INTEGRATION_TEST_ALIAS', installed_test)

def integration_test_list_builder_action(env, target, source):
    ofile = open(str(target[0]), 'wb')
    try:
        for s in _integration_tests:
            print '\t' + str(s)
            ofile.write('%s\n' % s)
    finally:
        ofile.close()

def build_cpp_integration_test(env, target, source, **kwargs):
    libdeps = kwargs.get('LIBDEPS', [])
    libdeps.append( '$BUILD_DIR/mongo/unittest/integration_test_main' )

    kwargs['LIBDEPS'] = libdeps

    result = env.Program(target, source, **kwargs)
    env.RegisterIntegrationTest(result[0])
    return result

def generate(env):
    env.Command('$INTEGRATION_TEST_LIST', env.Value(_integration_tests),
            Action(integration_test_list_builder_action, "Generating $TARGET"))
    env.AddMethod(register_integration_test, 'RegisterIntegrationTest')
    env.AddMethod(build_cpp_integration_test, 'CppIntegrationTest')
    env.Alias('$INTEGRATION_TEST_ALIAS', '$INTEGRATION_TEST_LIST')
