"""Pseudo-builders for building and registering benchmarks.
"""

def exists(env):
    return True

def register_benchmark(env, benchmark):
    installed_benchmark = env.Install("#/build/benchmarks/", benchmark)
    env['BENCHMARK_LIST_ENV']._BenchmarkList('$BENCHMARK_LIST', installed_benchmark)

def benchmark_list_builder_action(env, target, source):
    print "Generating " + str(target[0])
    ofile = open(str(target[0]), 'wb')
    try:
        for s in source:
            print '\t' + str(s)
            ofile.write('%s\n' % s)
    finally:
        ofile.close()

def build_cpp_benchmark(env, target, source, **kwargs):
    libdeps = kwargs.get('LIBDEPS', [])
    libdeps.append( '$BUILD_DIR/mongo/unittest/benchmarks_main' )

    # # Delete this if possible
    # includeCrutch = True
    # if "NO_CRUTCH" in kwargs:
    #     includeCrutch = not kwargs["NO_CRUTCH"]

    # if includeCrutch:
    #     libdeps.append( '$BUILD_DIR/mongo/unittest/unittest_crutch' )

    kwargs['LIBDEPS'] = libdeps

    result = env.Program(target, source, **kwargs)
    env.RegisterBenchmark(result[0])
    return result

def generate(env):
    # Capture the top level env so we can use it to generate the benchmark list file
    # indepenently of which environment CppBenchmark was called in. Otherwise we will get "Two
    # different env" warnings for the benchmark_list_builder_action.
    env['BENCHMARK_LIST_ENV'] = env;
    benchmark_list_builder = env.Builder(
        action=env.Action(benchmark_list_builder_action, "Generating $TARGET"),
        multi=True)
    env.Append(BUILDERS=dict(_BenchmarkList=benchmark_list_builder))
    env.AddMethod(register_benchmark, 'RegisterBenchmark')
    env.AddMethod(build_cpp_benchmark, 'CppBenchmark')
    env.Alias('$BENCHMARK_ALIAS', "#/build/benchmarks/")
    env.Alias('$BENCHMARK_ALIAS', '$BENCHMARK_LIST')
