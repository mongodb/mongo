"""Pseudo-builders for building and registering integration tests.
"""

def exists(env):
    return True

def register_integration_test(env, test):
    installed_test = env.Install("#/build/integration_tests/", test)
    env['INTEGRATION_TEST_LIST_ENV']._IntegrationTestList('$INTEGRATION_TEST_LIST', installed_test)

def integration_test_list_builder_action(env, target, source):
    print "Generating " + str(target[0])
    ofile = open(str(target[0]), 'wb')
    try:
        for s in source:
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
    # Capture the top level env so we can use it to generate the integration test list file
    # indepenently of which environment CppUnitTest was called in. Otherwise we will get "Two
    # different env" warnings for the unit_test_list_builder_action.
    env['INTEGRATION_TEST_LIST_ENV'] = env;
    integration_test_list_builder = env.Builder(
        action=env.Action(integration_test_list_builder_action, "Generating $TARGET"),
        multi=True)
    env.Append(BUILDERS=dict(_IntegrationTestList=integration_test_list_builder))
    env.AddMethod(register_integration_test, 'RegisterIntegrationTest')
    env.AddMethod(build_cpp_integration_test, 'CppIntegrationTest')
    env.Alias('$INTEGRATION_TEST_ALIAS', "#/build/integration_tests/")
    env.Alias('$INTEGRATION_TEST_ALIAS', '$INTEGRATION_TEST_LIST')
