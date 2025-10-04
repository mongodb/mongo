# 'test' is provided by the calling script.
# flake8: noqa: F821

test.compile("source.cpp")
test.run_analysis_script("gcTypes")

info = test.load_typeInfo()

gcFunctions = test.load_gcFunctions()

f = "void f(int32)"
g = "void g(int32)"
h = "void h(int32)"

assert f in gcFunctions
assert g in gcFunctions
assert h in gcFunctions
assert "void leaf()" not in gcFunctions
assert "void nonrecursive_root()" in gcFunctions

callgraph = test.load_callgraph()
assert callgraph.calleeGraph[f][g]
assert callgraph.calleeGraph[f][h]
assert callgraph.calleeGraph[g][f]
assert callgraph.calleeGraph[g][h]

node = ["void n{}(int32)".format(i) for i in range(10)]
mnode = [callgraph.unmangledToMangled.get(f) for f in node]
for src, dst in [
    (1, 2),
    (2, 1),
    (4, 5),
    (5, 4),
    (2, 3),
    (5, 3),
    (3, 6),
    (6, 7),
    (7, 8),
    (8, 7),
    (8, 9),
]:
    assert callgraph.calleeGraph[node[src]][node[dst]]

funcInfo = test.load_funcInfo()
rroots = set(
    [
        callgraph.mangledToUnmangled[f]
        for f in funcInfo
        if funcInfo[f].get("recursive_root")
    ]
)
assert len(set([node[1], node[2]]) & rroots) == 1
assert len(set([node[4], node[5]]) & rroots) == 1
assert len(rroots) == 4, "rroots = {}".format(rroots)  # n1, n4, f, self_recursive
