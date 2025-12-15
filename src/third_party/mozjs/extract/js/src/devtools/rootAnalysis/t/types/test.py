# flake8: noqa: F821


test.compile("source.cpp")
test.run_analysis_script()
hazards = test.load_hazards()
hazmap = {haz.variable: haz for haz in hazards}
assert "arg1" in hazmap
assert "arg2" in hazmap
assert "unsafe1" not in hazmap
assert "unsafe2" in hazmap
assert "unsafe3" not in hazmap
assert "unsafe4" in hazmap
assert "unsafe5" in hazmap
assert "safe6" not in hazmap
