/**
 * DO NOT DIRECTLY MODIFY THIS FILE:
 *
 * The code in this file is generated from scripts/import_libcbor.py
 * and any modifications should be in there.
 */

#ifndef LIBCBOR_CONFIGURATION_H
#define LIBCBOR_CONFIGURATION_H

#define CBOR_MAJOR_VERSION 0
#define CBOR_MINOR_VERSION 11
#define CBOR_PATCH_VERSION 0

#define CBOR_BUFFER_GROWTH 2
#define CBOR_MAX_STACK_SIZE 2048
#define CBOR_PRETTY_PRINTER 1

#if defined(_MSC_VER)
#    define CBOR_RESTRICT_SPECIFIER
#else
#    define CBOR_RESTRICT_SPECIFIER restrict
#endif

#define CBOR_INLINE_SPECIFIER

/* Ignore the compiler warnings for libcbor. */
#ifdef _MSC_VER
#    pragma warning(disable : 4028)
#    pragma warning(disable : 4715)
#    pragma warning(disable : 4232)
#    pragma warning(disable : 4068)
#    pragma warning(disable : 4244)
#    pragma warning(disable : 4701)
#    pragma warning(disable : 4703)
#endif

#ifdef __clang__
#    pragma clang diagnostic ignored "-Wreturn-type"
#elif defined(__GNUC__)
#    pragma GCC diagnostic ignored "-Wreturn-type"
#    pragma GCC diagnostic ignored "-Wunknown-pragmas"
#    pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#endif // LIBCBOR_CONFIGURATION_H
