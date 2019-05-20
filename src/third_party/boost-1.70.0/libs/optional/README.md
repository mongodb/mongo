optional
========

A library for representing optional (nullable) objects in C++.

```cpp
optional<int> readInt(); // this function may return either an int or a not-an-int

if (optional<int> oi = readInt()) // did I get a real int
  cout << "my int is: " << *oi;   // use my int
else
  cout << "I have no int";
```

For more information refer to the documentation provided with this library.
