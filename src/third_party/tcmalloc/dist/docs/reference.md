# TCMalloc Basic Reference

TCMalloc provides implementations for C and C++ library memory management
routines (`malloc()`, etc.) provided within the C and C++ standard libraries.

Currently, TCMalloc requires code that conforms to the C11 C standard library
and the C++11, C++14, or C++17 C++ standard library.

NOTE: although the C API in this document is specific to the C language, the
entire TCMalloc API itself is designed to be callable directly within C++ code
(and we expect most usage to be from C++). The documentation in this section
assumes C constructs (e.g. `size_t`) though invocations using equivalent C++
constructs of aliased types (e.g. `std::size_t`) are instrinsically supported.

## C++ API

We implement the variants of `operator new` and `operator delete` from the
C++11, C++14, C++17 standards exposed within the `<new>` header file. This
includes:

*   The basic `::operator new()`, `::operator delete()`, and array variant
    functions.
*   C++14's sized `::operator delete()`
*   C++17's overaligned `::operator new()` and `::operator delete()` functions.
    As required by the C++ standard, memory allocated using an aligned `operator
    new` function must be deallocated with an aligned `operator delete`.

### `::operator new` / `::operator new[]`

```
void* operator new(std::size_t count);
void* operator new(std::size_t count, const std::nothrow_t& tag) noexcept;
void* operator new(std::size_t count, std::align_val_t al);  // C++17
void* operator new(std::size_t count,
                   std::align_val_t al, const std::nothrow_t&) noexcept;  // C++17

void* operator new[](std::size_t count);
void* operator new[](std::size_t count, const std::nothrow_t& tag) noexcept;
void* operator new[](std::size_t count, std::align_val_t al);  // C++17
void* operator new[](std::size_t count,
                     std::align_val_t al, const std::nothrow_t&) noexcept;  // C++17
```

`operator new`/`operator new[]` allocates `count` bytes. They may be invoked
directly but are more commonly invoked as part of a *new*-expression.

When `__STDCPP_DEFAULT_NEW_ALIGNMENT__` is not specified (or is larger than 8
bytes), we use standard 16 byte alignments for `::operator new` without a
`std::align_val_t` argument. However, for allocations under 16 bytes, we may
return an object with a lower alignment, as no object with a larger alignment
requirement can be allocated in the space. When compiled with
`__STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 8`, we use a set of sizes aligned to 8
bytes for raw storage allocated with `::operator new`.

NOTE: On many platforms, the value of `__STDCPP_DEFAULT_NEW_ALIGNMENT__` can be
configured by the `-fnew-alignment=...` flag.

The `std::align_val_t` variants provide storage suitably aligned to the
requested alignment.

If the allocation is unsuccessful, a failure terminates the program.

NOTE: unlike in the C++ standard, we do not throw an exception in case of
allocation failure, or invoke `std::get_new_handler()` repeatedly in an attempt
to successfully allocate, but instead crash directly. Such behavior can be used
as a performance optimization for move constructors not currently marked
`noexcept`; such move operations can be allowed to fail directly due to
allocation failures. Within Abseil code, these direct allocation failures are
enabled with the Abseil build-time configuration macro
[`ABSL_ALLOCATOR_NOTHROW`](https://abseil.io/docs/cpp/guides/base#abseil-exception-policy).

If the `std::no_throw_t` variant is utilized, upon failure, `::operator new`
will return `nullptr` instead.

The `tcmalloc::hot_cold_t` variant accepts a `hot_cold` hint for how frequently
an allocation would be accessed. It takes an 8-bit unsigned integer value, with
`0` indicating that the allocation is rarely used and `255` indicating that the
allocation is accessed very frequently. TCMalloc may use these hints for better
data placement and locality.

### `::operator delete` / `::operator delete[]`

```
void operator delete(void* ptr) noexcept;
void operator delete(void* ptr, std::size_t sz) noexcept;
void operator delete(void* ptr, std::align_val_t al) noexcept;
void operator delete(void* ptr, std::size_t sz,
                     std::align_val_t all) noexcept;

void operator delete[](void* ptr) noexcept;
void operator delete[](void* ptr, std::size_t sz) noexcept;       // C++14
void operator delete[](void* ptr, std::align_val_t al) noexcept;  // C++17
void operator delete[](void* ptr, std::size_t sz,
                       std::align_val_t al) noexcept;             // C++17
```

`::operator delete`/`::operator delete[]` deallocate memory previously allocated
by a corresponding `::operator new`/`::operator new[]` call respectively. It is
commonly invoked as part of a *delete*-expression.

Sized delete is used as a critical performance optimization, eliminating the
need to perform a costly pointer-to-size lookup.

### Extensions

We also expose a prototype of
[P0901](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p0901r5.html) in
https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h with
`tcmalloc_size_returning_operator_new()`. This returns both memory and the size
of the allocation in bytes. It can be freed with `::operator delete`.

## C API

The C standard library specifies the API for dynamic memory management within
the `<stdlib.h>` header file. Implementations require C11 or greater.

TCMalloc provides implementation for the following C API functions:

*   `malloc()`
*   `calloc()`
*   `realloc()`
*   `free()`
*   `aligned_alloc()`

For `malloc`, `calloc`, and `realloc`, we obey the behavior of C90 DR075 and
[DR445](http://www.open-std.org/jtc1/sc22/wg14/www/docs/summary.htm#dr_445)
which states:

> The alignment requirement still applies even if the size is too small for any
> object requiring the given alignment.

In other words, `malloc(1)` returns `alignof(std::max_align_t)`-aligned pointer.
Based on the progress of
[N2293](http://www.open-std.org/jtc1/sc22/wg14/www/docs/n2293.htm), we may relax
this alignment in the future.

Additionally, TCMalloc provides an implementation for the following POSIX
standard library function, available within glibc:

*   `posix_memalign()`

TCMalloc also provides implementations for the following obsolete functions
typically provided within libc implementations:

*   `cfree()`
*   `memalign()`
*   `valloc()`
*   `pvalloc()`

Documentation is not provided for these obsolete functions. The implementations
are provided only for compatibility purposes.

### `malloc()`

```
void* malloc(size_t size);
```

`malloc` allocates `size` bytes of memory and returns a `void *` pointer to the
start of that memory.

`malloc(0)` returns a non-NULL zero-sized pointer. (Attempting to access memory
at this location is undefined.) If `malloc()` fails for some reason, it returns
NULL.

### `calloc()`

```
void* calloc(size_t num, size_t size);
```

`calloc()` allocates memory for an array of objects, zero-initializes all bytes
in allocated storage, and if allocation succeeds, returns a pointer to the first
byte in the allocated memory block.

`calloc(num, 0)` or `calloc(0, size)` returns a non-NULL zero-sized pointer.
(Attempting to access memory at this location is undefined.) If `calloc()` fails
for some reason, it returns NULL.

### `realloc()`

```
void* realloc(void *ptr, size_t new_size);
```

`realloc()` re-allocates memory for an existing region of memory by either
expanding or contracting the memory based on the passed `new_size` in bytes,
returning a `void*` pointer to the start of that memory (which may not change);
it does not perform any initialization of new areas of memory.

`realloc(OBJ*, 0)` returns a NULL pointer. If `realloc()` fails for some reason,
it also returns NULL.

### `aligned_alloc()`

```
void* aligned_alloc(size_t alignment, size_t size);
```

`aligned_alloc()` allocates `size` bytes of memory with alignment of size
`alignment` and returns a `void *` pointer to the start of that memory; it does
not perform any initialization.

The `size` parameter must be an integral multiple of `alignment` and `alignment`
must be a power of two. If either of these cases is not satisfied,
`aligned_alloc()` will fail and return a NULL pointer.

`aligned_alloc` with `size=0` returns a non-NULL zero-sized pointer. (Attempting
to access memory at this location is undefined.)

### `posix_memalign()`

```
int posix_memalign(void **memptr, size_t alignment, size_t size);
```

`posix_memalign()`, like `aligned_alloc()` allocates `size` bytes of memory with
alignment of size `alignment` to the start of memory pointed to by `**memptr`;
it does not perform any initialization. This pointer can be cast to the desired
type of data pointer in order to be dereferenceable. If the alignment allocation
succeeds, `posix_memalign()` returns `0`; otherwise it returns an error value.

`posix_memalign` is similar to `aligned_alloc()` but `alignment` be a power of
two multiple of `sizeof(void *)`. If the constraints are not satisfied,
`posix_memalign()` will fail.

`posix_memalign` with `size=0` returns a non-NULL zero-sized pointer.
(Attempting to access memory at this location is undefined.)

### `free()`

```
void free(void* ptr);
```

`free()` deallocates memory previously allocated by `malloc()`, `calloc()`,
`aligned_alloc()`, `posix_memalign()`, or `realloc()`. If `free()` is passed a
null pointer, the function does nothing.

### Extensions

These are contained in
https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h.

*   `nallocx(size_t size, int flags)` - Returns the number of bytes that would
    be allocated by `malloc(size)`, subject to the alignment specified in
    `flags`.
*   `sdallocx(void* ptr, size_t size, int flags)` - Deallocates memory allocated
    by `malloc` or `memalign`. It takes a size parameter to pass the original
    allocation size, improving deallocation performance.
