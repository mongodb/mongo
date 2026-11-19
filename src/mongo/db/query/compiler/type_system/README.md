# Type system

- Types model runtime execution. Soundness comes first; precision is secondary. Inference may return
  a broader type, but must not exclude a possible runtime value.
  - Expressions: find the expression class's evaluate method, then the evaluate overload taking that
    class by const reference.
  - Matcher: read the relevant MatchExpression class, its ElementPath's LeafArrayBehavior and
    NonLeafArrayBehavior, and BSONElementIterator's traversal implementation.

## Type Model

- TypeSet stores BSON-type alternatives and per-type exactness (Extent). Type adds an object shape
  to TypeSet; both carry exactness, but TypeSet alone loses field constraints. Always use Type, even
  when TypeSet would suffice today, to preserve support for shapes and future refinements such as
  literal types.

- Extent records whether a type covers all values of a BSON type or a subset. It matters for
  predicate constraints, especially negation: complementing a subset must leave that BSON type
  possible. For value types such as {$set: {a: 1}}, number and number(S) give the same BSON-type and
  arrayness facts; neither records the literal value 1.

- Singleton BSON types have only one possible value: minKey, missing, undefined, null, and maxKey.
  Their extent is always kAll. A type containing only one BSON type is not necessarily a singleton:
  int covers many values.

- Unnamed fields have type any in open objects and missing in closed objects. We model each BSONType
  enum value separately, including eoo (missing).

- Arrays are opaque. Subsystems often traverse arrays and test whether a matching element exists:
  `{x: {$type: 'number'}}` accepts `[1, 'str']`. A matching element does not constrain every
  element. Incompatible scalar predicates may both match a mixed array, so their conjunction need
  not imply never.

- Use std::move on Type operands that are no longer needed when passing them to intersectType,
  unionType, and other by-value operations. Copying shares field storage; moving avoids adding an
  owner and lets operations reuse uniquely owned storage in place instead of cloning through
  copy-on-write.

## Inference

- Use the path traversal and narrowing implementation for the consuming subsystem. Matcher,
  aggregation expressions, and catch-all getPathType have different semantics; do not substitute one
  for another.

- The free function narrowType supports MatchExpression: refine the input document type given a true
  or false match result. The free functions narrowType and getOutputType support Expression: the
  former refines the input under a known boolean-coerced outcome (e.g. $match/$expr); the latter
  infers the expression's result type (e.g. a computed field in $set). Unsupported narrowing leaves
  the input unchanged; unsupported output typing returns any.

- Both narrowType overloads and getOutputType take a document type: object or never, not any.
  Narrowing returns object or never; output typing returns the expression's value type.

- Negate the predicate, not its inferred document type. Pass assumeTrue=false into narrowing so it
  accounts for traversal when inferring a false result. For example, {x: {$not: {$type: 'number'}}}
  excludes numbers but still permits arrays with no numeric elements; it does not imply ~array.

- Comparison inference must use BSON canonical comparison classes, not just the constant's BSON
  type. Numbers compare across numeric types; string/symbol and missing/undefined also share
  comparison classes.

## Testing

- Where possible, assert toDebugString() against a literal expected type. It captures the full type
  information. Keep storage-specific assertions when storage behavior is what the test verifies.

- Test inference against runtime execution using the corpus helpers. For narrowing, check that the
  inferred type admits each document under its runtime match or boolean-coerced outcome, testing
  both true and false. For output typing, check that the inferred type admits each successfully
  evaluated value.
  - For dotted paths, vary prefixes and leaves: objects, scalars, missing, null, and arrays. Include
    empty, mixed, and nested arrays, plus numeric path components; do not test only a scalar leaf
    below object prefixes.
  - Include constrained input types as well as object, so tests catch lost prior field constraints.

## Type Debug Syntax

Use toDebugString() syntax in all communication about types, including explanations, reviews,
comments, logs, and test expectations. It is internal notation, not MQL.

- any includes every value, including missing; never includes no values.
- int|string is a union; number abbreviates double|int|long|decimal.
- string(S) means an unspecified subset; string means all strings.
- ~array excludes arrays; ~(null|array) excludes both. Subsets remain possible under complement, so
  ~string(S) still permits strings.
- {x: number} is closed (unnamed fields are missing); {x: number, ...} is open (unnamed fields are
  any). Shapes nest and refine only objects.
- object is any object; {} is the closed empty object.

Use actual rendered output for assertions. Search for toDebugString implementations and literal test
expectations for details and examples.
