"""Pseudo-builders for building and registering unit tests.
"""

def exists(env):
    return True

def register_unit_test(env, test):
    env._UnitTestList('$UNITTEST_LIST', test)
    env.Alias('$UNITTEST_ALIAS', test)

def register_module_test(env, test):
    env._UnitTestList('$MODULETEST_LIST', test)
    env.Alias('$MODULETEST_ALIAS', test)

def unit_test_list_builder_action(env, target, source):
    print "Generating " + str(target[0])
    ofile = open(str(target[0]), 'wb')
    try:
        for s in source:
            print '\t' + str(s)
            ofile.write('%s\n' % s)
    finally:
        ofile.close()

def build_cpp_unit_test(env, target, source, **kwargs):
    libdeps = kwargs.get('LIBDEPS', [])
    libdeps.append( '$BUILD_DIR/mongo/unittest/unittest_main' )
    
    includeCrutch = True
    if "NO_CRUTCH" in kwargs:
        includeCrutch = not kwargs["NO_CRUTCH"]

    if includeCrutch:
        libdeps.append( '$BUILD_DIR/mongo/unittest/unittest_crutch' )

    kwargs['LIBDEPS'] = libdeps

    result = env.Program(target, source, **kwargs)
    env.RegisterUnitTest(result[0])
    env.Install("#/build/unittests/", target)
    return result

def generate(env):
    unit_test_list_builder = env.Builder(action=unit_test_list_builder_action, multi=True)
    env.Append(BUILDERS=dict(_UnitTestList=unit_test_list_builder))
    env.AddMethod(register_unit_test, 'RegisterUnitTest')
    env.AddMethod(build_cpp_unit_test, 'CppUnitTest')
    env.AddMethod(register_module_test, 'RegisterModuleTest')
    env.Alias('$UNITTEST_ALIAS', '$UNITTEST_LIST')
    env.Alias('$MODULETEST_ALIAS', '$MODULETEST_LIST')
    env.RegisterUnitTest([])
    env.RegisterModuleTest([])
