#include "common-prelude.h"

#ifndef BSON_BSON_DSL_H_INCLUDED
#define BSON_BSON_DSL_H_INCLUDED

/**
 * @file bson-dsl.h
 * @brief Define a C-preprocessor DSL for working with BSON objects
 *
 * This file defines an embedded DSL for working with BSON objects consisely and
 * correctly.
 *
 * For more information about using this DSL, refer to `bson-dsl.md`.
 */

#include "bson/bson.h"

enum {
   /// Toggle this value to enable/disable debug output for all bsonDSL
   /// operations (printed to stderr). You can also set a constant
   /// BSON_DSL_DEBUG within the scope of a DSL command to selectively debug
   /// only the commands within that scope.
   BSON_DSL_DEBUG = 0
};

#define _bson_thread_local BSON_IF_GNU_LIKE (__thread) BSON_IF_MSVC (__declspec (thread))

#define _bson_comdat                        \
   BSON_IF_WINDOWS (__declspec (selectany)) \
   BSON_IF_POSIX (__attribute__ ((weak)))

#ifdef __GNUC__
// GCC has a bug handling pragma statements that disable warnings within complex
// nested macro expansions. If we're GCC, just disable -Wshadow outright:
BSON_IF_GNU_LIKE (_Pragma ("GCC diagnostic ignored \"-Wshadow\""))
#endif

#define _bsonDSL_disableWarnings()                                        \
   if (1) {                                                               \
      BSON_IF_GNU_LIKE (_Pragma ("GCC diagnostic push");)                 \
      BSON_IF_GNU_LIKE (_Pragma ("GCC diagnostic ignored \"-Wshadow\"");) \
   } else                                                                 \
      ((void) 0)

#define _bsonDSL_restoreWarnings()                       \
   if (1) {                                              \
      BSON_IF_GNU_LIKE (_Pragma ("GCC diagnostic pop");) \
   } else                                                \
      ((void) 0)

/**
 * @brief Parse the given BSON document.
 *
 * @param doc A bson_t object to walk. (Not a pointer)
 */
#define bsonParse(Document, ...)                              \
   _bsonDSL_begin ("bsonParse(%s)", _bsonDSL_str (Document)); \
   _bsonDSL_disableWarnings ();                               \
   bsonParseError = NULL;                                     \
   BSON_MAYBE_UNUSED bool _bvHalt = false;                    \
   BSON_MAYBE_UNUSED const bool _bvContinue = false;          \
   BSON_MAYBE_UNUSED const bool _bvBreak = false;             \
   _bsonDSL_eval (_bsonParse ((Document), __VA_ARGS__));      \
   _bsonDSL_restoreWarnings ();                               \
   _bsonDSL_end

/**
 * @brief Visit each element of a BSON document
 */
#define bsonVisitEach(Document, ...)                              \
   _bsonDSL_begin ("bsonVisitEach(%s)", _bsonDSL_str (Document)); \
   _bsonDSL_disableWarnings ();                                   \
   BSON_MAYBE_UNUSED bool _bvHalt = false;                        \
   _bsonDSL_eval (_bsonVisitEach ((Document), __VA_ARGS__));      \
   _bsonDSL_restoreWarnings ();                                   \
   _bsonDSL_end

#define bsonBuildContext (*_bsonBuildContextThreadLocalPtr)
#define bsonVisitContext (*_bsonVisitContextThreadLocalPtr)
#define bsonVisitIter (bsonVisitContext.iter)

/// Begin any function-like macro by opening a new scope and writing a debug
/// message.
#define _bsonDSL_begin(Str, ...)        \
   if (true) {                          \
      _bsonDSLDebug (Str, __VA_ARGS__); \
   ++_bson_dsl_indent

/// End a function-like macro scope.
#define _bsonDSL_end   \
   --_bson_dsl_indent; \
   }                   \
   else ((void) 0)

/**
 * @brief Expands to a call to bson_append_{Kind}, with the three first
 * arguments filled in by the DSL context variables.
 */
#define _bsonBuildAppendArgs bsonBuildContext.doc, bsonBuildContext.key, bsonBuildContext.key_len

/**
 * The _bsonDocOperation_XYZ macros handle the top-level bsonBuild()
 * items, and any nested doc() items, with XYZ being the doc-building
 * subcommand.
 */
#define _bsonDocOperation(Command, _ignore, _count)                                    \
   if (!bsonBuildError) {                                                              \
      _bsonDocOperation_##Command;                                                     \
      if (bsonBuildError) {                                                            \
         _bsonDSLDebug ("Stopping doc() due to bsonBuildError: [%s]", bsonBuildError); \
      }                                                                                \
   }

#define _bsonValueOperation(P) _bsonValueOperation_##P

/// key-value pair with explicit key length
#define _bsonDocOperation_kvl(String, Len, Element)                            \
   _bsonDSL_begin ("\"%s\" => [%s]", String, _bsonDSL_strElide (30, Element)); \
   const char *_bbString = (String);                                           \
   const uint64_t length = (Len);                                              \
   if (bson_in_range_unsigned (int, length)) {                                 \
      _bbCtx.key = _bbString;                                                  \
      _bbCtx.key_len = (int) length;                                           \
      _bsonValueOperation (Element);                                           \
   } else {                                                                    \
      bsonBuildError = "Out-of-range key string length value";                 \
   }                                                                           \
   _bsonDSL_end

/// Key-value pair with a C-string
#define _bsonDocOperation_kv(String, Element) _bsonDocOperation_kvl ((String), strlen ((String)), Element)

/// Execute arbitrary code
#define _bsonDocOperation_do(...)                                      \
   _bsonDSL_begin ("do(%s)", _bsonDSL_strElide (30, __VA_ARGS__));     \
   do {                                                                \
      __VA_ARGS__;                                                     \
   } while (0);                                                        \
   if (bsonBuildError) {                                               \
      _bsonDSLDebug ("do() set bsonBuildError: [%s]", bsonBuildError); \
   }                                                                   \
   _bsonDSL_end

/// We must defer expansion of the nested doc() to allow "recursive" evaluation
#define _bsonValueOperation_doc _bsonValueOperationDeferred_doc _bsonDSL_nothing ()
#define _bsonArrayOperation_doc(...) _bsonArrayAppendValue (doc (__VA_ARGS__))

#define _bsonValueOperationDeferred_doc(...)                                                   \
   _bsonDSL_begin ("doc(%s)", _bsonDSL_strElide (30, __VA_ARGS__));                            \
   /* Write to this variable as the child: */                                                  \
   bson_t _bbChildDoc = BSON_INITIALIZER;                                                      \
   if (!bson_append_document_begin (_bsonBuildAppendArgs, &_bbChildDoc)) {                     \
      bsonBuildError = "Error while initializing child document: " _bsonDSL_str (__VA_ARGS__); \
   } else {                                                                                    \
      _bsonBuildAppend (_bbChildDoc, __VA_ARGS__);                                             \
      if (!bsonBuildError) {                                                                   \
         if (!bson_append_document_end (bsonBuildContext.doc, &_bbChildDoc)) {                 \
            bsonBuildError = "Error while finalizing document: " _bsonDSL_str (__VA_ARGS__);   \
         }                                                                                     \
      }                                                                                        \
   }                                                                                           \
   _bsonDSL_end

/// We must defer expansion of the nested array() to allow "recursive"
/// evaluation
#define _bsonValueOperation_array _bsonValueOperationDeferred_array _bsonDSL_nothing ()
#define _bsonArrayOperation_array(...) _bsonArrayAppendValue (array (__VA_ARGS__))

#define _bsonValueOperationDeferred_array(...)                                                  \
   _bsonDSL_begin ("array(%s)", _bsonDSL_strElide (30, __VA_ARGS__));                           \
   /* Write to this variable as the child array: */                                             \
   bson_t _bbArray = BSON_INITIALIZER;                                                          \
   if (!bson_append_array_begin (_bsonBuildAppendArgs, &_bbArray)) {                            \
      bsonBuildError = "Error while initializing child array: " _bsonDSL_str (__VA_ARGS__);     \
   } else {                                                                                     \
      _bsonBuildArray (_bbArray, __VA_ARGS__);                                                  \
      if (!bsonBuildError) {                                                                    \
         if (!bson_append_array_end (bsonBuildContext.doc, &_bbArray)) {                        \
            bsonBuildError = "Error while finalizing child array: " _bsonDSL_str (__VA_ARGS__); \
         }                                                                                      \
      } else {                                                                                  \
         _bsonDSLDebug ("Got bsonBuildError: [%s]", bsonBuildError);                            \
      }                                                                                         \
   }                                                                                            \
   _bsonDSL_end

/// Append a UTF-8 string with an explicit length
#define _bsonValueOperation_utf8_w_len(String, Len)                                 \
   if (!bson_append_utf8 (_bsonBuildAppendArgs, (String), (int) (Len))) {           \
      bsonBuildError = "Error while appending utf8 string: " _bsonDSL_str (String); \
   } else                                                                           \
      ((void) 0)
#define _bsonArrayOperation_utf8_w_len(X) _bsonArrayAppendValue (utf8_w_len (X))

/// Append a "cstr" as UTF-8
#define _bsonValueOperation_cstr(String) _bsonValueOperation_utf8_w_len ((String), strlen (String))
#define _bsonArrayOperation_cstr(X) _bsonArrayAppendValue (cstr (X))

/// Append an int32
#define _bsonValueOperation_int32(Integer)                                        \
   if (!bson_append_int32 (_bsonBuildAppendArgs, (Integer))) {                    \
      bsonBuildError = "Error while appending int32(" _bsonDSL_str (Integer) ")"; \
   } else                                                                         \
      ((void) 0)
#define _bsonArrayOperation_int32(X) _bsonArrayAppendValue (int32 (X))

/// Append an int64
#define _bsonValueOperation_int64(Integer)                                        \
   if (!bson_append_int64 (_bsonBuildAppendArgs, (Integer))) {                    \
      bsonBuildError = "Error while appending int64(" _bsonDSL_str (Integer) ")"; \
   } else                                                                         \
      ((void) 0)
#define _bsonArrayOperation_int64(X) _bsonArrayAppendValue (int64 (X))

/// Append the value referenced by a given iterator
#define _bsonValueOperation_iterValue(Iter)                                        \
   if (!bson_append_iter (_bsonBuildAppendArgs, &(Iter))) {                        \
      bsonBuildError = "Error while appending iterValue(" _bsonDSL_str (Iter) ")"; \
   } else                                                                          \
      ((void) 0)
#define _bsonArrayOperation_iterValue(X) _bsonArrayAppendValue (iterValue (X))

/// Append the BSON document referenced by the given pointer
#define _bsonValueOperation_bson(Doc)                                                     \
   if (!bson_append_document (_bsonBuildAppendArgs, &(Doc))) {                            \
      bsonBuildError = "Error while appending subdocument: bson(" _bsonDSL_str (Doc) ")"; \
   } else                                                                                 \
      ((void) 0)
#define _bsonArrayOperation_bson(X) _bsonArrayAppendValue (bson (X))

/// Append the BSON document referenced by the given pointer as an array
#define _bsonValueOperation_bsonArray(Arr)                         \
   if (!bson_append_array (_bsonBuildAppendArgs, &(Arr))) {        \
      bsonBuildError = "Error while appending subdocument array: " \
                       "bsonArray(" _bsonDSL_str (Arr) ")";        \
   } else                                                          \
      ((void) 0)
#define _bsonArrayOperation_bsonArray(X) _bsonArrayAppendValue (bsonArray (X))

#define _bsonValueOperation_bool(b)                                        \
   if (!bson_append_bool (_bsonBuildAppendArgs, (b))) {                    \
      bsonBuildError = "Error while appending bool(" _bsonDSL_str (b) ")"; \
   } else                                                                  \
      ((void) 0)
#define _bsonArrayOperation_boolean(X) _bsonArrayAppendValue (boolean (X))
#define _bsonValueOperation_boolean(b) _bsonValueOperation_bool (b)

#define _bsonValueOperation_null                       \
   if (!bson_append_null (_bsonBuildAppendArgs)) {     \
      bsonBuildError = "Error while appending a null"; \
   } else                                              \
      ((void) 0)
#define _bsonArrayOperation_null _bsonValueOperation (null)

#define _bsonArrayOperation_value(X) _bsonArrayAppendValue (value (X))

#define _bsonValueOperation_value(Value)                                        \
   _bsonDSL_begin ("value(%s)", _bsonDSL_str (Value));                          \
   if (!bson_append_value (_bsonBuildAppendArgs, &(Value))) {                   \
      bsonBuildError = "Error while appending value(" _bsonDSL_str (Value) ")"; \
   }                                                                            \
   _bsonDSL_end

/// Insert the given BSON document into the parent document in-place
#define _bsonDocOperation_insert(OtherBSON, Pred)                                                    \
   _bsonDSL_begin ("Insert other document: [%s]", _bsonDSL_str (OtherBSON));                         \
   const bool _bvHalt = false; /* Required for _bsonVisitEach() */                                   \
   _bsonVisitEach (OtherBSON, if (Pred, then (do (_bsonDocOperation_iterElement (bsonVisitIter))))); \
   _bsonDSL_end

#define _bsonDocOperation_insertFromIter(Iter, Pred)                               \
   _bsonDSL_begin ("Insert document from iterator: [%s]", _bsonDSL_str (Iter));    \
   bson_t _bbDocFromIter = _bson_dsl_iter_as_doc (&(Iter));                        \
   if (_bbDocFromIter.len == 0) {                                                  \
      _bsonDSLDebug ("NOTE: Skipping insert of non-document value from iterator"); \
   } else {                                                                        \
      _bsonDocOperation_insert (_bbDocFromIter, Pred);                             \
   }                                                                               \
   _bsonDSL_end

#define _bsonDocOperation_iterElement(Iter)                                                             \
   _bsonDSL_begin ("Insert element from bson_iter_t [%s]", _bsonDSL_str (Iter));                        \
   bson_iter_t _bbIter = (Iter);                                                                        \
   _bsonDocOperation_kvl (bson_iter_key (&_bbIter), bson_iter_key_len (&_bbIter), iterValue (_bbIter)); \
   _bsonDSL_end

/// Insert the given BSON document into the parent array. Keys of the given
/// document are discarded and it is treated as an array of values.
#define _bsonArrayOperation_insert(OtherArr, Pred)                                                  \
   _bsonDSL_begin ("Insert other array: [%s]", _bsonDSL_str (OtherArr));                            \
   _bsonVisitEach (OtherArr, if (Pred, then (do (_bsonArrayOperation_iterValue (bsonVisitIter))))); \
   _bsonDSL_end

#define _bsonArrayAppendValue(ValueOperation)                                                             \
   _bsonDSL_begin ("[%d] => [%s]", (int) bsonBuildContext.index, _bsonDSL_strElide (30, ValueOperation)); \
   /* Set the doc key to the array index as a string: */                                                  \
   _bsonBuild_setKeyToArrayIndex (bsonBuildContext.index);                                                \
   /* Append a value: */                                                                                  \
   _bsonValueOperation_##ValueOperation;                                                                  \
   /* Increment the array index: */                                                                       \
   ++_bbCtx.index;                                                                                        \
   _bsonDSL_end


#define _bsonDocOperationIfThen_then _bsonBuildAppendWithCurrentContext
#define _bsonDocOperationIfElse_else _bsonBuildAppendWithCurrentContext

#define _bsonDocOperationIfThenElse(Condition, Then, Else)              \
   if ((Condition)) {                                                   \
      _bsonDSLDebug ("Taking TRUE branch: [%s]", _bsonDSL_str (Then));  \
      _bsonDocOperationIfThen_##Then;                                   \
   } else {                                                             \
      _bsonDSLDebug ("Taking FALSE branch: [%s]", _bsonDSL_str (Else)); \
      _bsonDocOperationIfElse_##Else;                                   \
   }

#define _bsonDocOperationIfThen(Condition, Then)                       \
   if ((Condition)) {                                                  \
      _bsonDSLDebug ("Taking TRUE branch: [%s]", _bsonDSL_str (Then)); \
      _bsonDocOperationIfThen_##Then;                                  \
   }

#define _bsonDocOperation_if(Condition, ...)                                                                 \
   _bsonDSL_begin ("Conditional append on [%s]", _bsonDSL_str (Condition));                                  \
   /* Pick a sub-macro depending on if there are one or two args */                                          \
   _bsonDSL_ifElse (_bsonDSL_hasComma (__VA_ARGS__), _bsonDocOperationIfThenElse, _bsonDocOperationIfThen) ( \
      Condition, __VA_ARGS__);                                                                               \
   _bsonDSL_end

#define _bsonArrayOperationIfThen_then _bsonBuildArrayWithCurrentContext
#define _bsonArrayOperationIfElse_else _bsonBuildArrayWithCurrentContext

#define _bsonArrayOperationIfThenElse(Condition, Then, Else)            \
   if ((Condition)) {                                                   \
      _bsonDSLDebug ("Taking TRUE branch: [%s]", _bsonDSL_str (Then));  \
      _bsonArrayOperationIfThen_##Then;                                 \
   } else {                                                             \
      _bsonDSLDebug ("Taking FALSE branch: [%s]", _bsonDSL_str (Else)); \
      _bsonArrayOperationIfElse_##Else;                                 \
   }

#define _bsonArrayOperationIfThen(Condition, Then)                     \
   if ((Condition)) {                                                  \
      _bsonDSLDebug ("Taking TRUE branch: [%s]", _bsonDSL_str (Then)); \
      _bsonArrayOperationIfThen_##Then;                                \
   }

#define _bsonArrayOperation_if(Condition, ...)                                                                   \
   _bsonDSL_begin ("Conditional value on [%s]", _bsonDSL_str (Condition));                                       \
   /* Pick a sub-macro depending on if there are one or two args */                                              \
   _bsonDSL_ifElse (_bsonDSL_hasComma (__VA_ARGS__), _bsonArrayOperationIfThenElse, _bsonArrayOperationIfThen) ( \
      Condition, __VA_ARGS__);                                                                                   \
   _bsonDSL_end

#define _bsonValueOperationIf_then(X) _bsonValueOperation_##X
#define _bsonValueOperationIf_else(X) _bsonValueOperation_##X

#define _bsonValueOperation_if(Condition, Then, Else)                   \
   if ((Condition)) {                                                   \
      _bsonDSLDebug ("Taking TRUE branch: [%s]", _bsonDSL_str (Then));  \
      _bsonValueOperationIf_##Then;                                     \
   } else {                                                             \
      _bsonDSLDebug ("Taking FALSE branch: [%s]", _bsonDSL_str (Else)); \
      _bsonValueOperationIf_##Else;                                     \
   }

#define _bsonBuild_setKeyToArrayIndex(Idx)                                                                       \
   _bbCtx.key_len = bson_snprintf (_bbCtx.index_key_str, sizeof _bbCtx.index_key_str, "%d", (int) _bbCtx.index); \
   _bbCtx.key = _bbCtx.index_key_str

/// Handle an element of array()
#define _bsonArrayOperation(Element, _nil, _count) \
   if (!bsonBuildError) {                          \
      _bsonArrayOperation_##Element;               \
   }

#define _bsonBuildAppendWithCurrentContext(...) _bsonDSL_mapMacro (_bsonDocOperation, ~, __VA_ARGS__)

#define _bsonBuildArrayWithCurrentContext(...) _bsonDSL_mapMacro (_bsonArrayOperation, ~, __VA_ARGS__)

#define _bsonDSL_Type_double BSON_TYPE_DOUBLE
#define _bsonDSL_Type_utf8 BSON_TYPE_UTF8
#define _bsonDSL_Type_doc BSON_TYPE_DOCUMENT
#define _bsonDSL_Type_array BSON_TYPE_ARRAY
#define _bsonDSL_Type_binary BSON_TYPE_BINARY
#define _bsonDSL_Type_undefined BSON_TYPE_UNDEFINED
#define _bsonDSL_Type_oid BSON_TYPE_OID
// Use `boolean`, not `bool`. `bool` may be defined as a macro to `_Bool` or `int`:
#define _bsonDSL_Type_boolean BSON_TYPE_BOOL
#define _bsonDSL_Type_date_time BSON_TYPE_DATE_TIME
#define _bsonDSL_Type_null BSON_TYPE_NULL
#define _bsonDSL_Type_regex BSON_TYPE_REGEX
#define _bsonDSL_Type_dbpointer BSON_TYPE_DBPOINTER
#define _bsonDSL_Type_code BSON_TYPE_CODE
#define _bsonDSL_Type_codewscope BSON_TYPE_CODEWSCOPE
#define _bsonDSL_Type_int32 BSON_TYPE_INT32
#define _bsonDSL_Type_timestamp BSON_TYPE_TIMESTAMP
#define _bsonDSL_Type_int64 BSON_TYPE_INT64
#define _bsonDSL_Type_decimal128 BSON_TYPE_DECIMAL128

#define _bsonDSL_Type_string __NOTE__No_type_named__string__did_you_mean__utf8

#define _bsonVisitOperation_halt _bvHalt = true

#define _bsonVisitOperation_if(Predicate, ...)                                                                \
   _bsonDSL_begin ("if(%s)", _bsonDSL_str (Predicate));                                                       \
   _bsonDSL_ifElse (_bsonDSL_hasComma (__VA_ARGS__), _bsonVisit_ifThenElse, _bsonVisit_ifThen) (Predicate,    \
                                                                                                __VA_ARGS__); \
   _bsonDSL_end

#define _bsonVisit_ifThenElse(Predicate, Then, Else) \
   if (bsonPredicate (Predicate)) {                  \
      _bsonDSLDebug ("then:");                       \
      _bsonVisit_ifThen_##Then;                      \
   } else {                                          \
      _bsonDSLDebug ("else:");                       \
      _bsonVisit_ifElse_##Else;                      \
   }

#define _bsonVisit_ifThen(Predicate, Then) \
   if (bsonPredicate (Predicate)) {        \
      _bsonDSLDebug ("then:");             \
      _bsonVisit_ifThen_##Then;            \
   } else {                                \
      _bsonDSLDebug ("[else nothing]");    \
   }

#define _bsonVisit_ifThen_then _bsonVisit_applyOps
#define _bsonVisit_ifElse_else _bsonVisit_applyOps

#define _bsonVisitOperation_storeBool(Dest)               \
   _bsonDSL_begin ("storeBool(%s)", _bsonDSL_str (Dest)); \
   (Dest) = bson_iter_as_bool (&bsonVisitIter);           \
   _bsonDSL_end

#define _bsonVisitOperation_storeStrRef(Dest)               \
   _bsonDSL_begin ("storeStrRef(%s)", _bsonDSL_str (Dest)); \
   (Dest) = bson_iter_utf8 (&bsonVisitIter, NULL);          \
   _bsonDSL_end

#define _bsonVisitOperation_storeStrDup(Dest)               \
   _bsonDSL_begin ("storeStrDup(%s)", _bsonDSL_str (Dest)); \
   (Dest) = bson_iter_dup_utf8 (&bsonVisitIter, NULL);      \
   _bsonDSL_end

#define _bsonVisitOperation_storeDocDup(Dest)               \
   _bsonDSL_begin ("storeDocDup(%s)", _bsonDSL_str (Dest)); \
   bson_t _bvDoc = BSON_INITIALIZER;                        \
   _bson_dsl_iter_as_doc (&_bvDoc, &bsonVisitIter);         \
   if (_bvDoc.len) {                                        \
      bson_copy_to (&_bvDoc, &(Dest));                      \
   }                                                        \
   _bsonDSL_end

#define _bsonVisitOperation_storeDocRef(Dest)               \
   _bsonDSL_begin ("storeDocRef(%s)", _bsonDSL_str (Dest)); \
   _bson_dsl_iter_as_doc (&(Dest), &bsonVisitIter);         \
   _bsonDSL_end

#define _bsonVisitOperation_storeDocDupPtr(Dest)               \
   _bsonDSL_begin ("storeDocDupPtr(%s)", _bsonDSL_str (Dest)); \
   bson_t _bvDoc = BSON_INITIALIZER;                           \
   _bson_dsl_iter_as_doc (&_bvDoc, &bsonVisitIter);            \
   if (_bvDoc.len) {                                           \
      (Dest) = bson_copy (&_bvDoc);                            \
   }                                                           \
   _bsonDSL_end

#define _bsonVisitOperation_storeInt32(Dest)               \
   _bsonDSL_begin ("storeInt32(%s)", _bsonDSL_str (Dest)); \
   (Dest) = bson_iter_int32 (&bsonVisitIter);              \
   _bsonDSL_end

#define _bsonVisitOperation_do(...)                                \
   _bsonDSL_begin ("do: %s", _bsonDSL_strElide (30, __VA_ARGS__)); \
   do {                                                            \
      __VA_ARGS__;                                                 \
   } while (0);                                                    \
   _bsonDSL_end

#define _bsonVisitOperation_appendTo(BSON)                                                                      \
   _bsonDSL_begin ("appendTo(%s)", _bsonDSL_str (BSON));                                                        \
   if (!bson_append_iter (                                                                                      \
          &(BSON), bson_iter_key (&bsonVisitIter), (int) bson_iter_key_len (&bsonVisitIter), &bsonVisitIter)) { \
      bsonParseError = "Error in appendTo(" _bsonDSL_str (BSON) ")";                                            \
   }                                                                                                            \
   _bsonDSL_end

#define _bsonVisitCase_when(Pred, ...)                 \
   _bsonDSL_begin ("when: [%s]", _bsonDSL_str (Pred)); \
   _bvCaseMatched = _bsonPredicate (Pred);             \
   if (_bvCaseMatched) {                               \
      _bsonVisit_applyOps (__VA_ARGS__);               \
   }                                                   \
   _bsonDSL_end

#define _bsonVisitCase_else(...)      \
   _bsonDSL_begin ("else:%s", "");    \
   _bvCaseMatched = true;             \
   _bsonVisit_applyOps (__VA_ARGS__); \
   _bsonDSL_end

#define _bsonVisitCase(Pair, _nil, _count) \
   if (!_bvCaseMatched) {                  \
      _bsonVisitCase_##Pair;               \
   } else                                  \
      ((void) 0);

#define _bsonVisitOperation_case(...)                  \
   _bsonDSL_begin ("case:%s", "");                     \
   BSON_MAYBE_UNUSED bool _bvCaseMatched = false;      \
   _bsonDSL_mapMacro (_bsonVisitCase, ~, __VA_ARGS__); \
   _bsonDSL_end

#define _bsonVisitOperation_append _bsonVisitOneApplyDeferred_append _bsonDSL_nothing ()
#define _bsonVisitOneApplyDeferred_append(Doc, ...)                                                 \
   _bsonDSL_begin ("append to [%s] : %s", _bsonDSL_str (Doc), _bsonDSL_strElide (30, __VA_ARGS__)); \
   _bsonBuildAppend (Doc, __VA_ARGS__);                                                             \
   if (bsonBuildError) {                                                                            \
      bsonParseError = bsonBuildError;                                                              \
   }                                                                                                \
   _bsonDSL_end

#define _bsonVisitEach(Doc, ...)                                                          \
   _bsonDSL_begin ("visitEach(%s)", _bsonDSL_str (Doc));                                  \
   do {                                                                                   \
      /* Reset the context */                                                             \
      struct _bsonVisitContext_t _bvCtx = {                                               \
         .doc = &(Doc),                                                                   \
         .parent = _bsonVisitContextThreadLocalPtr,                                       \
         .index = 0,                                                                      \
      };                                                                                  \
      _bsonVisitContextThreadLocalPtr = &_bvCtx;                                          \
      bsonParseError = NULL;                                                              \
      /* Iterate over each element of the document */                                     \
      if (!bson_iter_init (&_bvCtx.iter, &(Doc))) {                                       \
         bsonParseError = "Invalid BSON data [a]";                                        \
      }                                                                                   \
      BSON_MAYBE_UNUSED bool _bvBreak = false;                                            \
      BSON_MAYBE_UNUSED bool _bvContinue = false;                                         \
      while (bson_iter_next (&_bvCtx.iter) && !_bvHalt && !bsonParseError && !_bvBreak) { \
         _bvContinue = false;                                                             \
         _bsonVisit_applyOps (__VA_ARGS__);                                               \
         ++_bvCtx.index;                                                                  \
      }                                                                                   \
      if (bsonVisitIter.err_off) {                                                        \
         bsonParseError = "Invalid BSON data [b]";                                        \
      }                                                                                   \
      /* Restore the dsl context */                                                       \
      _bsonVisitContextThreadLocalPtr = _bvCtx.parent;                                    \
   } while (0);                                                                           \
   _bsonDSL_end

#define _bsonVisitOperation_visitEach _bsonVisitOperation_visitEachDeferred _bsonDSL_nothing ()
#define _bsonVisitOperation_visitEachDeferred(...)                             \
   _bsonDSL_begin ("visitEach:%s", "");                                        \
   do {                                                                        \
      const uint8_t *data;                                                     \
      uint32_t len;                                                            \
      bson_type_t typ = bson_iter_type_unsafe (&bsonVisitIter);                \
      if (typ == BSON_TYPE_ARRAY)                                              \
         bson_iter_array (&bsonVisitIter, &len, &data);                        \
      else if (typ == BSON_TYPE_DOCUMENT)                                      \
         bson_iter_document (&bsonVisitIter, &len, &data);                     \
      else {                                                                   \
         _bsonDSLDebug ("(Skipping visitEach() of non-array/document value)"); \
         break;                                                                \
      }                                                                        \
      bson_t inner;                                                            \
      BSON_ASSERT (bson_init_static (&inner, data, len));                      \
      _bsonVisitEach (inner, __VA_ARGS__);                                     \
   } while (0);                                                                \
   _bsonDSL_end

#define _bsonVisitOperation_nop _bsonDSLDebug ("[nop]")
#define _bsonVisitOperation_parse(...)                                    \
   do {                                                                   \
      const uint8_t *data;                                                \
      uint32_t len;                                                       \
      bson_type_t typ = bson_iter_type (&bsonVisitIter);                  \
      if (typ == BSON_TYPE_ARRAY)                                         \
         bson_iter_array (&bsonVisitIter, &len, &data);                   \
      else if (typ == BSON_TYPE_DOCUMENT)                                 \
         bson_iter_document (&bsonVisitIter, &len, &data);                \
      else {                                                              \
         _bsonDSLDebug ("Ignoring parse() for non-document/array value"); \
         break;                                                           \
      }                                                                   \
      bson_t inner;                                                       \
      BSON_ASSERT (bson_init_static (&inner, data, len));                 \
      _bsonParse (inner, __VA_ARGS__);                                    \
   } while (0);

#define _bsonVisitOperation_continue _bvContinue = true
#define _bsonVisitOperation_break _bvBreak = _bvContinue = true
#define _bsonVisitOperation_require(Predicate)                                  \
   _bsonDSL_begin ("require(%s)", _bsonDSL_str (Predicate));                    \
   if (!bsonPredicate (Predicate)) {                                            \
      bsonParseError = "Element requirement failed: " _bsonDSL_str (Predicate); \
   }                                                                            \
   _bsonDSL_end

#define _bsonVisitOperation_error(S) bsonParseError = (S)
#define _bsonVisitOperation_errorf(S, ...) (bsonParseError = _bson_dsl_errorf (&(S), __VA_ARGS__))
#define _bsonVisitOperation_dupPath(S)               \
   _bsonDSL_begin ("dupPath(%s)", _bsonDSL_str (S)); \
   _bson_dsl_dupPath (&(S));                         \
   _bsonDSL_end

#define _bsonVisit_applyOp(P, _const, _count)            \
   do {                                                  \
      if (!_bvContinue && !_bvHalt && !bsonParseError) { \
         _bsonVisitOperation_##P;                        \
      }                                                  \
   } while (0);

#define _bsonParse(Doc, ...)                                                                        \
   do {                                                                                             \
      BSON_MAYBE_UNUSED const bson_t *_bpDoc = &(Doc);                                              \
      /* Keep track of which elements have been visited based on their index*/                      \
      uint64_t _bpVisitBits_static[4] = {0};                                                        \
      BSON_MAYBE_UNUSED uint64_t *_bpVisitBits = _bpVisitBits_static;                               \
      BSON_MAYBE_UNUSED size_t _bpNumVisitBitInts = sizeof _bpVisitBits_static / sizeof (uint64_t); \
      BSON_MAYBE_UNUSED bool _bpFoundElement = false;                                               \
      _bsonParse_applyOps (__VA_ARGS__);                                                            \
      /* We may have allocated for visit bits */                                                    \
      if (_bpVisitBits != _bpVisitBits_static) {                                                    \
         bson_free (_bpVisitBits);                                                                  \
      }                                                                                             \
   } while (0)

#define _bsonParse_applyOps(...) _bsonDSL_mapMacro (_bsonParse_applyOp, ~, __VA_ARGS__)

/// Parse one entry referrenced by the context iterator
#define _bsonParse_applyOp(P, _nil, Counter) \
   do {                                      \
      if (!_bvHalt && !bsonParseError) {     \
         _bsonParseOperation_##P;            \
      }                                      \
   } while (0);

#define _bsonParseMarkVisited(Index)                                                               \
   if (1) {                                                                                        \
      const size_t nth_int = Index / 64u;                                                          \
      const size_t nth_bit = Index % 64u;                                                          \
      while (nth_int >= _bpNumVisitBitInts) {                                                      \
         /* Say that five times, fast: */                                                          \
         size_t new_num_visit_bit_ints = _bpNumVisitBitInts * 2u;                                  \
         uint64_t *new_visit_bit_ints = bson_malloc0 (sizeof (uint64_t) * new_num_visit_bit_ints); \
         memcpy (new_visit_bit_ints, _bpVisitBits, sizeof (uint64_t) * _bpNumVisitBitInts);        \
         if (_bpVisitBits != _bpVisitBits_static) {                                                \
            bson_free (_bpVisitBits);                                                              \
         }                                                                                         \
         _bpVisitBits = new_visit_bit_ints;                                                        \
         _bpNumVisitBitInts = new_num_visit_bit_ints;                                              \
      }                                                                                            \
                                                                                                   \
      _bpVisitBits[nth_int] |= (UINT64_C (1) << nth_bit);                                          \
   } else                                                                                          \
      ((void) 0)

#define _bsonParseDidVisitNth(Index) _bsonParseDidVisitNth_1 (Index / 64u, Index % 64u)
#define _bsonParseDidVisitNth_1(NthInt, NthBit) \
   (NthInt < _bpNumVisitBitInts && (_bpVisitBits[NthInt] & (UINT64_C (1) << NthBit)))

#define _bsonParseOperation_find(Predicate, ...)                                                                    \
   _bsonDSL_begin ("find(%s)", _bsonDSL_str (Predicate));                                                           \
   _bpFoundElement = false;                                                                                         \
   _bsonVisitEach (                                                                                                 \
      *_bpDoc,                                                                                                      \
      if (Predicate,                                                                                                \
          then (do (_bsonParseMarkVisited (bsonVisitContext.index); _bpFoundElement = true), __VA_ARGS__, break))); \
   if (!_bpFoundElement && !bsonParseError) {                                                                       \
      _bsonDSLDebug ("[not found]");                                                                                \
   }                                                                                                                \
   _bsonDSL_end

#define _bsonParseOperation_require(Predicate, ...)                                                                 \
   _bsonDSL_begin ("require(%s)", _bsonDSL_str (Predicate));                                                        \
   _bpFoundElement = false;                                                                                         \
   _bsonVisitEach (                                                                                                 \
      *_bpDoc,                                                                                                      \
      if (Predicate,                                                                                                \
          then (do (_bsonParseMarkVisited (bsonVisitContext.index); _bpFoundElement = true), __VA_ARGS__, break))); \
   if (!_bpFoundElement && !bsonParseError) {                                                                       \
      bsonParseError = "Failed to find a required element: " _bsonDSL_str (Predicate);                              \
   }                                                                                                                \
   _bsonDSL_end

#define _bsonParseOperation_visitOthers(...)                                                                      \
   _bsonDSL_begin ("visitOthers(%s)", _bsonDSL_strElide (30, __VA_ARGS__));                                       \
   _bsonVisitEach (*_bpDoc, if (not(eval (_bsonParseDidVisitNth (bsonVisitContext.index))), then (__VA_ARGS__))); \
   _bsonDSL_end

#define bsonPredicate(P) _bsonPredicate _bsonDSL_nothing () (P)
#define _bsonPredicate(P) _bsonPredicate_Condition_##P

#define _bsonPredicate_Condition_ __NOTE__Missing_name_for_a_predicate_expression

#define _bsonPredicate_Condition_allOf(...) (1 _bsonDSL_mapMacro (_bsonPredicateAnd, ~, __VA_ARGS__))
#define _bsonPredicate_Condition_anyOf(...) (0 _bsonDSL_mapMacro (_bsonPredicateOr, ~, __VA_ARGS__))
#define _bsonPredicate_Condition_not(...) (!(0 _bsonDSL_mapMacro (_bsonPredicateOr, ~, __VA_ARGS__)))
#define _bsonPredicateAnd(Pred, _ignore, _ignore1) &&_bsonPredicate _bsonDSL_nothing () (Pred)
#define _bsonPredicateOr(Pred, _ignore, _ignore2) || _bsonPredicate _bsonDSL_nothing () (Pred)

#define _bsonPredicate_Condition_eval(X) (X)

#define _bsonPredicate_Condition_key(...)                       \
   (_bson_dsl_key_is_anyof (bson_iter_key (&bsonVisitIter),     \
                            bson_iter_key_len (&bsonVisitIter), \
                            true /* case senstive */,           \
                            __VA_ARGS__,                        \
                            NULL))

#define _bsonPredicate_Condition_iKey(...)                      \
   (_bson_dsl_key_is_anyof (bson_iter_key (&bsonVisitIter),     \
                            bson_iter_key_len (&bsonVisitIter), \
                            false /* case insenstive */,        \
                            __VA_ARGS__,                        \
                            NULL))

#define _bsonPredicate_Condition_type(Type) (bson_iter_type (&bsonVisitIter) == _bsonDSL_Type_##Type)

#define _bsonPredicate_Condition_keyWithType(Key, Type) \
   (_bsonPredicate_Condition_allOf _bsonDSL_nothing () (key (Key), type (Type)))

#define _bsonPredicate_Condition_iKeyWithType(Key, Type) \
   (_bsonPredicate_Condition_allOf _bsonDSL_nothing () (iKey (Key), type (Type)))

#define _bsonPredicate_Condition_lastElement (_bson_dsl_iter_is_last_element (&bsonVisitIter))

#define _bsonPredicate_Condition_isNumeric BSON_ITER_HOLDS_NUMBER (&bsonVisitIter)

#define _bsonPredicate_Condition_1 1
#define _bsonPredicate_Condition_0 0
#define _bsonPredicate_Condition_true true
#define _bsonPredicate_Condition_false false

#define _bsonPredicate_Condition_isTrue (bson_iter_as_bool (&bsonVisitIter))
#define _bsonPredicate_Condition_isFalse (!bson_iter_as_bool (&bsonVisitIter))
#define _bsonPredicate_Condition_empty (_bson_dsl_is_empty_bson (&bsonVisitIter))

#define _bsonPredicate_Condition_strEqual(S) (_bson_dsl_test_strequal (S, true))
#define _bsonPredicate_Condition_iStrEqual(S) (_bson_dsl_test_strequal (S, false))

#define _bsonPredicate_Condition_eq(Type, Value) (_bsonPredicate_Condition_type (Type) && bsonAs (Type) == Value)

#define _bsonParseOperation_else _bsonParse_deferredElse _bsonDSL_nothing ()
#define _bsonParse_deferredElse(...)     \
   if (!_bpFoundElement) {               \
      _bsonDSL_begin ("else:%s", "");    \
      _bsonParse_applyOps (__VA_ARGS__); \
      _bsonDSL_end;                      \
   } else                                \
      ((void) 0)

#define _bsonParseOperation_do(...)                                \
   _bsonDSL_begin ("do: %s", _bsonDSL_strElide (30, __VA_ARGS__)); \
   do {                                                            \
      __VA_ARGS__;                                                 \
   } while (0);                                                    \
   _bsonDSL_end

#define _bsonParseOperation_halt _bvHalt = true

#define _bsonParseOperation_error(S) bsonParseError = (S)
#define _bsonParseOperation_errorf(S, ...) (bsonParseError = _bson_dsl_errorf (&(S), __VA_ARGS__))

/// Perform conditional parsing
#define _bsonParseOperation_if(Condition, ...)                                                                \
   _bsonDSL_begin ("if(%s)", _bsonDSL_str (Condition));                                                       \
   /* Pick a sub-macro depending on if there are one or two args */                                           \
   _bsonDSL_ifElse (_bsonDSL_hasComma (__VA_ARGS__), _bsonParse_ifThenElse, _bsonParse_ifThen) (Condition,    \
                                                                                                __VA_ARGS__); \
   _bsonDSL_end

#define _bsonParse_ifThen_then _bsonParse_applyOps
#define _bsonParse_ifElse_else _bsonParse_applyOps

#define _bsonParse_ifThenElse(Condition, Then, Else) \
   if ((Condition)) {                                \
      _bsonDSLDebug ("then:");                       \
      _bsonParse_ifThen_##Then;                      \
   } else {                                          \
      _bsonDSLDebug ("else:");                       \
      _bsonParse_ifElse_##Else;                      \
   }

#define _bsonParse_ifThen(Condition, Then)       \
   if ((Condition)) {                            \
      _bsonDSLDebug ("%s", _bsonDSL_str (Then)); \
      _bsonParse_ifThen_##Then;                  \
   } else {                                      \
      _bsonDSLDebug ("[else nothing]");          \
   }

#define _bsonParseOperation_append _bsonParseOperationDeferred_append _bsonDSL_nothing ()
#define _bsonParseOperationDeferred_append(Doc, ...)                                                \
   _bsonDSL_begin ("append to [%s] : %s", _bsonDSL_str (Doc), _bsonDSL_strElide (30, __VA_ARGS__)); \
   _bsonBuildAppend (Doc, __VA_ARGS__);                                                             \
   if (bsonBuildError) {                                                                            \
      bsonParseError = bsonBuildError;                                                              \
   }                                                                                                \
   _bsonDSL_end

#define _bsonVisit_applyOps _bsonVisit_applyOpsDeferred _bsonDSL_nothing ()
#define _bsonVisit_applyOpsDeferred(...)                      \
   do {                                                       \
      _bsonDSL_mapMacro (_bsonVisit_applyOp, ~, __VA_ARGS__); \
   } while (0);

#define bsonBuildArray(BSON, ...)                                                                       \
   _bsonDSL_begin ("bsonBuildArray(%s, %s)", _bsonDSL_str (BSON), _bsonDSL_strElide (30, __VA_ARGS__)); \
   _bsonDSL_eval (_bsonBuildArray (BSON, __VA_ARGS__));                                                 \
   _bsonDSL_end

#define _bsonBuildArray(BSON, ...)                     \
   do {                                                \
      _bsonDSL_disableWarnings ();                     \
      struct _bsonBuildContext_t _bbCtx = {            \
         .doc = &(BSON),                               \
         .parent = _bsonBuildContextThreadLocalPtr,    \
         .index = 0,                                   \
      };                                               \
      _bsonBuildContextThreadLocalPtr = &_bbCtx;       \
      _bsonBuildArrayWithCurrentContext (__VA_ARGS__); \
      _bsonBuildContextThreadLocalPtr = _bbCtx.parent; \
      _bsonDSL_restoreWarnings ();                     \
   } while (0)

/**
 * @brief Build a BSON document by appending to an existing bson_t document
 *
 * @param Pointer The document upon which to append
 * @param ... The Document elements to append to the document
 */
#define bsonBuildAppend(BSON, ...) _bsonDSL_eval (_bsonBuildAppend (BSON, __VA_ARGS__))
#define _bsonBuildAppend(BSON, ...)                                    \
   _bsonDSL_begin ("Appending to document '%s'", _bsonDSL_str (BSON)); \
   _bsonDSL_disableWarnings ();                                        \
   /* Save the dsl context */                                          \
   struct _bsonBuildContext_t _bbCtx = {                               \
      .doc = &(BSON),                                                  \
      .parent = _bsonBuildContextThreadLocalPtr,                       \
   };                                                                  \
   /* Reset the context */                                             \
   _bsonBuildContextThreadLocalPtr = &_bbCtx;                          \
   bsonBuildError = NULL;                                              \
   _bsonBuildAppendWithCurrentContext (__VA_ARGS__);                   \
   /* Restore the dsl context */                                       \
   _bsonBuildContextThreadLocalPtr = _bbCtx.parent;                    \
   _bsonDSL_restoreWarnings ();                                        \
   _bsonDSL_end

/**
 * @brief Build a new BSON document and assign the value into the given
 * pointer.
 */
#define bsonBuild(BSON, ...)                                              \
   _bsonDSL_begin ("Build a new document for '%s'", _bsonDSL_str (BSON)); \
   bson_t *_bbDest = &(BSON);                                             \
   bson_init (_bbDest);                                                   \
   bsonBuildAppend (*_bbDest, __VA_ARGS__);                               \
   _bsonDSL_end

/**
 * @brief Declare a variable and build it with the BSON DSL @see bsonBuild
 */
#define bsonBuildDecl(Variable, ...)   \
   bson_t Variable = BSON_INITIALIZER; \
   bsonBuild (Variable, __VA_ARGS__)


struct _bsonBuildContext_t {
   /// The document that is being built
   bson_t *doc;
   /// The key that is pending an append
   const char *key;
   /// The length of the string given in 'key'
   int key_len;
   /// The index of the array being built (if applicable)
   size_t index;
   /// A buffer for formatting key strings
   char index_key_str[16];
   /// The parent context (if building a sub-document)
   struct _bsonBuildContext_t *parent;
};

/// A pointer to the current thread's bsonBuild context
_bson_thread_local _bson_comdat struct _bsonBuildContext_t *_bsonBuildContextThreadLocalPtr = NULL;

struct _bsonVisitContext_t {
   const bson_t *doc;
   bson_iter_t iter;
   const struct _bsonVisitContext_t *parent;
   size_t index;
};

/// A pointer to the current thread's bsonVisit/bsonParse context
_bson_thread_local _bson_comdat struct _bsonVisitContext_t const *_bsonVisitContextThreadLocalPtr = NULL;

/**
 * @brief The most recent error from a bsonBuild() DSL command.
 *
 * If NULL, no error occurred. Users can assign a value to this string to
 * indicate failure.
 */
_bson_thread_local _bson_comdat const char *bsonBuildError = NULL;

/**
 * @brief The most recent error from a buildVisit() or bsonParse() DSL command.
 *
 * If NULL, no error occurred. Users can assign a value to this string to
 * indicate an error.
 *
 * If this string becomes non-NULL, the current bsonVisit()/bsonParse() will
 * halt and return.
 *
 * Upon entering a new bsonVisit()/bsonParse(), this will be reset to NULL.
 */
_bson_thread_local _bson_comdat const char *bsonParseError = NULL;

#define _bsonDSLDebug(...) _bson_dsl_debug (BSON_DSL_DEBUG, __FILE__, __LINE__, BSON_FUNC, __VA_ARGS__)


static BSON_INLINE bool
_bson_dsl_test_strequal (const char *string, bool case_sensitive)
{
   bson_iter_t it = bsonVisitIter;
   if (bson_iter_type (&it) == BSON_TYPE_UTF8) {
      uint32_t len;
      const char *s = bson_iter_utf8 (&it, &len);
      if (len != (uint32_t) strlen (string)) {
         return false;
      }
      if (case_sensitive) {
         return memcmp (string, s, len) == 0;
      } else {
         return bson_strcasecmp (string, s) == 0;
      }
   }
   return false;
}

static BSON_INLINE bool
_bson_dsl_key_is_anyof (const char *key, const size_t keylen, int case_sensitive, ...)
{
   va_list va;
   va_start (va, case_sensitive);
   const char *str;
   while ((str = va_arg (va, const char *))) {
      size_t str_len = strlen (str);
      if (str_len != keylen) {
         continue;
      }
      if (case_sensitive) {
         if (memcmp (str, key, str_len) == 0) {
            va_end (va);
            return true;
         }
      } else {
         if (bson_strcasecmp (str, key) == 0) {
            va_end (va);
            return true;
         }
      }
   }
   va_end (va);
   return false;
}

static BSON_INLINE void
_bson_dsl_iter_as_doc (bson_t *into, const bson_iter_t *it)
{
   uint32_t len = 0;
   const uint8_t *dataptr = NULL;
   if (BSON_ITER_HOLDS_ARRAY (it)) {
      bson_iter_array (it, &len, &dataptr);
   } else if (BSON_ITER_HOLDS_DOCUMENT (it)) {
      bson_iter_document (it, &len, &dataptr);
   }
   if (dataptr) {
      BSON_ASSERT (bson_init_static (into, dataptr, len));
   }
}

static BSON_INLINE bool
_bson_dsl_is_empty_bson (const bson_iter_t *it)
{
   bson_t d = BSON_INITIALIZER;
   _bson_dsl_iter_as_doc (&d, it);
   return d.len == 5; // Empty documents/arrays have byte-size of five
}

static BSON_INLINE bool
_bson_dsl_iter_is_last_element (const bson_iter_t *it)
{
   bson_iter_t dup = *it;
   return !bson_iter_next (&dup) && dup.err_off == 0;
}

_bson_thread_local _bson_comdat int _bson_dsl_indent = 0;

static BSON_INLINE void BSON_GNUC_PRINTF (5, 6)
   _bson_dsl_debug (bool do_debug, const char *file, int line, const char *func, const char *string, ...)
{
   if (do_debug) {
      fprintf (stderr, "%s:%d: [%s] bson_dsl: ", file, line, func);
      for (int i = 0; i < _bson_dsl_indent; ++i) {
         fputs ("  ", stderr);
      }
      va_list va;
      va_start (va, string);
      vfprintf (stderr, string, va);
      va_end (va);
      fputc ('\n', stderr);
      fflush (stderr);
   }
}

static BSON_INLINE char *BSON_GNUC_PRINTF (2, 3) _bson_dsl_errorf (char **const into, const char *const fmt, ...)
{
   if (*into) {
      bson_free (*into);
      *into = NULL;
   }
   va_list args;
   va_start (args, fmt);
   *into = bson_strdupv_printf (fmt, args);
   va_end (args);
   return *into;
}

static BSON_INLINE void
_bson_dsl_dupPath (char **into)
{
   if (*into) {
      bson_free (*into);
      *into = NULL;
   }
   char *acc = bson_strdup ("");
   for (const struct _bsonVisitContext_t *ctx = &bsonVisitContext; ctx; ctx = ctx->parent) {
      char *prev = acc;
      if (ctx->parent && BSON_ITER_HOLDS_ARRAY (&ctx->parent->iter)) {
         // We're an array element
         acc = bson_strdup_printf ("[%d]%s", (int) ctx->index, prev);
      } else {
         // We're a document element
         acc = bson_strdup_printf (".%s%s", bson_iter_key (&ctx->iter), prev);
      }
      bson_free (prev);
   }
   *into = bson_strdup_printf ("$%s", acc);
   bson_free (acc);
}

static BSON_INLINE const char *
_bsonVisitIterAs_cstr (void)
{
   return bson_iter_utf8 (&bsonVisitIter, NULL);
}

static BSON_INLINE int32_t
_bsonVisitIterAs_int32 (void)
{
   return bson_iter_int32 (&bsonVisitIter);
}

static BSON_INLINE bool
_bsonVisitIterAs_boolean (void)
{
   return bson_iter_as_bool (&bsonVisitIter);
}

#define bsonAs(Type) _bsonDSL_paste (_bsonVisitIterAs_, Type) ()

/// Convert the given argument into a string without inhibitting macro expansion
#define _bsonDSL_str(...) _bsonDSL_str_1 (__VA_ARGS__)
// Empty quotes "" are to ensure a string appears. Old MSVC has a bug
// where empty #__VA_ARGS__ just vanishes.
#define _bsonDSL_str_1(...) "" #__VA_ARGS__

#define _bsonDSL_strElide(MaxLen, ...) \
   (strlen (_bsonDSL_str (__VA_ARGS__)) > (MaxLen) ? "[...]" : _bsonDSL_str (__VA_ARGS__))

/// Paste two tokens:
#define _bsonDSL_paste(a, ...) _bsonDSL_paste_impl (a, __VA_ARGS__)
#define _bsonDSL_paste_impl(a, ...) a##__VA_ARGS__

/// Paste three tokens:
#define _bsonDSL_paste3(a, b, c) _bsonDSL_paste (a, _bsonDSL_paste (b, c))
/// Paste four tokens:
#define _bsonDSL_paste4(a, b, c, d) _bsonDSL_paste (a, _bsonDSL_paste3 (b, c, d))

// clang-format off

/// Now we need a MAP() macro. This idiom is common, but fairly opaque. Below is
/// some crazy preprocessor trickery to implement it. Fortunately, once we have
/// MAP(), the remainder of this file is straightforward. This implementation
/// isn't the simplest one possible, but is one that supports the old
/// non-compliant MSVC preprocessor.

/* Expands to nothing. Used to defer a function-like macro and to ignore arguments */
#define _bsonDSL_nothing(...)

/// Expand to the 64th argument. See below for why this is useful.
#define _bsonDSL_pick64th(\
                    _0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
                    _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
                    _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
                    _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
                    _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
                    _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
                    _61, _62, _63, ...) \
    _63

/**
 * @brief Expands to 1 if the given arguments contain any top-level commas, zero otherwise.
 *
 * There is an expansion of __VA_ARGS__, followed by 62 '1' arguments, followed
 * by single '0'. If __VA_ARGS__ contains no commas, pick64th() will return the
 * single zero. If __VA_ARGS__ contains any top-level commas, the series of ones
 * will shift to the right and pick64th will return one of those ones. (This only
 * works __VA_ARGS__ contains fewer than 62 commas, which is a somewhat reasonable
 * limit.) The _bsonDSL_nothing() is a workaround for MSVC's bad preprocessor that
 * expands __VA_ARGS__ incorrectly.
 *
 * If we have __VA_OPT__, this can be a lot simpler.
 */
#define _bsonDSL_hasComma(...) \
    _bsonDSL_pick64th \
    _bsonDSL_nothing() (__VA_ARGS__, \
                        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
                        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
                        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
                        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, ~)

/**
 * Expands to a single comma if "invoked" as a function-like macro.
 * (This will make sense, I promise.)
 */
#define _bsonDSL_commaIfRHSHasParens(...) ,

/**
 * @brief Expand to 1 if given no arguments, otherwise 0.
 *
 * This could be done much more simply using __VA_OPT__, but we need to work on
 * older compilers.
 */
#define _bsonDSL_isEmpty(...) \
    _bsonDSL_isEmpty_1(\
        /* Expands to '1' if __VA_ARGS__ contains any top-level commas */ \
        _bsonDSL_hasComma(__VA_ARGS__), \
        /* Expands to '1' if __VA_ARGS__ begins with a parenthesis, because \
         * that will cause an "invocation" of _bsonDSL_commaIfRHSHasParens, \
         * which immediately expands to a single comma. */ \
        _bsonDSL_hasComma(_bsonDSL_commaIfRHSHasParens __VA_ARGS__), \
        /* Expands to '1' if __VA_ARGS__ expands to a function-like macro name \
         * that then expands to anything containing a top-level comma */ \
        _bsonDSL_hasComma(__VA_ARGS__ ()), \
        /* Expands to '1' if __VA_ARGS__ expands to nothing. */ \
        _bsonDSL_hasComma(_bsonDSL_commaIfRHSHasParens __VA_ARGS__ ()))

/**
 * A helper for isEmpty(): If given (0, 0, 0, 1), expands as:
 *    - first: _bsonDSL_hasComma(_bsonDSL_isEmpty_CASE_0001)
 *    -  then: _bsonDSL_hasComma(,)
 *    -  then: 1
 * Given any other aruments:
 *    - first: _bsonDSL_hasComma(_bsonDSL_isEmpty_CASE_<somethingelse>)
 *    -  then: 0
 */
#define _bsonDSL_isEmpty_1(_1, _2, _3, _4) \
    _bsonDSL_hasComma(_bsonDSL_paste(_bsonDSL_isEmpty_CASE_, _bsonDSL_paste4(_1, _2, _3, _4)))
#define _bsonDSL_isEmpty_CASE_0001 ,

/**
 * @brief Expand to the first argument if `Cond` is 1, the second argument if `Cond` is 0
 */
#define _bsonDSL_ifElse(Cond, IfTrue, IfFalse) \
    /* Suppress expansion of the two branches by using the '#' operator */ \
    _bsonDSL_nothing(#IfTrue, #IfFalse)  \
    /* Concat the cond 1/0 with a prefix macro: */ \
    _bsonDSL_paste(_bsonDSL_ifElse_PICK_, Cond)(IfTrue, IfFalse)

#define _bsonDSL_ifElse_PICK_1(IfTrue, IfFalse) \
   /* Expand the first operand, throw away the second */ \
   IfTrue _bsonDSL_nothing(#IfFalse)
#define _bsonDSL_ifElse_PICK_0(IfTrue, IfFalse) \
   /* Expand to the second operand, throw away the first */ \
   IfFalse _bsonDSL_nothing(#IfTrue)

#ifdef _MSC_VER
// MSVC's "traditional" preprocessor requires many more expansion passes,
// but GNU and Clang are very slow when evaluating hugely nested expansions
// and generate massive macro expansion backtraces.
#define _bsonDSL_eval_1(...) __VA_ARGS__
#define _bsonDSL_eval_2(...) _bsonDSL_eval_1(_bsonDSL_eval_1(_bsonDSL_eval_1(_bsonDSL_eval_1(_bsonDSL_eval_1(__VA_ARGS__)))))
#define _bsonDSL_eval_4(...) _bsonDSL_eval_2(_bsonDSL_eval_2(_bsonDSL_eval_2(_bsonDSL_eval_2(_bsonDSL_eval_2(__VA_ARGS__)))))
#define _bsonDSL_eval_8(...) _bsonDSL_eval_4(_bsonDSL_eval_4(_bsonDSL_eval_4(_bsonDSL_eval_4(_bsonDSL_eval_4(__VA_ARGS__)))))
#define _bsonDSL_eval_16(...) _bsonDSL_eval_8(_bsonDSL_eval_8(_bsonDSL_eval_8(_bsonDSL_eval_8(_bsonDSL_eval_8(__VA_ARGS__)))))
#define _bsonDSL_eval(...) _bsonDSL_eval_16(_bsonDSL_eval_16(_bsonDSL_eval_16(_bsonDSL_eval_16(_bsonDSL_eval_16(__VA_ARGS__)))))
#else
// Each level of "eval" applies double the expansions of the previous level.
#define _bsonDSL_eval_1(...) __VA_ARGS__
#define _bsonDSL_eval_2(...) _bsonDSL_eval_1(_bsonDSL_eval_1(__VA_ARGS__))
#define _bsonDSL_eval_4(...) _bsonDSL_eval_2(_bsonDSL_eval_2(__VA_ARGS__))
#define _bsonDSL_eval_8(...) _bsonDSL_eval_4(_bsonDSL_eval_4(__VA_ARGS__))
#define _bsonDSL_eval_16(...) _bsonDSL_eval_8(_bsonDSL_eval_8(__VA_ARGS__))
#define _bsonDSL_eval_32(...) _bsonDSL_eval_16(_bsonDSL_eval_16(__VA_ARGS__))
#define _bsonDSL_eval(...) _bsonDSL_eval_32(__VA_ARGS__)
#endif

/**
 * Finally, the Map() macro that allows us to do the magic, which we've been
 * building up to all along.
 *
 * The dance with mapMacro_first, mapMacro_final, and _bsonDSL_nothing
 * conditional on argument count is to prevent warnings from pre-C99 about
 * passing no arguments to the '...' parameters. Yet again, if we had C99 and
 * __VA_OPT__ this would be simpler.
 */
#define _bsonDSL_mapMacro(Action, Constant, ...) \
   /* Pick our first action based on the content of '...': */ \
    _bsonDSL_ifElse( \
      /* If given no arguments: */\
      _bsonDSL_isEmpty(__VA_ARGS__), \
         /* expand to _bsonDSL_nothing */ \
         _bsonDSL_nothing, \
         /* Otherwise, expand to mapMacro_first: */ \
         _bsonDSL_mapMacro_first) \
   /* Now "invoke" the chosen macro: */ \
   _bsonDSL_nothing() (Action, Constant, __VA_ARGS__)

#define _bsonDSL_mapMacro_first(Action, Constant, ...) \
   /* Select our next step based on whether we have one or more arguments: */ \
   _bsonDSL_ifElse( \
      /* If '...' contains more than one argument (has a top-level comma): */ \
      _bsonDSL_hasComma(__VA_ARGS__), \
         /* Begin the mapMacro loop with mapMacro_A: */ \
         _bsonDSL_mapMacro_A, \
         /* Otherwise skip to the final step of the loop: */ \
         _bsonDSL_mapMacro_final) \
   /* Invoke the chosen macro, setting the counter to zero: */ \
   _bsonDSL_nothing() (Action, Constant, 0, __VA_ARGS__)

/// Handle the last expansion in a mapMacro sequence.
#define _bsonDSL_mapMacro_final(Action, Constant, Counter, FinalElement) \
    Action(FinalElement, Constant, Counter)

/**
 * mapMacro_A and mapMacro_B are identical and just invoke each other.
 */
#define _bsonDSL_mapMacro_A(Action, Constant, Counter, Head, ...) \
   /* First evaluate the action once: */ \
   Action(Head, Constant, Counter) \
   /* Pick our next step: */ \
   _bsonDSL_ifElse( \
      /* If '...' contains more than one argument (has a top-level comma): */ \
      _bsonDSL_hasComma(__VA_ARGS__), \
         /* Jump to the other mapMacro: */ \
         _bsonDSL_mapMacro_B, \
         /* Otherwise go to mapMacro_final */ \
         _bsonDSL_mapMacro_final) \
   /* Invoke the next step of the map: */ \
   _bsonDSL_nothing() (Action, Constant, Counter + 1, __VA_ARGS__)

#define _bsonDSL_mapMacro_B(Action, Constant, Counter, Head, ...) \
    Action(Head, Constant, Counter) \
    _bsonDSL_ifElse(_bsonDSL_hasComma(__VA_ARGS__), _bsonDSL_mapMacro_A, _bsonDSL_mapMacro_final) \
    _bsonDSL_nothing() (Action, Constant, Counter + 1, __VA_ARGS__)

// clang-format on


#endif // BSON_BSON_DSL_H_INCLUDED
