/**
 * @file bson/validate.c
 * @brief Implementation of BSON document validation
 * @date 2025-05-28
 *
 * This file implements the backend for the `bson_validate` family of functions.
 *
 * The `_validate_...` functions all accept `validator* self` as their first parameter,
 * and must `return false` AND set `self->error` if-and-only-if they encounter a validation error.
 * If a function returns true, it is assumed that validation of that item succeeded.
 *
 * For brevity, the `require...` macros are defined, which check conditions, set errors,
 * and `return false` inline.
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/validate-private.h>

#include <bson/bson.h>

#include <mlib/intencode.h>
#include <mlib/test.h>

#include <stdbool.h>
#include <string.h>

/**
 * @brief User parameters for validation behavior. These correspond to the various
 * flags that can be given when the user requests validation
 */
typedef struct {
   /**
    * @brief Should we allow invalid UTF-8 in string components?
    *
    * This affects the behavior of validation of key strings and string-like
    * elements that require UTF-8 encoding.
    *
    * Technically invalid UTF-8 is invalid in BSON, but applications may already
    * rely on this being accepted.
    */
   bool allow_invalid_utf8;
   /**
    * @brief Should we allow a zero-valued codepoint in text?
    *
    * Unicode U+0000 is a valid codepoint, but a lot of software doesn't like
    * it and handles it poorly. By default, we reject it, but the user may
    * want to allow it.
    *
    * Note that because element keys rely on null termination, element keys
    * cannot contain U+0000 by construction.
    */
   bool allow_null_in_utf8;
   /// Should we allow element key strings to be empty strings?
   bool allow_empty_keys;
   /// Should we allow ASCII dot "." in element key strings?
   bool allow_dot_in_keys;
   /**
    * @brief Check for special element keys that begin with an ASCII dollar "$"
    *
    * By default, we ignore them and treat them as regular elements. If this is
    * enabled, we reject key strings that start with a dollar, unless it is a
    * special extended JSON DBRef document.
    *
    * This also enables DBRef validation, which checks the structure of a document
    * whose first key is "$ref".
    */
   bool check_special_dollar_keys;
} validation_params;

/**
 * @brief State for a validator.
 */
typedef struct {
   /// The parameters that control validation behavior
   const validation_params *params;
   /// Error storage that is updated if any validation encounters an error
   bson_error_t error;
   /// The zero-based index of the byte where validation stopped in case of an error.
   size_t error_offset;
} validator;

// Undef these macros, if they are defined.
#ifdef require_with_error
#undef require_with_error
#endif
#ifdef require
#undef require
#endif
#ifdef require_advance
#undef require_advance
#endif

/**
 * @brief Check that the given condition is satisfied, or set an error and return `false`
 *
 * @param Condition The condition that should evaluate to `true`
 * @param Offset The byte offset where an error should be indicated.
 * @param Code The error code that should be set if the condition fails
 * @param ... The error string and format arguments to be used in the error message
 *
 * This macro assumes a `validator* self` is in scope. This macro will evaluate `return false`
 * if the given condition is not true.
 */
#define require_with_error(Condition, Offset, Code, ...)                   \
   if (!(Condition)) {                                                     \
      self->error_offset = (Offset);                                       \
      bson_set_error(&self->error, BSON_ERROR_INVALID, Code, __VA_ARGS__); \
      return false;                                                        \
   } else                                                                  \
      ((void)0)

/**
 * @brief Check that the given condition is satisfied, or `return false` immediately.
 *
 * This macro does not modify the validator state. It only does an early-return.
 */
#define require(Cond) \
   if (!(Cond)) {     \
      return false;   \
   } else             \
      ((void)0)

/**
 * @brief Advance the pointed-to iterator, check for errors, and test whether we are done.
 *
 * @param DoneVar An l-value of type `bool` that is set to `true` if the iterator hit the end of
 * the document, otherwise `false`
 * @param IteratorPointer An expression of type `bson_iter_t*`, which will be advanced.
 *
 * If advancing the iterator results in a decoding error, then this macro sets an error
 * on the `validator* self` that is in scope and will immediately `return false`.
 */
#define require_advance(DoneVar, IteratorPointer)                                                      \
   if ((DoneVar = !bson_iter_next(IteratorPointer))) {                                                 \
      /* The iterator indicates that it stopped */                                                     \
      if ((IteratorPointer)->err_off) {                                                                \
         /* The iterator stopped because of a decoding error */                                        \
         require_with_error(false, (IteratorPointer)->err_off, BSON_VALIDATE_CORRUPT, "corrupt BSON"); \
      }                                                                                                \
   } else                                                                                              \
      ((void)0)

// Test if the element's key is equal to the given string
static bool
_key_is(bson_iter_t const *iter, const char *const key)
{
   BSON_ASSERT_PARAM(iter);
   BSON_ASSERT_PARAM(key);
   return !strcmp(bson_iter_key(iter), key);
}

/**
 * @brief Validate a document or array object, recursively.
 *
 * @param self The validator which will be updated and used to do the validation
 * @param bson The object to be validated
 * @param depth The validation depth. We indicate an error if this exceeds a limit.
 * @return true If the object is valid
 * @return false Otherwise
 */
static bool
_validate_doc(validator *self, const bson_t *bson, int depth);

/**
 * @brief Validate a UTF-8 string, if-and-only-if UTF-8 validation is requested
 *
 * @param self Pointer to the validator object
 * @param offset The byte-offset of the string, used to set the error offset
 * @param u8 Pointer to the first byte in a UTF-8 string
 * @param u8len The length of the array pointed-to by `u8`
 * @return true If the UTF-8 string is valid, or if UTF-8 validation is disabled
 * @return false If UTF-8 validation is requested, AND (the UTF-8 string is invalid OR (UTF-8 strings should not contain
 * null characters and the UTF-8 string contains a null character))
 */
static bool
_maybe_validate_utf8(validator *self, size_t offset, const char *u8, size_t u8len)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(u8);
   if (self->params->allow_invalid_utf8) {
      // We are not doing UTF-8 checks, so always succeed
      return true;
   }
   // Validate UTF-8
   const bool u8okay = bson_utf8_validate(u8, u8len, self->params->allow_null_in_utf8);
   if (u8okay) {
      // Valid UTF-8, no more checks
      return true;
   }
   // Validation error. It may be invalid UTF-8, or it could be valid UTF-8 with a disallowed null
   if (!self->params->allow_null_in_utf8) {
      // We are disallowing null in UTF-8. Check whether it is invalid UTF-8, or is
      // valid UTF-8 with a null character
      const bool u8okay_with_null = bson_utf8_validate(u8, u8len, true);
      if (u8okay_with_null) {
         // The UTF-8 is valid, but contains a null character.
         require_with_error(
            false, offset, BSON_VALIDATE_UTF8_ALLOW_NULL, "UTF-8 string contains a U+0000 (null) character");
      }
   }
   // The UTF-8 is invalid, regardless of whether it contains a null character
   require_with_error(false, offset, BSON_VALIDATE_UTF8, "Text element is not valid UTF-8");
}

// Same as `_maybe_validate_u8`, but relies on a null-terminated C string to get the string length
static bool
_maybe_validate_utf8_cstring(validator *self, size_t offset, const char *const u8)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(u8);
   return _maybe_validate_utf8(self, offset, u8, strlen(u8));
}

/**
 * @brief Validate a string-like element (UTF-8, Symbol, or Code)
 *
 * This function relies on the representation of the text-like elements within
 * the iterator struct to reduce code dup around text validation.
 */
static bool
_validate_stringlike_element(validator *self, bson_iter_t const *iter)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   // iter->d1 is the offset to the string header. Subtract 1 to exclude the null terminator
   const uint32_t u8len = mlib_read_u32le(iter->raw + iter->d1) - 1;
   // iter->d2 is the offset to the first byte of the string
   const char *u8 = (const char *)iter->raw + iter->d2;
   return _maybe_validate_utf8(self, iter->off, u8, u8len);
}

static bool
_validate_regex_elem(validator *self, bson_iter_t const *iter)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   mlib_check(BSON_ITER_HOLDS_REGEX(iter));
   const char *opts;
   const char *const rx = bson_iter_regex(iter, &opts);
   mlib_check(rx);
   mlib_check(opts);
   return _maybe_validate_utf8_cstring(self, iter->off, rx) //
          && _maybe_validate_utf8_cstring(self, iter->off, opts);
}

static bool
_validate_codewscope_elem(validator *self, bson_iter_t const *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   mlib_check(BSON_ITER_HOLDS_CODEWSCOPE(iter));
   // Extract the code and the scope object
   uint8_t const *doc;
   uint32_t doc_len;
   uint32_t u8len;
   const char *const u8 = bson_iter_codewscope(iter, &u8len, &doc_len, &doc);
   bson_t scope;
   require_with_error(
      bson_init_static(&scope, doc, doc_len), iter->off, BSON_VALIDATE_CORRUPT, "corrupt scope document");

   // Validate the code string
   require(_maybe_validate_utf8(self, iter->off, u8, u8len));

   // Now we validate the scope object.
   // Don't validate the scope document using the parent parameters, because it should
   // be treated as an opaque closure of JS variables.
   validation_params const scope_params = {
      // JS object keys can contain dots
      .allow_dot_in_keys = true,
      // JS object keys can be empty
      .allow_empty_keys = true,
      // JS strings can contain null bytes
      .allow_null_in_utf8 = true,
      // JS strings need to encode properly
      .allow_invalid_utf8 = false,
      // JS allows object keys to have dollars
      .check_special_dollar_keys = false,
   };
   validator scope_validator = {.params = &scope_params};
   // We could do more validation that the scope keys are valid JS identifiers,
   // but that would require using a full Unicode database.
   if (_validate_doc(&scope_validator, &scope, depth)) {
      // No error
      return true;
   }
   // Validation error. Copy the error message, adding the name of the bad element
   bson_set_error(&self->error,
                  scope_validator.error.domain,
                  scope_validator.error.code,
                  "Error in scope document for element \"%s\": %s",
                  bson_iter_key(iter),
                  scope_validator.error.message);
   // Adjust the error offset by the offset of the iterator
   self->error_offset = scope_validator.error_offset + iter->off;
   return false;
}

// Validate an element's key string according to the validation rules
static bool
_validate_element_key(validator *self, bson_iter_t const *iter)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);

   const char *const key = bson_iter_key(iter);
   mlib_check(key);
   const size_t key_len = bson_iter_key_len(iter);

   // Check the UTF-8 of the key
   require(_maybe_validate_utf8(self, iter->off, key, key_len));

   // Check for special keys
   if (self->params->check_special_dollar_keys) {
      // dollar-keys are checked during the startup of _validate_doc. If we get here, there's a problem.
      require_with_error(
         key[0] != '$', iter->off, BSON_VALIDATE_DOLLAR_KEYS, "Disallowed '$' in element key: \"%s\"", key);
   }

   if (!self->params->allow_empty_keys) {
      require_with_error(key_len != 0, iter->off, BSON_VALIDATE_EMPTY_KEYS, "Element key cannot be an empty string");
   }

   if (!self->params->allow_dot_in_keys) {
      require_with_error(
         !strstr(key, "."), iter->off, BSON_VALIDATE_DOT_KEYS, "Disallowed '.' in element key: \"%s\"", key);
   }

   return true;
}

// Extract a document referred-to by the given iterator. It must point to a
// document or array element. Returns `false` if `bson_init_static` returns false
static bool
_get_subdocument(bson_t *subdoc, bson_iter_t const *iter)
{
   BSON_ASSERT_PARAM(subdoc);
   BSON_ASSERT_PARAM(iter);
   uint32_t len = mlib_read_u32le(iter->raw + iter->d1);
   uint8_t const *data = (uint8_t const *)iter->raw + iter->d1;
   return bson_init_static(subdoc, data, len);
}

// Validate the value of an element, without checking its key
static bool
_validate_element_value(validator *self, bson_iter_t const *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);

   const bson_type_t type = bson_iter_type(iter);
   switch (type) {
   default:
   case BSON_TYPE_EOD:
      BSON_UNREACHABLE("Validation execution encountered an element of type 0x0, but this should not happen as tag "
                       "validation is handled before we get to this point.");
   case BSON_TYPE_DOUBLE:
   case BSON_TYPE_NULL:
   case BSON_TYPE_OID:
   case BSON_TYPE_INT32:
   case BSON_TYPE_INT64:
   case BSON_TYPE_MINKEY:
   case BSON_TYPE_MAXKEY:
   case BSON_TYPE_TIMESTAMP:
   case BSON_TYPE_UNDEFINED:
   case BSON_TYPE_DECIMAL128:
   case BSON_TYPE_DATE_TIME:
   case BSON_TYPE_BOOL:
      // No validation on these simple scalar elements. `bson_iter_next` does validation
      // on these objects for us.
      return true;
   case BSON_TYPE_BINARY:
      // Note: BSON binary validation is handled by bson_iter_next, which checks the
      // internal structure properly. If we get here, then the binary data is okay.
      return true;
   case BSON_TYPE_DBPOINTER:
      // DBPointer contains more than just a string, but we only need to validate
      // the string component, which happens to align with the repr of other stringlike
      // elements. bson_iter_next will do the validation on the element's size.
      //! fallthrough
   case BSON_TYPE_SYMBOL:
   case BSON_TYPE_CODE:
   case BSON_TYPE_UTF8:
      return _validate_stringlike_element(self, iter);
   case BSON_TYPE_DOCUMENT:
   case BSON_TYPE_ARRAY: {
      bson_t doc;
      require_with_error(_get_subdocument(&doc, iter), iter->off, BSON_VALIDATE_CORRUPT, "corrupt BSON");
      if (_validate_doc(self, &doc, depth)) {
         // No error
         return true;
      }
      // Error in subdocument. Adjust the error offset for the current iterator position,
      // plus the key length, plus 2 for the tag and key's null terminator.
      self->error_offset += iter->off + bson_iter_key_len(iter) + 2;
      return false;
   }

   case BSON_TYPE_REGEX:
      return _validate_regex_elem(self, iter);
   case BSON_TYPE_CODEWSCOPE:
      return _validate_codewscope_elem(self, iter, depth);
   }
}

// Validate a single BSON element referred-to by the given iterator
static bool
_validate_element(validator *self, bson_iter_t *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   return _validate_element_key(self, iter) && _validate_element_value(self, iter, depth);
}

/**
 * @brief Validate the elements of a document, beginning with the element pointed-to
 * by the given iterator.
 */
static bool
_validate_remaining_elements(validator *self, bson_iter_t *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   bool done = false;
   while (!done) {
      require(_validate_element(self, iter, depth));
      require_advance(done, iter);
   }
   return true;
}

// Do validation for a DBRef document, indicated by a leading $ref key
static bool
_validate_dbref(validator *self, bson_iter_t *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);

   // The iterator must be pointing to the initial $ref element
   mlib_check(_key_is(iter, "$ref"));
   // Check that $ref is a UTF-8 element
   require_with_error(
      BSON_ITER_HOLDS_UTF8(iter), iter->off, BSON_VALIDATE_DOLLAR_KEYS, "$ref element must be a UTF-8 element");
   require(_validate_element_value(self, iter, depth));

   // We require an $id as the next element
   bool done;
   require_advance(done, iter);
   require_with_error(
      !done && _key_is(iter, "$id"), iter->off, BSON_VALIDATE_DOLLAR_KEYS, "Expected an $id element following $ref");
   // While $id is typically a OID value, it is not constraint to any specific type, so
   // we just validate it as an arbitrary value.
   require(_validate_element_value(self, iter, depth));

   // We should stop, or we should have a $db, or we may have other elements
   require_advance(done, iter);
   if (done) {
      // No more elements. Nothing left to check
      return true;
   }
   // If it's a $db, check that it's a UTF-8 string
   if (_key_is(iter, "$db")) {
      require_with_error(BSON_ITER_HOLDS_UTF8(iter),
                         iter->off,
                         BSON_VALIDATE_DOLLAR_KEYS,
                         "$db element in DBRef must be a UTF-8 element");
      require(_validate_element_value(self, iter, depth));
      // Advance past the $db
      require_advance(done, iter);
      if (done) {
         // Nothing left to do
         return true;
      }
   }
   // All subsequent elements should be validated as normal, and we don't expect
   // any more $-keys
   return _validate_remaining_elements(self, iter, depth);
}

// If we are validating special $-keys, validate a document whose first element is a $-key
static bool
_validate_dollar_doc(validator *self, bson_iter_t *iter, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(iter);
   if (_key_is(iter, "$ref")) {
      return _validate_dbref(self, iter, depth);
   }
   // Have the element key validator issue an error message about the bad $-key
   bool okay = _validate_element_key(self, iter);
   mlib_check(!okay);
   return false;
}

static bool
_validate_doc(validator *self, const bson_t *bson, int depth)
{
   BSON_ASSERT_PARAM(self);
   BSON_ASSERT_PARAM(bson);

   require_with_error(
      depth <= BSON_VALIDATION_MAX_NESTING_DEPTH, 0, BSON_VALIDATE_CORRUPT, "BSON document nesting depth is too deep");
   // We increment the depth here, otherwise we'd have `depth + 1` in several places.
   ++depth;

   // Initialize an iterator into the document to be validated
   bson_iter_t iter;
   require_with_error(
      bson_iter_init(&iter, bson), 0, BSON_VALIDATE_CORRUPT, "Document header corruption, unable to iterate");
   bool done;
   require_advance(done, &iter);
   if (done) {
      // Nothing to check (empty doc/array)
      return true;
   }

   // Check if the first key starts with a dollar
   if (self->params->check_special_dollar_keys) {
      const char *const key = bson_iter_key(&iter);
      if (key[0] == '$') {
         return _validate_dollar_doc(self, &iter, depth);
      }
   }

   return _validate_remaining_elements(self, &iter, depth);
}

// This private function is called by `bson_validate_with_error_and_offset`
bool
_bson_validate_impl_v2(const bson_t *bson, bson_validate_flags_t flags, size_t *offset, bson_error_t *error)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(offset);
   BSON_ASSERT_PARAM(error);

   // Clear the error
   *error = (bson_error_t){0};

   // Initialize validation parameters
   validation_params const params = {
      .allow_invalid_utf8 = !(flags & BSON_VALIDATE_UTF8),
      .allow_null_in_utf8 = flags & BSON_VALIDATE_UTF8_ALLOW_NULL,
      .check_special_dollar_keys = (flags & BSON_VALIDATE_DOLLAR_KEYS),
      .allow_dot_in_keys = !(flags & BSON_VALIDATE_DOT_KEYS),
      .allow_empty_keys = !(flags & BSON_VALIDATE_EMPTY_KEYS),
   };

   // Start the validator on the root document
   validator v = {.params = &params};
   bool okay = _validate_doc(&v, bson, 0);
   *offset = v.error_offset;
   *error = v.error;
   mlib_check(okay == (v.error.code == 0) &&
              "Validation routine should return `false` if-and-only-if it sets an error code");
   return okay;
}
