
#ifndef MONGOCRYPT_EXPORT_H
#define MONGOCRYPT_EXPORT_H

#ifdef MONGOCRYPT_STATIC_DEFINE
#  define MONGOCRYPT_EXPORT
#  define MONGOCRYPT_NO_EXPORT
#else
#  ifndef MONGOCRYPT_EXPORT
#    ifdef mongocrypt_EXPORTS
        /* We are building this library */
#      define MONGOCRYPT_EXPORT __declspec(dllexport)
#    else
        /* We are using this library */
#      define MONGOCRYPT_EXPORT __declspec(dllimport)
#    endif
#  endif

#  ifndef MONGOCRYPT_NO_EXPORT
#    define MONGOCRYPT_NO_EXPORT 
#  endif
#endif

#ifndef MONGOCRYPT_DEPRECATED
#  define MONGOCRYPT_DEPRECATED __declspec(deprecated)
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
