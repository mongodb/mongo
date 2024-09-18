# Using `bson-dsl.h`

The header `<bson-dsl.h>` exposes an embedded domain-specific language
(DSL) built upon the C99 preprocessor. The DSL emits valid C99 and C++03 code
that can be used to inspect and construct `bson_t` objects.

The intent of the DSL is to simplify and streamline the process of working with
BSON data within C, which is otherwise slow, error-prone, and requires a lot of
boilerplate. The DSL presents a declarative style of working with BSON data.

The DSL can be broken down into two classes:

- [BSON writing](#generating-bson-data):
  - [`bsonBuild`](#bsonbuild)
  - [`bsonBuildDecl`](#bsonbuilddecl)
  - [`bsonBuildArray`](#bsonbuildarray)
  - [`bsonBuildAppend`](#bsonbuildappend)
- [BSON reading](#reading-bson-data)
  - [`bsonVisitEach`](#bsonvisiteach)
  - [`bsonParse`](#bsonparse)

The following DSL "grammatical elements" are defined:

- [*DocOperation*](#docoperation)
- [*ValueOperation*](#valueoperation)
- [*ArrayOperation*](#arrayoperation)
- [*ParseOperation*](#parseoperation)
- [*VisitOperation*](#visitoperation)
- [*Predicate*](#predicate)

**NOTE:** The `bson-dsl.h` header is *not* a public API. It is intended for
internal use and may be changed, broken, updated, or removed.

# Generating BSON Data

## Examples

### Empty

```c
bsonBuildDecl(empty);
```

Result:

```js
{}
```

### A Simple "okay"

```c
bsonBuildDecl(e, kv("okay", boolean(true)));
```

Result:

```js
{ okay: true }
```

### Nested Documents and Arrays

```c
bsonBuildDecl(e,
    kv("integer", int32(8)),
    kv("subdoc", doc(kv("foo", cstr("bar")), kv("baz", null))),
    kv("anArray", array(int32(1), int32(2), null, cstr("I am a string"))));
```

Result:

```js
{
  integer: 8,
  subdoc: { foo: "bar", "baz": null },
  anArray: [1, 2, null, "I am a string"]
}
```

### Create an Aggregate Command

```c
bson_t* make_aggregate(const char* coll,
                       const bson_t* pipeline_array,
                       int timeout_ms,
                       int batch_size,
                       const bson_t* comment,
                       bson_error_t* error) {
  bson_t* r = bson_new();
  bsonBuildAppend(r,
    kv("aggregate", cstr(coll)),
    kv("pipeline", bsonArray(*pipeline_array)),
    kv("cursor",
       doc(if(batch_size != -1,
              then(kv("batchSize", int32(batch_size)))))),
    if(timeout_ms > 0, then(kv("maxTimeMS", int32(timeout_ms))),
    if(comment, then(kv("comment", bson(*comment)))));
  // Check if any of the above failed:
  if (bsonBuildError) {
    bson_set_error(error,
                   ERROR_COND,
                   ERROR_CODE,
                   "Failed to build aggregate: %s",
                   bsonBuildError);
    bson_destroy(r);
    r = NULL;
  }
  return r;
}
```


## `bsonBuild`

> `bsonBuild(BSON, DocOperation... ops)`

Calls `bson_init(&BSON)`, followed by `bsonBuildAppend(BSON, ops...)`.


## `bsonBuildDecl`

> `bsonBuildDecl(VarName, DocOperation... ops)`

Declares a new `bson_t` named `VarName` and calls `bsonBuild(VarName, ops...)`.


## `bsonBuildAppend`

> `bsonBuildAppend(BSON, DocOperation... ops)`

Appends to an already-initialized `bson_t` given by `BSON` (which must be a
modifiable l-value of type `bson_t`).

For information on the meaning of `ops...`, See: [`DocOperation`](#DocOperation)


## `bsonBuildArray`

> `bsonBuildArray(VarName, ArrayOperation... ops)`

Appends to an already-initialized `bson_t` at `VarName`. Elements are inserted
using incrementally increasing integer string keys.

For more information, refer to [`ArrayOperation`](#ArrayOperation)


## Syntax

### *DocOperation*

The *DocOperations* are used to generate elements of a `bson_t` document or
subdocument thereof. The top-level `bsonBuildAppend()` and the `doc()`
*ValueOperation* both accept a list of *DocOperations*.

The following *DocOperations* are defined:

#### `kvl()`

> `kvl(const char* str, int len, ValueOperation val)`

The lowest-level *DocOperation*, generates a key-value pair in the document,
with the string begining at `str` for `len` characters defining the key, and
`val` defining the value of the element. (See:
[ValueOperation](#valueoperation))

If `str` is null-terminated, you can just use `kv()`

#### `kv()`

> `kv(const char* zstr, ValueOperation val)`

Generate a key-value pair element, with the key given as the null-terminated C
string at `zstr`, and the element value given by `val`. (See:
[*ValueOperation*](#valueoperation))


#### `if()`

> `if(bool c, then(DocOperation... thens))`
> `if(bool c, then(DocOperation... thens), else(DocOperation... elses))`

Conditionally insert elements into the document. If `c` evaluates to `true`,
evaluates `thens`, otherwise `elses` (if given).

`c` is a regular C expression (not a DSL *Predicate*, which is only used for
visiting/parsing).

Note that the `then()` and `else()` keywords must wrap the sub-operations:

```c
if(some_condition, then(...), else(...))
```


#### `insert()`

> `insert(bson_t b, Predicate cond)`

For some other valid `bson_t` given as `b`, copy every element from `b` that
satisfies `cond` into the parent document.

The *Predicate* `cond` will be evaluated for every element in `b`. Refer:
[*Predicate*](#Predicate).

To copy *all* elements from `b`, simply use the bare `true` predicate.


#### `insertFromIter()`

> `insertFromIter(bson_iter_t iter, Predicate cond)`

Like `insert()`, but copies from the bson document/array referred to by `iter`.


#### `iterElement()`

> `iterElement(bson_iter_t iter)`

Copy the BSON document element referred to by the given iterator. The element
will have the same key+value as from the iterator.


#### `do()`

> `do(...)`

Execute arbitrary C code while building the document. The document that is being
built can be accessed and further modified by using the `bsonBuildContext.doc`
pointer.

Note: The given code can use `break` to "early exit" from the `do()`, but MUST
NOT use any other form of control flow that would exit the `do()` block (i.e.
`return`, `continue`, `goto`).


### *ValueOperation*

The DSL *ValueOperations* are used to specify the value of an element to be
appended to a document or array.


#### `null`

Generates a BSON null value.


#### `boolean(bool b)`

Generate a BSON boolean value from the given C boolean expression.


#### `int32(int32_t v)`
#### `int64(int64_t v)`

Generate an integral value from the given C integer expression.


#### `cstr(const char* zstr)`

Generate a UTF-8 value from the given null-terminated character array beginning
at `zstr`.


#### `utf8_w_len(const char* str, int len)`

Generate a UTF-8 value from the character array beginning at `str`, with length
`len`.


#### `iterValue(bson_iter_t iter)`

Copy the BSON value that is referenced by the given `iter`.


#### `bson(bson_t)`

Copy the given `bson_t` document as a subdocument for the element.


#### `bsonArray(bson_t)`

Copy the given `bson_t` document as an array element. (All keys in the array
document are expected to be integer index keys, but this is not enforced).


#### `doc(DocOperation... ops)`

Create a sub-document using DSL *DocOperation* commands `ops`.


#### `array(ArrayOperation... ops)`

Create a sub-document of array type using the DSL *ArrayOperation* commands
`ops`.


#### `value(bson_value_t)`

Insert the value denoted by the given `bson_value_t`.


#### `if(bool cond, then(ValueOperation), else(ValueOperation))`

"pick" one of two *ValueOperations* to use for the value, based on `cond`. Note
that both of `then()` and `else()` are required.


### *ArrayOperation*

*ArrayOperations* are used to build elements of arrays. **Most**
*ValueOperations* are also valid *ArrayOperations*, with the following
additions:

#### `insert(bson_t other, Predicate cond)`

Like the *DocOperation* `insert()`, but elements from `other` are given integer
index keys to match the enclosing array.


#### `if(bool cond, then(ArrayOperation... thens))
#### `if(bool cond, then(ArrayOperation... thens), else(ArrayOperation... elses))

Conditionally execute more *ArrayOperations* based on `cond`.


# Reading BSON Data

## `bsonParse`

> `bsonParse(bson_t b, ParseOperation... ops)`

Perform the given *ParseOperations* `ops` on `b`.


## `bsonVisitEach`

> `bsonVisitEach(bson_t b, VisitOperation... ops)`

Perform the given *VisitOperations* `ops` *on every element* of `b`.

## `bsonAs`

> `bsonAs(TypeName t)`

Dereference the currently visited/parsed element as a value of the given type
`t`. May only be evaluated within a visit/parsing context.


## "Parse" versus "Visit"

`bsonParse` and `bsonVisitEach` are both commands for inspecting and
destructuring BSON data, but work differently.

`bsonParse` is given a list of *ParseOperations*, and then evaluates each of
those operations in the given order exactly once for the whole document.
`bsonParse` is used to inspect a document as a whole.

`bsonVisitEach` is given a list of *VisitOperations*, and *for each element*
will evaluate *all* of those operations. `bsonVisitEach` is used to inspect the
individual elements of a document. Each element is visited in the order that it
appears, and each operation is evaluated in the order they were given.


## The `bsonVisitIter`

There is a global (thread-local) non-modifiable lvalue name `bsonVisitIter`,
which evaluates to a `const bson_iter_t` that refers to whatever (sub)document
element is currently being visited or parsed.

This is useful in contexts where one wishes to execute arbitrary code within a
parse/visit, and needs to refer to the element that is being visited.

**Note** that evaluating this name outside of a parse/visit context is
**undefined behavior**.


## Syntax

### *ParseOperation*

Parse operations are used to find specific elements within documents, and
perform additional control-flow operations.


#### `find`
#### `require`

> `find(Predicate p, VisitOperation... ops)`
> `require(Predicate p, VisitOperation... ops)`

Locate the first element that matches the *Predicate* `p`, and then evaluate
each *VisitOperation* in `ops` for that element.

If the `require` command fails to find a matching element, it will generate an
`error()` command that mentions the failed predicate `p`.


#### `else`

> `else(ParseOperation... ops)`

If the previous `find()` command *did not* find an element, evaluate `ops`,
otherwise does nothing. **NOTE** that this is unrelated to the "`if`"
*ParseOperation*.


#### `visitOthers`

> `visitOthers(VisitOperation... ops)`

For each element in the document being parsed that *has not* been visited by any
previous `find` or `require` operation, evaluate `ops`


#### `error`

> `error(const char* S)`

Assign the given character pointer `S` to `bsonParseError`. **Note** that this
string is thread-local and is not freed or allocated. It is recommended to only
use string literals for `S`.


#### `errorf`

> `errorf(char*& E, const char* fmt, ...)`

If `E` is non-`NULL`, it will be freed, then: Generate an error string using
printf-formatting. The allocated pointer will be written to `E`.
`bsonParseError` will also be set to point to the same address as `E`. **NOTE:**
It is the callers's responsibility to eventually free `E`.


#### `halt`

The `halt` keyword stops the current `bsonVisit()`/`bsonParse()` in which it
appears.


#### `if`

> `if(bool cond, then(ParseOperation... thens))`
> `if(bool cond, then(ParseOperation... thens), else(ParseOperation... elses))`

If `cond` is true, evaluate `thens`, otherwise evaluate `elses` (if provided).


#### `append`

> `append(bson_t b, DocOperation... ops)`

Evaluate as-if a `bsonBuildAppend(b, ops...)` appears at this location. The
value of `bsonVisitIter` is unspecified.


#### `do`

> `do(...)`

Evaluate the given arbitrary code. The value of `bsonVisitIter` is unspecified.


### *VisitOperation*

A *VisitOperation* applies a specific action to individual elements within a
BSON document. During a visit operation, `bsonVisitIter` refers to the current
element that is being visited or parsed.

For example, during a parse `find()` operation, `bsonVisitIter` refers to the
element that was found matching the predicate. During a `bsonVisitEach()`, each
element is iterated over, and `bsonVisitIter` will refer to every element in
turn.


#### `do(...)`

Evaluate the given C code. The `bsonVisitIter` can be used to reference the
element being visited/parsed.


#### `storeBool(bool& b)`

If `bsonVisitIter` refers to a boolean value, assign that value to `b` (which
must be a modifiable lvalue of type `bool`).


#### `storeStrRef(const char*& s)`

If `bsonVisitIter` refers to a UTF-8 string, a pointer to that string element
will be assigned to `s`. **NOTE** that this writes a pointer to the string
within the BSON data, so `s` is only valid as long as the `bson_t` document is
valid and unmodified.


#### `storeStrDup(const char*& s)`

If `bsonVisitIter` refers to a UTF-8 string, that string will be copied and a
pointer to the allocated region will be assigned to `s`, which must later be
freed.


#### `storeDocRef(bson_t& doc)`

Update `doc` to be a view of the subdocument at `bsonVisitIter`. This view will
remain valid for as long as the root document being parsed is valid. The `doc`
is a view, so it will not need to be destroyed, and cannot be modified.

`storeDocDup(bson_t&)` and `storeDocDupPtr(bson_t*&)` will duplicate the
document element into the given `bson_t` (or pointer thereof). The duplicated
document will need to be destroyed, and is valid to modify.


#### `append(bson_t b, DocOperation... ops)`

Evaluate a `bsonBuildAppend(b, ops...)`, with `bsonVisitIter` referring to the
element being visited.


#### `appendTo(bson_t b)`

Append the current element to the BSON document `b`. The element will be copied
with the same key and value.


#### `visitEach(VisitOperation... ops)`

If `bsonVisitIter` refers to a document or array element `A`, apply
`bsonVisitEach(A, ops...)`.


#### `parse(ParseOperation... ops)`

If `bsonVisitIter` refers to a document or array element `D`, apply
`bsonParse(D, ops...)`.


#### `halt`

Completely stop the current `bsonParse`/`bsonVisit` command.


#### `break`

Stop the inner-most `bsonVisitEach`/`visitEach` command. Only valid when
directly within a `bsonVisitEach` or `visitEach` context.


#### `continue`

If directly within a `bsonVisitEach`/`visitEach` evaluation, stop visiting the
current element and advance to the next element.


#### `error(const char* S)`

Assign the given character pointer `S` to `bsonParseError`. **Note** that this
string is thread-local and is not freed or allocated. It is recommended to only
use string literals for `S`.


#### `errorf(char*& E, const char* fmt, ...)`

If `E` is not `NULL`, `E` will be freed. Then: Generate an error string using
printf-style formatting. The allocated string will be written to `E`.
`bsonParseError` will be updated to point to the generate string. **NOTE:** It
is the caller's responsibility to eventually free `E`.


#### `dupPath(char* P)`

If `P` is non-`NULL`, `P` is freed. Then: Create a JSONPath-style string that
refers to the document element currently being visited. The path will be rooted
at the document given to the top-most `bsonParse`/`bsonVisit` invocation in the
calling thread. **NOTE:** It is the caller's responsibility to eventually free
`P`.


#### `if()`

> `if(Predicate cond, then(VisitOperation... thens))`
> `if(Predicate cond, then(VisitOperation... thens), else(VisitOperation... elses))`

If the *Predicate* `cond` matches the current element, apply the `thens`
operations, otherwise the `elses` (if given).

**NOTE** that this `if()` command expects a DSL *Predicate* as its condition,
and *not* a regular C expression like the other `if()` DSL commands.


#### `nop`

Does nothing.


#### `case()`

> ```
> case(when(Predicate w,
>           VisitOperations... ops)
>      ...,
>      [else(VisitOperations... ops)])
> ```

Perform case selection based on a set of predicates.

`case` accepts zero or more `when(Predicate w, VisitOperation... ops)` clauses.
Each `when` is considered in the listed order. If the *Predicate* `w` of a
`when` matches the `bsonVisitIter` element, then the `when`'s *VisitOperations*
`ops` are evaluated on the element.

After any `when` clause matches an element, no subsequent `when` clauses are
considered.

An `else(ops...)` clause is equivalent to `when(true, ops...)`.

If no clause matches the element, nothing happens.


### *Predicate*

DSL *Predicates* appears in several locations. A *Predicate* evaluates some
condition on the current `bsonVisitIter` (which is updated automatically by
other DSL commands). A predicate can be evaluated at any point in C code by
using the `bsonPredicate()` expression (this is only valid within a `visitEach`
or `parse` context, since it references the `bsonVisitIter` name).

The following predicates are defined:


#### `key(const char* s...)`

Specify one or more C strings for `s`. This predicate will match if *any of* `s`
is equivalent to the element's key string.

The `iKey(s...)` predicate is the same as `key`, but performs a case-insensitive
match.


#### `type(TypeName t)`

Matches if the element's current type matches the `TypeName` `t`.

The `TypeName`s are: `double`, `utf8`, `doc`, `array`, `binary`, `undefined`,
`oid`, `boolean`, `date_time`, `null`, `regex`, `dbpointer`, `code`, `codewscope`,
`int32`, `timestamp`, `int64`, and `decimal128`.


#### `keyWithType(const char* k, TypeName t)`

Matches an element with key `k` and type `t`.

`iKeyWithType` performs a case-insensitive string match for the key.


#### `isNumeric`

The `isNumeric` predicate matches if the element's value is a numeric type
(`double`, `int32`, or `int64`. `decimal128` is *not* included).


#### `lastElement`

Matches only the final element in a document.


#### `true`

Always matches.


#### `false`

Never matches.


#### `isTrue`

Matches is the element is "true" according to `bson_iter_as_bool()`: A `true`
boolean, a non-zero int/double value, or other non-null value.


#### `isFalse`

Matches any element that is not `isTrue`.


#### `empty`

Matches an element which is an empty BSON subdocument or array.


#### `strEqual(const char* s)`

Matches if the element value is a UTF-8 string and is equivalent to the string
`s`.

`iStrEqual(s)` performs the same match, but performs a case-insensitive
comparison.


#### `eq(TypeName t, any v)`

Matches if the element has type `t` and `bsonAs(t) == v`.


#### `not(Predicate... ps)`

Matches if *none of* the predicates `ps` match.


#### `allOf(Predicate... ps)`

Matches if *all of* `ps` match the element (Logical conjunction). If `ps` is
empty, always matches.


#### `anyOf(Predicate... ps)`

Matches if *any of* `ps` match the element (logical disjunction). If `ps` is
empty, never matches.


#### `eval(...)`

Evaluate the given C expression, which must return a boolean-testable value. The
code can refer to the element under consideration via `bsonVisitIter`. The code
can test other predicates by using `bsonPredicate(P)`, which evaluates to a
boolean value.


# Error Handling

The DSL uses two global (thread-local) names to perform error handling:
`bsonBuildError` and `bsonParseError`.

Upon entry, BSON-constructing commands will set `bsonBuildError` to `NULL`.
BSON-reading commands will set `bsonParseError` to `NULL`.

If `bsonBuildError` becomes non-`NULL` during DSL evaluation, the enclosing
`bsonBuild-` command will stop evaluating and immediately return to the caller.
`bsonParse` and `bsonVisitEach` behave similarly with respect to the
`bsonParseError` name.

After a DSL command returns, `bsonParseError`/`bsonBuildError` should be checked
for `NULL`. If non-`NULL`, the relevant command encountered an error while
executing. This can happen if the BSON iteration encountered corruption, if any
build operation failed, if an `error()` or `errorf()` DSL command was executed,
or if any inner C code assigned a non-`NULL` value to the respective error
string:

```c
// These functions add items to bsonBuildContext.doc:
void add_stuff();
void add_data();
void add_more();
```
```c
bsonBuildAppend(mydoc,
                do(add_stuff()),
                do({
                  if (!some_computation()) {
                    bsonBuildError = "some_computation() failed!";
                  }
                }),
                kv("subarray", bsonArray(some_array)),
                do(add_data()),
                do(add_more()));
if (bsonBuildError) {
  fprintf(stderr, "Error building BSON: %s\n", bsonBuildError);
}
```

In the above, any of the `do` blocks can indicate an error by assigning
`bsonBuildError` to a non-`NULL` string. The `bsonArray` *ValueOperation* may
also generate an error while appending the `subarray` element (e.g. if
`some_array` is corrupt). As soon as one subcommand indicates an error, the
`bsonBuildAppend()` will not evaluate any further subcommands. The
`bsonBuildError` value will be retained after it returns, for inspection by the
caller.

The `error()` (or `errorf()`) visit/parse command can be used to do terse
document validation:

```c++
struct user_info { char* name; int age; };
user_info get_user(const bson_t* data) {
  user_info ret = {0};
  bsonParse(*data,
            // Get the name
            find(key("name"),
                 if(not(type(utf8)),
                    then(error("'name' must be a string"))),
                 if(strEqual(""),
                    then(error("'name' must not be empty"))),
                 storeStrDup(ret.name)),
            else(error("The 'name' property is missing")),
            // Get the age
            find(key("age"),
                 case(when(not(isNumeric),
                           error("'age' must be a number")),
                      when(eval(bsonAs(int32) < 0),
                           error("'age' cannot be negative")),
                      else(do(ret.age = bsonAs(int32))))),
            else(error("The 'age' property is missing")));
  if (bsonParseError) {
    bson_free(ret.name);
    fprintf(stderr, "Error while reading user_info from bson: %s\n", bsonParseError);
    return {0};
  }
  return ret;
}
```


# Other Important Details

## Deferred Evaluation

In a regular C program a function call `foo(x, y, z)` will evaluate `x`, `y`,
and `z` *before* calling `foo`. In the DSL, this is not the case: Subcommands
that accept C code and expressions will evaluate the C code only if/when the
subcommand is executed.

This allows one to rely on previous-executed DSL subcommands in subsequent
commands. e.g. one can store a variable extracted from a document, and then
later perform additional conditional parsing based on the value that was
extracted:

```c
int32_t expect = 0;
bsonParse(
  some_data,
  require(key("expect"), do(v = bsonAs(int32))),
  // At this point "expect" has been extracted from 'some_data'
  require(key("value"),
          case(when(bsonAs(int32) < expect), error("'value' is less than 'expect'"),
               when(bsonAs(int32) > expect), error("'value' is greater than 'expect'"))));
```


## Building while Parsing

The `append()` visit operation allows one to update one BSON document while
parsing another:

```c
const bson_t* input = ...;
bson_t* output = ...;
bsonParse(
  input,
  // Find a "src' subdocument
  require(keyWithType("src", doc),
          // Append what we found to the "got" key in 'output':
          // (bsonVisitIter still points to "src" in 'input')
          append(*output, kv("got", iterValue(bsonVisitIter)))),
  // Add "readonly", but only if we find it.
  find(key("readonly"),
       // bsonPredicate() will evaluate a predicate on bsonVisitIter
       // (which here points to the "readonly" property in "input")
       append(*output, kv("readonly", boolean(bsonPredicate(truthy))))));
```


# Understanding Compiler Errors

If you misspell the name of any DSL subcommand, or use one in an incorrect
location, you will receive a string of errors from your compiler. (If you are
using GCC, this will include a length expansion backtrace for `_bsonDSL_eval`
and `_bsonDSL_mapMacro`, which you can mostly scroll over and ignore).

However, all errors should be ignored *except* for the first one, which will
often indicate the invocation of an unknown/implicitly-declared function. The
name of the "missing function" will be the concatenation of a DSL-specific
command namespace and the erroneous command name that was given. For example:

```c
bsonBuildAppend(
  cmd,
  kv("count",
     utf8_w_len(collection->collection, collection->collectionlen)),
  kv("query", if(query, then (bson (*query)), else(doc()))),
  if(limit, then(kv("limit", o64(limit)))),  // <-- misspelled "int64"
  if(skip, then(kv("skip", int64(skip)))));
```

results in the following error from GCC:

```
error: implicit declaration of function ‘_bsonValueOperation_o64’;
  did you mean ‘_bsonValueOperation_int64’? [-Werror=implicit-function-declaration]
```

This error message includes a helpful hint as to the spelling error and likely
correction.


# Debugging

Unfortunately, debugging through macro definitions is still a tricky subject.

`bson-dsl.h` includes a function `_bson_dsl_debug()`, which is invoked for every
DSL subcommand, including a string expressing the arguments to the command. If
debug is "enabled" for the DSL, `_bson_dsl_debug` will write the debug output to
`stderr`. If it is "disabled", `_bson_dsl_debug` is a no-op.

A breakpoint can be set within `_bson_dsl_debug` to "step through" each DSL
operation.

Each DSL command looks for a name `BSON_DSL_DEBUG`. If this value is truthy,
debug output is enabled. The default of `BSON_DSL_DEBUG` is a global constant
set to zero. A local `BSON_DSL_DEBUG` variable can be declared within the scope
of a DSL command to toggle debugging for that scope only.


# How?

The DSL makes extreme usage of the C preprocessor to expand to fairly simple C
code inline.


## The "MAP" Macro

Suppose we have a macro "MAP", with the following psuedo-code:

```c
#define MAP(F, Args... args) \
  #foreach arg in args          \
    F(arg)                   \
  #endforeach
```

That is, MAP accepts a name `F` and zero or more arguments as `args`. For
each argument `a` in `args`, expand one `F(x)`.

The actual definition of `MAP` is non-trivial. The definition provded in
`bson-dsl.h` can be found near the bottom of the file, and goes by the name
`_bsonDSL_mapMacro`. It is not necessary to understand how `MAP` works to
understand the DSL.


## Token Pasting

The token-paste operator `##` is a preprocessor operator that concatenates two
identifiers for form a single new identifier.

If the right-hand operand of `##` contains additional non-identifier characters,
this does not matter. It only matters that the character touching the `##`
operator is a valid alphanumeric.

For example, pasting `foo` and `bar(baz)`, results in `foobar(baz)`.


## Pasting with "MAP"

Suppose the following macro is defined:

```c
#define execEach(...) \
  MAP(_doExecEach, __VA_ARGS__)

#define _doExecEach(Item) \
  _doExecOperation_ ## Item
```

and then we use it:

```c
execEach(foo(1), bar(2), baz(3))
```

For the first expansion:

```c
_doExecEach(foo(1))
_doExecEach(bar(2))
_doExecEach(baz(3))
```

Then:

```c
_doExecOperation_foo(1)
_doExecOperation_bar(2)
_doExecOperation_baz(3)
```

The resulting names may be regular functions, or may be additional macros that
will be further expanded. This is the basis of this BSON DSL.


## One More Thing: `EVAL`

There's one more thing to note. Imagine this:

```c
#define AGAIN(X) X AGAIN(X)
```

A naive intuition is that `AGAIN(f)` will expand to an unending sequence of
`f f f f f f f f f ...` since `AGAIN` "calls itself." However, the C
preprocessor is written such that this will not occur. During the expansion of
any macro `M`, any appearance of `M` within the result will be ignored.

What we need is a way to force the preprocessor to "do it again." Besides simply
appearing in the source text or during a re-scan, there is another context in
which preprocessor macros will be expanded: When preparing the arguments to
other function-like macros.

This is where `EVAL` comes in:

```c
#define EVAL(X) _doEval2(_doEval2(X))

#define _doEval2(X) _doEval4(_doEval4(X))
#define _doEval4(X) _doEval8(_doEval8(X))
#define _doEval8(X) _doEval16(_doEval16(X))
#define _doEval16(X) X
```

In this example, `EVAL(X)` will macro-expand `X` sixteen times: Twice more for
each layer. This can be extended to arbitrary depth and recurring expansions.

For `EVAL(AGAIN(f))`, we will end up with a string of `f f f f f f f f ...` followed
by a single `AGAIN(f)`.

There are additional tricks required to make `EVAL`, `MAP`, and token pasting
play nice, but those details are not necessary here.

