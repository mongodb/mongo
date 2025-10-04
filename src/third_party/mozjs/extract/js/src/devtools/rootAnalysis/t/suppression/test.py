# flake8: noqa: F821
test.compile("source.cpp")
test.run_analysis_script("gcTypes", upto="gcFunctions")

# The suppressions file uses mangled names.
info = test.load_funcInfo()
suppressed = [f for f, v in info.items() if v.get("limits", 0) | 1]

# Only one of these is fully suppressed (ie, *always* called within the scope
# of an AutoSuppressGC).
assert len(list(filter(lambda f: "suppressedFunction" in f, suppressed))) == 1
assert len(list(filter(lambda f: "halfSuppressedFunction" in f, suppressed))) == 0
assert len(list(filter(lambda f: "unsuppressedFunction" in f, suppressed))) == 0

# gcFunctions should be the inverse, but we get to rely on unmangled names here.
gcFunctions = test.load_gcFunctions()
assert "void GC()" in gcFunctions
assert "void suppressedFunction()" not in gcFunctions
assert "void halfSuppressedFunction()" in gcFunctions
assert "void unsuppressedFunction()" in gcFunctions
assert "void f()" in gcFunctions
