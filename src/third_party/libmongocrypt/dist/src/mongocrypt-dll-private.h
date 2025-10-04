#ifndef MONGOCRYPT_DLL_PRIVATE_H
#define MONGOCRYPT_DLL_PRIVATE_H

#include <mlib/error.h>
#include <mlib/str.h>

#include <stdlib.h>

#if defined(_WIN32)
#define MCR_DLL_SUFFIX ".dll"
#elif defined(__APPLE__)
#define MCR_DLL_SUFFIX ".dylib"
#else
#define MCR_DLL_SUFFIX ".so"
#endif

#define MCR_DLL_NULL ((mcr_dll){._native_handle = NULL, .error_string = MSTR_NULL})

/**
 * @brief A dynamically-loaded library i.e. returned by LoadLibrary() or
 * dlopen()
 */
typedef struct mcr_dll {
    // (All supported platforms use a void* as the library handle type)
    void *_native_handle;
    mstr error_string;
} mcr_dll;

/**
 * @brief Open and load a dynamic library
 *
 * @param lib A path or name of a library, suitable for encoding as a
 * filepath on the host system.
 *
 * @return mcr_dll A newly opened dynamic library, which must be
 * released using @ref mcr_dll_close()
 *
 * If the given `lib` is a qualified path (either relative or absolute) and not
 * just a filename, then the system will search for the library on the system's
 * default library search paths. If `lib` is a qualified relative path (not just
 * a filename), it will be resolved relative to the application's working
 * directory.
 */
mcr_dll mcr_dll_open(const char *lib);

/**
 * @brief Close a dynamic library opened with @ref mcr_dll_open
 *
 * @param dll A dynamic library handle
 */
static inline void mcr_dll_close(mcr_dll dll) {
    extern void mcr_dll_close_handle(mcr_dll);
    mcr_dll_close_handle(dll);
    mstr_free(dll.error_string);
}

// All currently supported platforms require casting to/from `void*` for dynamically loaded function symbols by
// necessity despite being undefined behavior according to the C standard specification.
#if defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
// GCC requires silencing `-Wpedantic` for this cast.
#define MC_BEGIN_CAST_FUNCTION_TYPE_STRICT_IGNORE                                                                      \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wpedantic\"")
#define MC_END_CAST_FUNCTION_TYPE_STRICT_IGNORE _Pragma("GCC diagnostic pop")
#elif defined(__clang__)
#define MC_BEGIN_CAST_FUNCTION_TYPE_STRICT_IGNORE                                                                      \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wunknown-warning-option\"")                  \
        _Pragma("clang diagnostic ignored \"-Wcast-function-type-strict\"")
#define MC_END_CAST_FUNCTION_TYPE_STRICT_IGNORE _Pragma("clang diagnostic pop")
#else
#define MC_BEGIN_CAST_FUNCTION_TYPE_STRICT_IGNORE
#define MC_END_CAST_FUNCTION_TYPE_STRICT_IGNORE
#endif

/**
 * @brief Obtain a pointer to an exported entity from the given dynamic library.
 *
 * @param dll A library opened with @ref mcr_dll_open
 * @param symbol The name of a symbol to open
 * @return void* A pointer to that symbol, or NULL if not found
 */
void *mcr_dll_sym(mcr_dll dll, const char *symbol);

/**
 * @brief Determine whether the given DLL is a handle to an open library
 */
static inline bool mcr_dll_is_open(mcr_dll dll) {
    return dll._native_handle != NULL;
}

typedef struct mcr_dll_path_result {
    mstr path;
    mstr error_string;
} mcr_dll_path_result;

/**
 * @brief Obtain a filepath to the given loaded DLL
 *
 * @param dll The library loaded to inspect
 * @return mcr_dll_path_result A result containing the absolute path to a
 * library, or an error string.
 *
 * @note Caller must free both `retval.path` and `retval.error_string`.
 * @note Returns an error if not supported on this platform. Use
 * `mcr_dll_path_supported` to check before calling.
 */
mcr_dll_path_result mcr_dll_path(mcr_dll dll);

/**
 * @brief Return true if `mcr_dll_path` is supported on this platform.
 */
bool mcr_dll_path_supported(void);

#endif // MONGOCRYPT_DLL_PRIVATE_H
