
#ifndef RNP_EXPORT
#define RNP_EXPORT

#ifdef RNP_STATIC
#  define RNP_API
#  define RNP_NO_EXPORT
#else
#  ifndef RNP_API
#    ifdef librnp_EXPORTS
        /* We are building this library */
#      define RNP_API 
#    else
        /* We are using this library */
#      define RNP_API 
#    endif
#  endif

#  ifndef RNP_NO_EXPORT
#    define RNP_NO_EXPORT 
#  endif
#endif

#ifndef RNP_DEPRECATED
#  define RNP_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef RNP_DEPRECATED_EXPORT
#  define RNP_DEPRECATED_EXPORT RNP_API RNP_DEPRECATED
#endif

#ifndef RNP_DEPRECATED_NO_EXPORT
#  define RNP_DEPRECATED_NO_EXPORT RNP_NO_EXPORT RNP_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef RNP_NO_DEPRECATED
#    define RNP_NO_DEPRECATED
#  endif
#endif

#endif /* RNP_EXPORT */
