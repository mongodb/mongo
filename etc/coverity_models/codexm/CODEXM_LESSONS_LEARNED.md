# CodeXM Lessons Learned

Compiled during development of `network_size_unchecked_alloc.cxm` (CVE-2025-14847 pattern detection)
in Coverity 2025.6.0 against MongoDB's C++ codebase.

---

## 1. The rendered HTML documentation is wrong

**The single biggest pitfall.** The official Coverity HTML documentation renders code examples with
`endif` stripped. The `insideTry` function shown in the docs appears as:

```
function insideTry(n : astnode) : bool ->
    if n.parent matches NonNull as p then
        switch(p) {
        | tryStatement -> true
        | default -> insideTry(p)
        }
    else
        false
```

The raw HTML source actually contains `endif;` after `false`. Without it, the parser fails with
`if-expression may not be terminated by 'endif'`. The correct form is:

```
function insideTry(n : astnode) : bool ->
    if n.parent matches NonNull as p then
        switch(p) {
        | tryStatement -> true
        | default -> insideTry(p)
        }
    else
        false
    endif;
```

**Always read the raw HTML source when in doubt about code examples.**

---

## 2. Function definitions require a trailing `;`

```
// WRONG (parser says "unexpected 'checker', expecting ';'")
function myFn(n : astnode) : bool ->
    true

checker { ... }

// CORRECT
function myFn(n : astnode) : bool ->
    true;

checker { ... }
```

---

## 3. `if-expressions` require `endif` before the function-terminating `;`

```
// WRONG — ';' terminates the if-expression, which is not allowed
function enclosingFn(n : astnode) : astnode? ->
    if n matches functionDefinition then n
    elsif n.parent matches NonNull as p then enclosingFn(p)
    else null;

// CORRECT — 'endif' closes the if, then ';' terminates the function
function enclosingFn(n : astnode) : astnode? ->
    if n matches functionDefinition then n
    elsif n.parent matches NonNull as p then enclosingFn(p)
    else null
    endif;
```

---

## 4. `where` is not valid inside pattern decomposition blocks `{ }`

Inside `{ .prop == value }` property decompositions, use only chained property access. The `where`
keyword for additional conditions is only valid at the `for`/`exists` level.

```
// WRONG — 'where' not allowed inside { }
anyMatchingCodeIn(
    simpleStatement {
        .expression == functionCall as call
        where call.calledFunction.identifier == "allocate"  // parse error
    },
    fn.body
)

// CORRECT — nest the property constraint directly
anyMatchingCodeIn(
    simpleStatement {
        .expression == functionCall {
            .calledFunction.identifier == "allocate"
        }
    },
    fn.body
)
```

---

## 5. `globalset` iterators do NOT inherit outer-scope variables

Variables bound in an outer `for code in globalset allFunctionCode` loop are **not** accessible
inside a nested `exists ... in globalset allFunctionCode`. This is a hard constraint, not a subtle
bug.

```
// FAILS — varDecl is undefined inside the globalset exists
for mr in globalset allFunctionCode % memberReference
where innermostOwner(variableDeclaration, mr) matches variableDeclaration as varDecl
&& exists allocStmt in globalset allFunctionCode   // NEW globalset scope
   where allocStmt matches variableReference as vr
   && vr.variable == varDecl.variable              // ERROR: varDecl undefined
```

**Solution**: Use `allCodeIn(someNode)` which returns `set<astnode>` (not a globalset). Iterating
over a regular set **does** inherit outer-scope variables.

```
// WORKS — allCodeIn returns a set, not a globalset
for fn in globalset allFunctionDefinitions
where anyMatchingCodeIn(memberReference { ... }, fn.body) matches NonNull as mr
&& innermostOwner(variableDeclaration, mr) matches variableDeclaration as varDecl
&& exists sizeArg in allocCall.argumentList   // list — inherits varDecl
   where sizeArg matches variableReference as vr
   && vr.variable == varDecl.variable         // OK
```

---

## 6. Variables bound inside `exists` are NOT accessible in the `events` block

```
// FAILS — allocStmt is out of scope in events
for fn in globalset allFunctionDefinitions
where exists allocNode in allCodeIn(fn.body)
   where allocNode matches simpleStatement as allocStmt
   && ...
:
{ events = [{ location = allocStmt.location; }]; };  // ERROR: undefined
```

**Solution**: Use `anyMatchingCodeIn` in the outer `where` clause. It returns the matched node into
the outer scope via `matches NonNull as varName`:

```
// WORKS — allocCall is bound in the outer for-loop scope
for fn in globalset allFunctionDefinitions
where anyMatchingCodeIn(
    functionCall { .calledFunction.identifier == "allocate" },
    fn.body
) matches NonNull as allocCall
:
{ events = [{ location = allocCall.location; }]; };  // OK
```

---

## 7. Static method calls are NOT `simpleStatement` expressions

C++ static method calls that initialize a variable are `variableDeclaration` nodes, not
`simpleStatement` nodes:

```cpp
auto buf = SharedBuffer::allocate(bufferSize);  // variableDeclaration, not simpleStatement
allocate(bufferSize);                           // simpleStatement (standalone call)
```

To find calls to a static method regardless of context, match `functionCall` directly rather than
`simpleStatement { .expression == functionCall }`:

```
// MISSES the assignment case
anyMatchingCodeIn(
    simpleStatement { .expression == functionCall { .calledFunction.identifier == "allocate" } },
    fn.body
)

// FINDS it in all contexts (assignment, declaration, standalone)
anyMatchingCodeIn(
    functionCall { .calledFunction.identifier == "allocate" },
    fn.body
)
```

---

## 8. `functionDefinition` cannot be used as a pattern argument to `innermostOwner`

`innermostOwner(pattern, node)` accepts pattern types like `statement`, `variableDeclaration`,
`memberReference`. It does NOT accept `functionDefinition`:

```
// FAILS — "Cannot convert functionDefinition to astnode"
innermostOwner(functionDefinition, allocStmt)

// WORKS for all other tested pattern types
innermostOwner(variableDeclaration, mr)    // OK
innermostOwner(memberReference, sizeArg)   // OK
```

To find the enclosing function, use `allFunctionDefinitions` as the outer loop and
`allCodeIn(fn.body)` to scope searches within it. This avoids the need to traverse `.parent` to a
`functionDefinition` at all.

---

## 9. `functionDefinition` cannot be matched in an `if` condition (use `switch` or restructure)

```
// FAILS — "Cannot match a value of type astnode with a pattern matching functionDefinition"
if n matches functionDefinition as fn then fn

// WORKS inside a switch pattern arm
switch(n) {
| functionDefinition as fn -> fn
| default -> ...
}
```

If you need to check whether a node is a `functionDefinition` in an if-condition, restructure to use
`allFunctionDefinitions` as the primary iteration and `allCodeIn(fn.body)` to confine other searches
to the same function.

---

## 10. `anyMatchingCodeIn` vs `containsMatch` vs `exists in allCodeIn`

| Mechanism                          | Returns                     | Variables in scope                  | Use when                                                                 |
| ---------------------------------- | --------------------------- | ----------------------------------- | ------------------------------------------------------------------------ |
| `anyMatchingCodeIn(pattern, code)` | `T?` (first match, or null) | Outer scope can use result          | You need the MATCHED NODE in scope for events or further checks          |
| `containsMatch(pattern, code)`     | `bool`                      | No binding                          | You only need to know IF a match exists                                  |
| `exists node in allCodeIn(code)`   | `bool`                      | Inner scope can use outer variables | You need to use outer variables INSIDE the check (e.g., compare symbols) |

The scoping rule: `allCodeIn` returns a regular set, so `exists node in allCodeIn(x)` inherits all
outer-scope variables. This is the only way to express "find a code node that satisfies a condition
involving an outer-scope variable."

---

## 11. Effective pattern for same-function scoping

The recommended pattern for writing checkers that need same-function scoping:

```
for fn in globalset allFunctionDefinitions

// Step 1: Find pattern A in the function — binds result in outer scope
where anyMatchingCodeIn(patternA, fn.body) matches NonNull as resultA

// Step 2: Derive additional context from result A
&& innermostOwner(someDeclaration, resultA) matches someDeclaration as derived

// Step 3: Find pattern B in the SAME function — can use resultA and derived
// because allCodeIn returns a set (not globalset)
&& anyMatchingCodeIn(patternB, fn.body) matches NonNull as resultB

// Step 4: Connect A and B via symbol equality (list exists inherits outer scope)
&& exists argX in resultB.argumentList
   where argX matches variableReference as vr
   && vr.variable == derived.variable
:
{ events = [...]; };
```

---

## 12. Security Directives for C/C++ taint are extremely limited

**Background** (discovered before CodeXM work):

Coverity's Security Directives (`--directive-file`) support taint sources/sinks for Java/managed
languages only. For C-like (C/C++/Objective-C) in format_version 12:

- `sink_for_checker`: **Java only** — parse error in C-like context
- `method_returns_tainted_data`: **Java only**
- `simple_entry_point`: **Java only**
- `dc_checker_name` / `method_set_for_dc_checker`: C/C++ supported but **deprecated** since 2020.03
  and functionally a no-op in 2025.6.0

**CodeXM is the correct mechanism** for custom C/C++ taint-like pattern detection, and
`__coverity_mark_pointee_as_tainted__` in user model files marks data as "potentially uninitialized"
(affects UNINIT) but does NOT make TAINTED_SCALAR fire.

---

## 13. CodeXM language summary (version 2025.6.0)

```
// Include the C/C++ language context
include `C/C++`;

// Function definition (always ends with the body expression followed by ';')
function name(param : type) : returnType ->
    expression;

// if-expression (requires 'endif' before the terminating ';')
function hasParent(n : astnode) : bool ->
    if n.parent matches NonNull then true
    else false
    endif;

// Recursive function with elsif
function depth(n : astnode) : int ->
    if n.parent matches NonNull as p then 1 + depth(p)
    else 0
    endif;

// Checker structure
checker {
    name = "MY.CHECKER";
    reports =
        for code in globalset allFunctionCode
        where code matches gotoStatement :
        {
            events = [{
                description = "goto found";
                location = code.location;
            }];
        };
};
```

---

## 14. Useful CodeXM library functions

| Function                           | Returns        | Notes                                                                                                                             |
| ---------------------------------- | -------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `allCodeIn(node)`                  | `set<astnode>` | All code within node; inherits outer scope in exists                                                                              |
| `anyMatchingCodeIn(pattern, node)` | `T?`           | First match or null; binds result in outer scope via `matches NonNull`                                                            |
| `containsMatch(pattern, node)`     | `bool`         | Existence check only; no binding                                                                                                  |
| `innermostOwner(pattern, node)`    | `astnode?`     | Nearest ancestor matching pattern; works for `variableDeclaration`, `memberReference`, `statement` etc.; NOT `functionDefinition` |
| `ownerStatement(code)`             | `astnode?`     | The enclosing statement                                                                                                           |

---

## 15. Recommended approach for writing new C/C++ checkers

1. **Start minimal**: write a checker that finds ONE piece of the pattern using
   `for code in globalset allFunctionCode % patternType`. Verify it fires.

2. **Add function scoping**: switch to `for fn in globalset allFunctionDefinitions` with
   `anyMatchingCodeIn(pattern, fn.body)` to scope to a single function.

3. **Build the full pattern**: add additional conditions using `anyMatchingCodeIn` for each pattern
   piece you need in the events block, and `exists ... in allCodeIn` or `exists ... in list` for
   conditions that don't need their results in events.

4. **Test incrementally**: each time you add a condition, verify the checker still parses (no syntax
   errors) and produces the expected number of findings.

5. **Read the raw HTML docs**: for any syntax question, search the raw `cov_codexm.html` file rather
   than the rendered documentation.
