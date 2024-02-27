# Query Shape

A query shape is a transformed version of a command with literal values replaced by a "canonical"
BSON Type placeholder. Hence, different instances of a command would be considered to have the same
query shape if they are identical once their literal values are abstracted.

For example, these two queries would have the same shape:

```js
db.example.findOne({x: 24});
db.example.findOne({x: 53});
```

While these queries would each have a distinct shape:

```js
db.example.findOne({x: 53, y: 1});
db.example.findOne({x: 53});
db.example.findOne({x: "string"});
```

While different literal _values_ result in the same shape (matching `x` for 23 vs 53), different
BSON _types_ of the literal are considered distinct shapes (matching `x` for 53 vs "string").

The concept of a query shape exists not just for the find command, but for many of the CRUD commands
and aggregate. It also includes most (but not all) components of these commands, not just the query
predicate (MatchExpresssion). In these ways, "query" is meant more generally. While some components
included in the query shape are shared across the different types of commands (e.g., the "hint"
field), some are unique. For example, a find command would include a `filter` while an aggregate
command would have a `pipeline`.

You can see which components are considered part of the query shape or not for each specific shape
type in their respective "shape component" classes, whose purpose is to determine which components
are relevant and should be included for determining the shape for specific type of command. The
structure is as follows:

-   [`CmdSpecificShapeComponents`](query_shape.h#L65)
    -   [`LetShapeComponent`](cmd_with_let_shape.h#L48)
        -   [`AggCmdShapeComponents`](agg_cmd_shape.h#L82)
        -   [`FindCmdShapeComponents`](find_cmd_shape.h#L48)

See more information for the different shapes in their respective classes, structured as follows:

-   [`Shape`](query_shape.h)
    -   [`CmdWithLetShape`](cmd_with_let_shape.h)
        -   [`AggCmdShape`](agg_cmd_shape.h)
        -   [`FindCmdShape`](find_cmd_shape.h)

## Serialization Options

`SerializationOptions` describes the way we serialize literal values.

There are 3 different serialization options:

-   `kUnchanged`: literals are serialized unmodified
    -   `{x: 5, y: "hello"}` -> `{x: 5, y: "hello"}`
-   `kToDebugTypeString`: human readable format, type string of the literal is serialized
    -   `{x: 5, y: "hello"}` -> `{x: "?number", y: "?string"}`
-   `kToRepresentativeParseableValue`: literal serialized to one canonical value for given type, which
    must be parseable - `{x: 5, y: "hello"}` -> `{x: 1, y: "?"}` - An example of a query which is serialized differently due to the parseable requirement is `{x:
{$regex: "^p.*"}}`. If we serialized the pattern as if it were a normal string we would end up
    with `{x: {$regex: "?"}}` however `"?"` is not a valid regex pattern, so this would fail
    parsing. Instead we will serialize it this way to maintain parseability, `{x: {$regex:
"\\?"}}`, since `"\\?"` is valid regex.

See [serialization_options.h](serialization_options.h) for more details.

When we compute the [query shape hash](query_shape.cpp#L99-107), we use the
`kToRepresentativeParseableValue`, since all literals of the same type will become the same value.
This allows us to group together queries that have the same structure but different literal values
into the same shape, since they will result in the same hash. The term we use to refer to this is
"shapify", as we simplify the queries into their query shape.

When shapifying, we try to get as close as possible to the original user input, but there are some
stages like `$jsonSchema` and `$setWindowFields` that output "internal" stages that are already
transformed from user input.
