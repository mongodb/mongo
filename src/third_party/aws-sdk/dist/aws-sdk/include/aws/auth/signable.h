#ifndef AWS_AUTH_SIGNABLE_H
#define AWS_AUTH_SIGNABLE_H

#include <aws/auth/auth.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http_message;
struct aws_http_headers;
struct aws_input_stream;
struct aws_signable;
struct aws_string;

/*
 * While not referenced directly in this file, this is the structure expected to be in the property lists
 */
struct aws_signable_property_list_pair {
    struct aws_byte_cursor name;
    struct aws_byte_cursor value;
};

typedef int(aws_signable_get_property_fn)(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value);

typedef int(aws_signable_get_property_list_fn)(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_list);

typedef int(aws_signable_get_payload_stream_fn)(
    const struct aws_signable *signable,
    struct aws_input_stream **out_input_stream);

typedef void(aws_signable_destroy_fn)(struct aws_signable *signable);

struct aws_signable_vtable {
    aws_signable_get_property_fn *get_property;
    aws_signable_get_property_list_fn *get_property_list;
    aws_signable_get_payload_stream_fn *get_payload_stream;
    aws_signable_destroy_fn *destroy;
};

/**
 * Signable is a generic interface for any kind of object that can be cryptographically signed.
 *
 * Like signing_result, the signable interface presents
 *
 *   (1) Properties - A set of key-value pairs
 *   (2) Property Lists - A set of named key-value pair lists
 *
 * as well as
 *
 *   (3) A message payload modeled as a stream
 *
 * When creating a signable "subclass" the query interface should map to retrieving
 * the properties of the underlying object needed by signing algorithms that can operate on it.
 *
 * As an example, if a signable implementation wrapped an http request, you would query
 * request elements like method and uri from the property interface, headers would be queried
 * via the property list interface, and the request body would map to the payload stream.
 *
 * String constants that map to agreed on keys for particular signable types
 * ("METHOD", "URI", "HEADERS", etc...) are exposed in appropriate header files.
 */
struct aws_signable {
    struct aws_allocator *allocator;
    void *impl;
    struct aws_signable_vtable *vtable;
};

AWS_EXTERN_C_BEGIN

/**
 * Cleans up and frees all resources associated with a signable instance
 *
 * @param signable signable object to destroy
 */
AWS_AUTH_API
void aws_signable_destroy(struct aws_signable *signable);

/**
 * Retrieves a property (key-value pair) from a signable.  Global property name constants are
 * included below.
 *
 * @param signable signable object to retrieve a property from
 * @param name name of the property to query
 * @param out_value output parameter for the property's value
 *
 * @return AWS_OP_SUCCESS if the property was successfully fetched, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signable_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value);

/**
 * Retrieves a named property list (list of key-value pairs) from a signable.  Global property list name
 * constants are included below.
 *
 * @param signable signable object to retrieve a property list from
 * @param name name of the property list to fetch
 * @param out_property_list output parameter for the fetched property list
 *
 * @return AWS_OP_SUCCESS if the property list was successfully fetched, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signable_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_property_list);

/**
 * Retrieves the signable's message payload as a stream.
 *
 * @param signable signable to get the payload of
 * @param out_input_stream output parameter for the payload stream
 *
 * @return AWS_OP_SUCCESS if successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signable_get_payload_stream(const struct aws_signable *signable, struct aws_input_stream **out_input_stream);

/*
 * Some global property and property-list name constants
 */

/**
 * Name of the property list that wraps the headers of an http request
 */
AWS_AUTH_API extern const struct aws_string *g_aws_http_headers_property_list_name;

/**
 * Name of the property list that wraps the query params of an http request.  Only used by signing_result.
 * For input to a http signing algorithm, query params are assumed to be part of the uri.
 */
AWS_AUTH_API extern const struct aws_string *g_aws_http_query_params_property_list_name;

/**
 * Name of the property that holds the method of an http request
 */
AWS_AUTH_API extern const struct aws_string *g_aws_http_method_property_name;

/**
 * Name of the property that holds the URI of an http request
 */
AWS_AUTH_API extern const struct aws_string *g_aws_http_uri_property_name;

/**
 * Name of the property that holds the signature value.  This is always added to signing results.
 * Depending on the requested signature type, the signature may be padded or encoded differently:
 *   (1) Header - hex encoding of the binary signature value
 *   (2) QueryParam - hex encoding of the binary signature value
 *   (3) Chunk/Sigv4 - hex encoding of the binary signature value
 *   (4) Chunk/Sigv4a - fixed-size-rhs-padded (with AWS_SIGV4A_SIGNATURE_PADDING_BYTE) hex encoding of the
 *       binary signature value
 *   (5) Event - binary signature value (NYI)
 */
AWS_AUTH_API extern const struct aws_string *g_aws_signature_property_name;

/**
 * Name of the property that holds the (hex-encoded) signature value of the signing event that preceded this one.
 * This property must appear on signables that represent chunks or events.
 */
AWS_AUTH_API extern const struct aws_string *g_aws_previous_signature_property_name;

/**
 * Name of the property that holds the canonical request associated with this signable.
 * This property must appear on signables that represent an http request's canonical request.
 */
AWS_AUTH_API extern const struct aws_string *g_aws_canonical_request_property_name;

/*
 * Common signable constructors
 */

/**
 * Creates a signable wrapper around an http request.
 *
 * @param allocator memory allocator to use to create the signable
 * @param request http request to create a signable for
 *
 * @return the new signable object, or NULL if failure
 */
AWS_AUTH_API
struct aws_signable *aws_signable_new_http_request(struct aws_allocator *allocator, struct aws_http_message *request);

/**
 * Creates a signable that represents a unit of chunked encoding within an http request.
 * This can also be used for Transcribe event signing with encoded payload as chunk_data.
 *
 * @param allocator memory allocator use to create the signable
 * @param chunk_data stream representing the data in the chunk; it should be in its final, encoded form
 * @param previous_signature the signature computed in the most recent signing that preceded this one.  It can be
 * found by copying the "signature" property from the signing_result of that most recent signing.
 *
 * @return the new signable object, or NULL if failure
 */
AWS_AUTH_API
struct aws_signable *aws_signable_new_chunk(
    struct aws_allocator *allocator,
    struct aws_input_stream *chunk_data,
    struct aws_byte_cursor previous_signature);

/**
 * Creates a signable wrapper around a set of headers.
 *
 * @param allocator memory allocator use to create the signable
 * @param trailing_headers http headers to create a signable for
 * @param previous_signature the signature computed in the most recent signing that preceded this one.  It can be
 * found by copying the "signature" property from the signing_result of that most recent signing.
 *
 * @return the new signable object, or NULL if failure
 */
AWS_AUTH_API
struct aws_signable *aws_signable_new_trailing_headers(
    struct aws_allocator *allocator,
    struct aws_http_headers *trailing_headers,
    struct aws_byte_cursor previous_signature);

/**
 * Creates a signable that represents a pre-computed canonical request from an http request
 * @param allocator memory allocator use to create the signable
 * @param canonical_request text of the canonical request
 * @return the new signable object, or NULL if failure
 */
AWS_AUTH_API
struct aws_signable *aws_signable_new_canonical_request(
    struct aws_allocator *allocator,
    struct aws_byte_cursor canonical_request);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_AUTH_SIGNABLE_H */
