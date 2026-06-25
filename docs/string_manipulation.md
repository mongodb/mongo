# String Manipulation

For string manipulation, use the util/mongoutils/str.h library.

## `str.h`

`util/mongoutils/str.h` provides string helper functions for each manipulation.

`str::stream()` is quite useful for assembling strings inline:

```
uassert(12345, str::stream() << "bad ns:" << ns, isOk);
```
