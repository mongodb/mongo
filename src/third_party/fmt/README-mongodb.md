# Using the fmt Library in the MongoDB Server.

The fmt library is extensible, but to avoid user extensions interfering with
each other we want to be disciplined in how we use the extension points. For
now, please discuss extension use cases with the #server-programmability team.

Our `mongo::StringData` is usable wherever a string-like type can be used in
`fmt` library calls.  This was done by adding a special function to
`string_data.h`. More types will be added as we figure out which to support.

Basic Usage:

    s = fmt::format("{}, {}, {}, ... {}!", 1, '2', "three", kInf);

Note also that the functions in `fmt/printf.h` (printf, sprintf, fprintf) can
replace most calls to the `printf` family of functions, with a safer and often
faster alternative. These will throw if the argument types don't match the
format specifiers, where `printf` would have just suffered undefined behavior.

The `fmt::formatter<T>` template can be specialized to support custom types,
but this is advanced and not easy to get right. In any case, such hooks must be
defined in the same place as the `T` to avoid one-definition-rule violations.
Any type that can be put to a `std::ostream` can be written out with `fmt`
via `#include <fmt/ostream.h>`, so many potential `fmt::formatter`
specializations will be unnecessary.
