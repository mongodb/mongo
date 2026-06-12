#ifndef BSON_VALIDATE_PRIVATE_H_INCLUDED
#define BSON_VALIDATE_PRIVATE_H_INCLUDED

#include <bson/bson-types.h>

enum {
   /**
    * @brief This compile-time constant represents the maximum document nesting
    * depth permitted by the `bson_validate` family of functions. If the nesting
    * depth exceeds this limit, the data will be rejected.
    *
    * This limit is intentionally larger than the default limit of MongoDB
    * server, since we cannot anticipate what a libbson user might actually want
    * to do with BSON, and to prevent accidentally rejecting data that the
    * server might accept. The main purpose of this limit is to prevent stack
    * overflow, not to reject invalid data.
    */
   BSON_VALIDATION_MAX_NESTING_DEPTH = 500,
};

/**
 * @brief Private function backing the implementation of validation.
 *
 * Validation was previously defined in the overburdened `bson-iter.c`, but it
 * is now defined in its own file.
 *
 * @param bson The document to validate. Must be non-null.
 * @param flags Validation control flags
 * @param offset Receives the offset at which validation failed. Must be non-null.
 * @param error Receives the error describing why validation failed. Must be non-null.
 * @return true If the given document has no validation errors
 * @return false Otherwise
 */
bool
_bson_validate_impl_v2(const bson_t *bson, bson_validate_flags_t flags, size_t *offset, bson_error_t *error);

#endif // BSON_VALIDATE_PRIVATE_H_INCLUDED
