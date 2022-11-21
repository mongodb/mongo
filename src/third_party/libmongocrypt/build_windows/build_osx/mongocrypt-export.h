
#ifndef MONGOCRYPT_EXPORT_H
#define MONGOCRYPT_EXPORT_H

#ifdef MONGOCRYPT_STATIC_DEFINE
#  define MONGOCRYPT_EXPORT
#  define MONGOCRYPT_NO_EXPORT
#else
#  ifndef MONGOCRYPT_EXPORT
#    ifdef mongocrypt_EXPORTS
        /* We are building this library */
#      define MONGOCRYPT_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define MONGOCRYPT_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef MONGOCRYPT_NO_EXPORT
#    define MONGOCRYPT_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef MONGOCRYPT_DEPRECATED
#  define MONGOCRYPT_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef MONGOCRYPT_DEPRECATED_EXPORT
#  define MONGOCRYPT_DEPRECATED_EXPORT MONGOCRYPT_EXPORT MONGOCRYPT_DEPRECATED
#endif

#ifndef MONGOCRYPT_DEPRECATED_NO_EXPORT
#  define MONGOCRYPT_DEPRECATED_NO_EXPORT MONGOCRYPT_NO_EXPORT MONGOCRYPT_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef MONGOCRYPT_NO_DEPRECATED
#    define MONGOCRYPT_NO_DEPRECATED
#  endif
#endif

#endif /* MONGOCRYPT_EXPORT_H */
