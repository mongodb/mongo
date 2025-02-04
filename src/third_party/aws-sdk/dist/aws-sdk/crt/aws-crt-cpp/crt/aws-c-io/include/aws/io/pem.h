#ifndef AWS_IO_PEM_H
#define AWS_IO_PEM_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/io.h>

AWS_EXTERN_C_BEGIN

/*
 * Naming follows OpenSSL convention for PEM types.
 * Refer to comment after each enum value for the type string it represents.
 */
enum aws_pem_object_type {
    AWS_PEM_TYPE_UNKNOWN = 0,
    AWS_PEM_TYPE_X509_OLD,                /* X509 CERTIFICATE */
    AWS_PEM_TYPE_X509,                    /* CERTIFICATE */
    AWS_PEM_TYPE_X509_TRUSTED,            /* TRUSTED CERTIFICATE */
    AWS_PEM_TYPE_X509_REQ_OLD,            /* NEW CERTIFICATE REQUEST */
    AWS_PEM_TYPE_X509_REQ,                /* CERTIFICATE REQUEST */
    AWS_PEM_TYPE_X509_CRL,                /* X509 CRL */
    AWS_PEM_TYPE_EVP_PKEY,                /* ANY PRIVATE KEY */
    AWS_PEM_TYPE_PUBLIC_PKCS8,            /* PUBLIC KEY */
    AWS_PEM_TYPE_PRIVATE_RSA_PKCS1,       /* RSA PRIVATE KEY */
    AWS_PEM_TYPE_PUBLIC_RSA_PKCS1,        /* RSA PUBLIC KEY */
    AWS_PEM_TYPE_PRIVATE_DSA_PKCS1,       /* RSA PRIVATE KEY */
    AWS_PEM_TYPE_PUBLIC_DSA_PKCS1,        /* RSA PUBLIC KEY */
    AWS_PEM_TYPE_PKCS7,                   /* PKCS7 */
    AWS_PEM_TYPE_PKCS7_SIGNED_DATA,       /* PKCS #7 SIGNED DATA */
    AWS_PEM_TYPE_PRIVATE_PKCS8_ENCRYPTED, /* ENCRYPTED PRIVATE KEY */
    AWS_PEM_TYPE_PRIVATE_PKCS8,           /* PRIVATE KEY */
    AWS_PEM_TYPE_DH_PARAMETERS,           /* X9.42 DH PARAMETERS */
    AWS_PEM_TYPE_DH_PARAMETERS_X942,      /* X9.42 DH PARAMETERS */
    AWS_PEM_TYPE_SSL_SESSION_PARAMETERS,  /* SSL SESSION PARAMETERS */
    AWS_PEM_TYPE_DSA_PARAMETERS,          /* DSA PARAMETERS */
    AWS_PEM_TYPE_ECDSA_PUBLIC,            /* ECDSA PUBLIC KEY */
    AWS_PEM_TYPE_EC_PARAMETERS,           /* EC PARAMETERS */
    AWS_PEM_TYPE_EC_PRIVATE,              /* EC PRIVATE KEY */
    AWS_PEM_TYPE_PARAMETERS,              /* PARAMETERS */
    AWS_PEM_TYPE_CMS,                     /* CMS */
    AWS_PEM_TYPE_SM2_PARAMETERS           /* SM2 PARAMETERS */
};

/*
 * Describes PEM object decoded from file.
 * data points to raw data bytes of object (decoding will do additional base 64
 * decoding for each object).
 * type will be set to object type or to AWS_PEM_TYPE_UNKNOWN if it could not
 * figure out type.
 * type_string is the string between -----BEGIN and -----
 */
struct aws_pem_object {
    enum aws_pem_object_type type;
    struct aws_string *type_string;
    struct aws_byte_buf data;
};

/**
 * Cleans up elements of pem_objects list 'aws_pem_objects_init_from_file_contents()'
 * and 'aws_pem_objects_init_from_file_path()'.
 */
AWS_IO_API void aws_pem_objects_clean_up(struct aws_array_list *pem_objects);

/**
 * Decodes PEM data and reads objects sequentially adding them to pem_objects.
 * If it comes across an object it cannot read, list of all object read until
 * that point is returned.
 * If no objects can be read from PEM or objects could not be base 64 decoded,
 * AWS_ERROR_PEM_MALFORMED is raised.
 * out_pem_objects stores aws_pem_object struct by value.
 * Function will initialize pem_objects list.
 * This code is slow, and it allocates, so please try
 * not to call this in the middle of something that needs to be fast or resource sensitive.
 */
AWS_IO_API int aws_pem_objects_init_from_file_contents(
    struct aws_array_list *pem_objects,
    struct aws_allocator *alloc,
    struct aws_byte_cursor pem_cursor);

/**
 * Decodes PEM data from file and reads objects sequentially adding them to pem_objects.
 * If it comes across an object it cannot read, list of all object read until
 * that point is returned.
 * If no objects can be read from PEM or objects could not be base 64 decoded,
 * AWS_ERROR_PEM_MALFORMED is raised.
 * out_pem_objects stores aws_pem_object struct by value.
 * Function will initialize pem_objects list.
 * This code is slow, and it allocates, so please try
 * not to call this in the middle of something that needs to be fast or resource sensitive.
 */
AWS_IO_API int aws_pem_objects_init_from_file_path(
    struct aws_array_list *pem_objects,
    struct aws_allocator *allocator,
    const char *filename);

AWS_EXTERN_C_END
#endif /* AWS_IO_PEM_H */
