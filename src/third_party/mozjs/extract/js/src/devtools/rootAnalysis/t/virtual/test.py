# 'test' is provided by the calling script.
# flake8: noqa: F821

test.compile("source.cpp")
test.run_analysis_script("gcTypes")

info = test.load_typeInfo()

assert "Sub1" in info["OtherCSUTags"]
assert ["CSU1", "CSU2"] == sorted(info["OtherCSUTags"]["Sub1"])
assert "Base" in info["OtherFieldTags"]
assert "someGC" in info["OtherFieldTags"]["Base"]
assert "Sub1" in info["OtherFieldTags"]
assert "someGC" in info["OtherFieldTags"]["Sub1"]

# For now, fields with the same name (eg overloaded virtual methods) just
# accumulate attributes.
assert ["Sub1 override", "Sub1 override for int overload", "second attr"] == sorted(
    info["OtherFieldTags"]["Sub1"]["someGC"]
)

gcFunctions = test.load_gcFunctions()

assert "void Sub1::noneGC()" not in gcFunctions
assert "void Sub1::someGC()" not in gcFunctions
assert "void Sub1::someGC(int32)" not in gcFunctions
assert "void Sub1::allGC()" in gcFunctions
assert "void Sub2::noneGC()" not in gcFunctions
assert "void Sub2::someGC()" in gcFunctions
assert "void Sub2::someGC(int32)" in gcFunctions
assert "void Sub2::allGC()" in gcFunctions

callgraph = test.load_callgraph()

assert callgraph.calleeGraph["void f()"]["Super.noneGC:0"]
assert callgraph.calleeGraph["Super.noneGC:0"]["Sub1.noneGC:0"]
assert callgraph.calleeGraph["Super.noneGC:0"]["Sub2.noneGC:0"]
assert callgraph.calleeGraph["Sub1.noneGC:0"]["void Sub1::noneGC()"]
assert callgraph.calleeGraph["Sub2.noneGC:0"]["void Sub2::noneGC()"]
assert "void Sibling::noneGC()" not in callgraph.calleeGraph["Super.noneGC:0"]
assert callgraph.calleeGraph["Super.onBase:0"]["Sub1.onBase:0"]
assert callgraph.calleeGraph["Sub1.onBase:0"]["void Sub1::onBase()"]
assert callgraph.calleeGraph["Super.onBase:0"]["void Base::onBase()"]
assert "void Sibling::onBase()" not in callgraph.calleeGraph["Super.onBase:0"]

hazards = test.load_hazards()
hazmap = {haz.variable: haz for haz in hazards}

assert "c1" not in hazmap
assert "c2" in hazmap
assert "c3" in hazmap
assert "c4" not in hazmap
assert "c5" in hazmap
assert "c6" in hazmap
assert "c7" not in hazmap
assert "c8" in hazmap
assert "c9" in hazmap
assert "c10" in hazmap
assert "c11" in hazmap

# Virtual resolution should take the static type into account: the only method
# implementations considered should be those of descendants, even if the
# virtual method is inherited and not overridden in the static class. (Base
# defines sibGC() as pure virtual, Super inherits it without overriding,
# Sibling and Sub2 both implement it.)

# Call Base.sibGC on a Super pointer: can only call Sub2.sibGC(), which does not GC.
# In particular, PEdgeCallInstance.Exp.Field.FieldCSU.Type = {Kind: "CSU", Name="Super"}
assert "c12" not in hazmap
# Call Base.sibGC on a Base pointer; can call Sibling.sibGC(), which GCs.
assert "c13" in hazmap

# Call nsISupports.danger() which is annotated to be overridable and hence can GC.
assert "c14" in hazmap

# someGC(int) overload
assert "c16" in hazmap
assert "c17" in hazmap

# Super.onBase() could call the GC'ing Base::onBase().
assert "c15" in hazmap

# virtual ~nsJSPrincipals calls ~JSPrincipals calls GC.
assert "c18" in hazmap
assert "c19" in hazmap

# ~SafePrincipals does not GC.
assert "c20" not in hazmap

# ...but when cast to a nsISupports*, the compiler can't tell that it won't.
assert "c21" in hazmap

# Function pointers! References to function pointers! Created by reference-capturing lambdas!
assert "c22" in hazmap
assert "c23" in hazmap
assert "c24" in hazmap
assert "c25" not in hazmap
assert "c26" not in hazmap
assert "c27" not in hazmap
