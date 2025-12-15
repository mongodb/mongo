# flake8: noqa: F821

from collections import defaultdict

test.compile("source.cpp")
test.run_analysis_script("gcTypes")

# gcFunctions should be the inverse, but we get to rely on unmangled names here.
gcFunctions = test.load_gcFunctions()
assert "void GC()" in gcFunctions
assert "void suppressedFunction()" not in gcFunctions
assert "void halfSuppressedFunction()" in gcFunctions
assert "void unsuppressedFunction()" in gcFunctions
assert "int32 Subcell::method()" in gcFunctions
assert "Cell* f()" in gcFunctions

hazards = test.load_hazards()
hazmap = {haz.variable: haz for haz in hazards}
assert "cell1" not in hazmap
assert "cell2" in hazmap
assert "cell3" in hazmap
assert "cell4" not in hazmap
assert "cell5" not in hazmap
assert "cell6" not in hazmap
assert "<returnvalue>" in hazmap
assert "this" in hazmap

assert hazmap["cell2"].function == "Cell* f()"

# Check that the correct GC call is reported for each hazard. (cell3 has a
# hazard from two different GC calls; it doesn't really matter which is
# reported.)
assert hazmap["cell2"].GCFunction == "void halfSuppressedFunction()"
assert hazmap["cell3"].GCFunction in (
    "void halfSuppressedFunction()",
    "void unsuppressedFunction()",
)
returnval_hazards = set(
    haz.function for haz in hazards if haz.variable == "<returnvalue>"
)
assert "Cell* f()" in returnval_hazards
assert "Cell* refptr_test1()" in returnval_hazards
assert "Cell* refptr_test2()" not in returnval_hazards
assert "Cell* refptr_test3()" in returnval_hazards
assert "Cell* refptr_test4()" in returnval_hazards
assert "Cell* refptr_test5()" not in returnval_hazards
assert "Cell* refptr_test6()" in returnval_hazards
assert "Cell* refptr_test7()" in returnval_hazards
assert "Cell* refptr_test8()" in returnval_hazards
assert "Cell* refptr_test9()" not in returnval_hazards

assert "container1" in hazmap
assert "container2" not in hazmap

# Type names are handy to have in the report.
assert hazmap["cell2"].type == "Cell*"
assert hazmap["<returnvalue>"].type == "Cell*"
assert hazmap["this"].type == "Subcell*"

# loopy hazards. See comments in source.
assert "haz1" not in hazmap
assert "haz2" not in hazmap
assert "haz3" in hazmap
assert "haz4" in hazmap
assert "haz5" in hazmap
assert "haz6" not in hazmap
assert "haz7" not in hazmap
assert "haz8" in hazmap

# safevals hazards. See comments in source.
assert "unsafe1" in hazmap
assert "safe2" not in hazmap
assert "unsafe3" in hazmap
assert "unsafe3b" in hazmap
assert "unsafe4" in hazmap
assert "safe5" not in hazmap
assert "safe6" not in hazmap
assert "unsafe7" in hazmap
assert "safe8" not in hazmap
assert "safe9" not in hazmap
assert "safe10" not in hazmap
assert "safe11" not in hazmap
assert "safe12" not in hazmap
assert "unsafe13" in hazmap
assert "unsafe14" in hazmap
assert "unsafe15" in hazmap
assert "safe16" not in hazmap
assert "safe17" not in hazmap
assert "safe18" not in hazmap
assert "safe19" not in hazmap

# method hazard.

byfunc = defaultdict(lambda: defaultdict(dict))
for haz in hazards:
    byfunc[haz.function][haz.variable] = haz

methhaz = byfunc["int32 Subcell::method()"]
assert "this" in methhaz
assert methhaz["this"].type == "Subcell*"

haz_functions = set(haz.function for haz in hazards)

# RefPtr<T> tests.

haz_functions = set(haz.function for haz in hazards)
assert "Cell* refptr_test1()" in haz_functions
assert "Cell* refptr_test2()" not in haz_functions
assert "Cell* refptr_test3()" in haz_functions
assert "Cell* refptr_test4()" in haz_functions
assert "Cell* refptr_test5()" not in haz_functions
assert "Cell* refptr_test6()" in haz_functions
assert "Cell* refptr_test7()" in haz_functions
assert "Cell* refptr_test8()" in haz_functions
assert "Cell* refptr_test9()" not in haz_functions
assert "Cell* refptr_test10()" in haz_functions

# aggr_init tests.

assert "void aggr_init_safe()" not in haz_functions
assert "void aggr_init_unsafe()" in haz_functions

# stack_array tests.

assert "void stack_array()" in haz_functions
haz_vars = byfunc["void stack_array()"]
assert "array" in haz_vars
assert "array2" not in haz_vars
