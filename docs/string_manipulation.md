# String Manipulation

For string manipulation, use the util/mongoutils/str.h library.

## `str.h`

`util/mongoutils/str.h` provides string helper functions for each manipulation.

`str::stream()` is quite useful for assembling strings inline:

```
uassert(12345, str::stream() << "bad ns:" << ns, isOk);
```

## `StringData`

```
/** A StringData object wraps a 'const std::string&' or a 'const char*' without
 * copying its contents. The most common usage is as a function argument that
 * takes any of the two forms of strings above. Fundamentally, this class tries
 * to work around the fact that string literals in C++ are char[N]'s.
 *
 * Important: the object StringData wraps must remain alive while the StringData
 * is.
*/
class StringData {
```

See also [`bson/string_data.h`][1].

[1]: ../src/mongo/base/string_data.h
