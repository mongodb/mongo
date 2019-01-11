test.compile("source.cpp", '-fno-exceptions')
test.run_analysis_script('gcTypes')

hazards = test.load_hazards()
assert(len(hazards) == 0)

# If we compile with exceptions, then there *should* be a hazard because
# AutoSomething::AutoSomething might throw an exception, which would cause the
# partially-constructed value to be torn down, which will call ~RAII_GC.

test.compile("source.cpp", '-fexceptions')
test.run_analysis_script('gcTypes')

hazards = test.load_hazards()
assert(len(hazards) == 1)
hazard = hazards[0]
assert(hazard.function == 'void f()')
assert(hazard.variable == 'thing')
assert("AutoSomething::AutoSomething" in hazard.GCFunction)
