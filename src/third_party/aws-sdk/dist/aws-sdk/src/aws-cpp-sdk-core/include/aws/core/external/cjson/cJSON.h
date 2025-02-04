/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef cJSON_AS4CPP__h
#define cJSON_AS4CPP__h

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

/* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called from a project with a different default calling convention.  For windows you have 3 define options:

CJSON_AS4CPP_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
CJSON_AS4CPP_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
CJSON_AS4CPP_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

For *nix builds that support visibility attribute, you can define similar behavior by

setting default visibility to hidden by adding
-fvisibility=hidden (for gcc)
or
-xldscope=hidden (for sun cc)
to CFLAGS

then using the CJSON_AS4CPP_API_VISIBILITY flag to "export" the same symbols the way CJSON_AS4CPP_EXPORT_SYMBOLS does

*/

#define CJSON_AS4CPP_CDECL __cdecl
#define CJSON_AS4CPP_STDCALL __stdcall

/* Decide calling convention based on the cmake parameters defined in C++ SDK. */
#ifdef USE_IMPORT_EXPORT
#ifdef AWS_CORE_EXPORTS
#define CJSON_AS4CPP_EXPORT_SYMBOLS
#else
#define CJSON_AS4CPP_IMPORT_SYMBOLS
#endif // AWS_CORE_EXPORTS
#else
#define CJSON_AS4CPP_HIDE_SYMBOLS
#endif // USE_IMPORT_EXPORT

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(CJSON_AS4CPP_HIDE_SYMBOLS) && !defined(CJSON_AS4CPP_IMPORT_SYMBOLS) && !defined(CJSON_AS4CPP_EXPORT_SYMBOLS)
#define CJSON_AS4CPP_EXPORT_SYMBOLS
#endif

#if defined(CJSON_AS4CPP_HIDE_SYMBOLS)
#define CJSON_AS4CPP_PUBLIC(type)   type CJSON_AS4CPP_STDCALL
#elif defined(CJSON_AS4CPP_EXPORT_SYMBOLS)
#define CJSON_AS4CPP_PUBLIC(type)   __declspec(dllexport) type CJSON_AS4CPP_STDCALL
#elif defined(CJSON_AS4CPP_IMPORT_SYMBOLS)
#define CJSON_AS4CPP_PUBLIC(type)   __declspec(dllimport) type CJSON_AS4CPP_STDCALL
#endif
#else /* !__WINDOWS__ */
#define CJSON_AS4CPP_CDECL
#define CJSON_AS4CPP_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(CJSON_AS4CPP_API_VISIBILITY)
#define CJSON_AS4CPP_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define CJSON_AS4CPP_PUBLIC(type) type
#endif
#endif

/* project version */
#define CJSON_AS4CPP_VERSION_MAJOR 1
#define CJSON_AS4CPP_VERSION_MINOR 7
#define CJSON_AS4CPP_VERSION_PATCH 14

#include <stddef.h>

/* cJSON Types: */
#define cJSON_AS4CPP_Invalid (0)
#define cJSON_AS4CPP_False  (1 << 0)
#define cJSON_AS4CPP_True   (1 << 1)
#define cJSON_AS4CPP_NULL   (1 << 2)
#define cJSON_AS4CPP_Number (1 << 3)
#define cJSON_AS4CPP_String (1 << 4)
#define cJSON_AS4CPP_Array  (1 << 5)
#define cJSON_AS4CPP_Object (1 << 6)
#define cJSON_AS4CPP_Raw    (1 << 7) /* raw json */

#define cJSON_AS4CPP_IsReference 256
#define cJSON_AS4CPP_StringIsConst 512

/* The cJSON structure: */
typedef struct cJSON
{
    /* next/prev allow you to walk array/object chains. Alternatively, use GetArraySize/GetArrayItem/GetObjectItem */
    struct cJSON *next;
    struct cJSON *prev;
    /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */
    struct cJSON *child;

    /* The type of the item, as above. */
    int type;

    /* The item's string, if type==cJSON_AS4CPP_String  and type == cJSON_AS4CPP_Raw */
    char *valuestring;
    /* writing to valueint is DEPRECATED, use cJSON_AS4CPP_SetNumberValue instead */
    int valueint;
    /* The item's number, if type==cJSON_AS4CPP_Number */
    double valuedouble;

    /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
    char *string;
} cJSON;

typedef struct cJSON_AS4CPP_Hooks
{
      /* malloc/free are CDECL on Windows regardless of the default calling convention of the compiler, so ensure the hooks allow passing those functions directly. */
      void *(CJSON_AS4CPP_CDECL *malloc_fn)(size_t sz);
      void (CJSON_AS4CPP_CDECL *free_fn)(void *ptr);
} cJSON_AS4CPP_Hooks;

typedef int cJSON_AS4CPP_bool;

/* Limits how deeply nested arrays/objects can be before cJSON rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_AS4CPP_NESTING_LIMIT
#define CJSON_AS4CPP_NESTING_LIMIT 1000
#endif

/* returns the version of cJSON as a string */
CJSON_AS4CPP_PUBLIC(const char*) cJSON_AS4CPP_Version(void);

/* Supply malloc, realloc and free functions to cJSON */
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_InitHooks(cJSON_AS4CPP_Hooks* hooks);

/* Memory Management: the caller is always responsible to free the results from all variants of cJSON_AS4CPP_Parse (with cJSON_AS4CPP_Delete) and cJSON_AS4CPP_Print (with stdlib free, cJSON_AS4CPP_Hooks.free_fn, or cJSON_AS4CPP_free as appropriate). The exception is cJSON_AS4CPP_PrintPreallocated, where the caller has full responsibility of the buffer. */
/* Supply a block of JSON, and this returns a cJSON object you can interrogate. */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_Parse(const char *value);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_ParseWithLength(const char *value, size_t buffer_length);
/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to the final byte parsed. */
/* If you supply a ptr in return_parse_end and parsing fails, then return_parse_end will contain a pointer to the error so will match cJSON_AS4CPP_GetErrorPtr(). */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_AS4CPP_bool require_null_terminated);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_AS4CPP_bool require_null_terminated);

/* Render a cJSON entity to text for transfer/storage. */
CJSON_AS4CPP_PUBLIC(char *) cJSON_AS4CPP_Print(const cJSON *item);
/* Render a cJSON entity to text for transfer/storage without any formatting. */
CJSON_AS4CPP_PUBLIC(char *) cJSON_AS4CPP_PrintUnformatted(const cJSON *item);
/* Render a cJSON entity to text using a buffered strategy. prebuffer is a guess at the final size. guessing well reduces reallocation. fmt=0 gives unformatted, =1 gives formatted */
CJSON_AS4CPP_PUBLIC(char *) cJSON_AS4CPP_PrintBuffered(const cJSON *item, int prebuffer, cJSON_AS4CPP_bool fmt);
/* Render a cJSON entity to text using a buffer already allocated in memory with given length. Returns 1 on success and 0 on failure. */
/* NOTE: cJSON is not always 100% accurate in estimating how much memory it will use, so to be safe allocate 5 bytes more than you actually need */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_AS4CPP_bool format);
/* Delete a cJSON entity and all subentities. */
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_Delete(cJSON *item);

/* Returns the number of items in an array (or object). */
CJSON_AS4CPP_PUBLIC(int) cJSON_AS4CPP_GetArraySize(const cJSON *array);
/* Retrieve item number "index" from array "array". Returns NULL if unsuccessful. */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_GetArrayItem(const cJSON *array, int index);
/* Get item "string" from object. Case insensitive. */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_GetObjectItem(const cJSON * const object, const char * const string);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_HasObjectItem(const cJSON *object, const char *string);
/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to look a few chars back to make sense of it. Defined when cJSON_AS4CPP_Parse() returns 0. 0 when cJSON_AS4CPP_Parse() succeeds.
 * NOTE: disabled, since this method is not thread-safe. See comments in source/external/cjson/cJSON.cpp.
CJSON_AS4CPP_PUBLIC(const char *) cJSON_AS4CPP_GetErrorPtr(void);
 */

/* Check item type and return its value */
CJSON_AS4CPP_PUBLIC(char *) cJSON_AS4CPP_GetStringValue(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(double) cJSON_AS4CPP_GetNumberValue(const cJSON * const item);

/* These functions check the type of an item */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsInvalid(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsFalse(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsTrue(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsBool(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsNull(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsNumber(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsString(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsArray(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsObject(const cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_IsRaw(const cJSON * const item);

/* These calls create a cJSON item of the appropriate type. */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateNull(void);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateTrue(void);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateFalse(void);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateBool(cJSON_AS4CPP_bool boolean);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateNumber(double num);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateInt64(long long num);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateString(const char *string);
/* raw json */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateRaw(const char *raw);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateArray(void);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateObject(void);

/* Create a string where valuestring references a string so
 * it will not be freed by cJSON_AS4CPP_Delete */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateStringReference(const char *string);
/* Create an object/array that only references it's elements so
 * they will not be freed by cJSON_AS4CPP_Delete */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateObjectReference(const cJSON *child);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateArrayReference(const cJSON *child);

/* These utilities create an Array of count items.
 * The parameter count cannot be greater than the number of elements in the number array, otherwise array access will be out of bounds.*/
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateIntArray(const int *numbers, int count);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateFloatArray(const float *numbers, int count);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateDoubleArray(const double *numbers, int count);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_CreateStringArray(const char *const *strings, int count);

/* Append item to the specified array/object. */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_AddItemToArray(cJSON *array, cJSON *item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_AddItemToObject(cJSON *object, const char *string, cJSON *item);
/* Use this when string is definitely const (i.e. a literal, or as good as), and will definitely survive the cJSON object.
 * WARNING: When this function was used, make sure to always check that (item->type & cJSON_AS4CPP_StringIsConst) is zero before
 * writing to `item->string` */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing cJSON to a new cJSON, but don't want to corrupt your existing cJSON. */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_AddItemReferenceToArray(cJSON *array, cJSON *item);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

/* Remove/Detach items from Arrays/Objects. */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_DetachItemViaPointer(cJSON *parent, cJSON * const item);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_DetachItemFromArray(cJSON *array, int which);
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_DeleteItemFromArray(cJSON *array, int which);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_DetachItemFromObject(cJSON *object, const char *string);
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_DeleteItemFromObject(cJSON *object, const char *string);
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

/* Update array items. */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_InsertItemInArray(cJSON *array, int which, cJSON *newitem); /* Shifts pre-existing items to the right. */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_ReplaceItemInObject(cJSON *object,const char *string,cJSON *newitem);
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_ReplaceItemInObjectCaseSensitive(cJSON *object,const char *string,cJSON *newitem);

/* Duplicate a cJSON item */
CJSON_AS4CPP_PUBLIC(cJSON *) cJSON_AS4CPP_Duplicate(const cJSON *item, cJSON_AS4CPP_bool recurse);
/* Duplicate will create a new, identical cJSON item to the one you pass, in new memory that will
 * need to be released. With recurse!=0, it will duplicate any children connected to the item.
 * The item->next and ->prev pointers are always zero on return from Duplicate. */
/* Recursively compare two cJSON items for equality. If either a or b is NULL or invalid, they will be considered unequal.
 * case_sensitive determines if object keys are treated case sensitive (1) or case insensitive (0) */
CJSON_AS4CPP_PUBLIC(cJSON_AS4CPP_bool) cJSON_AS4CPP_Compare(const cJSON * const a, const cJSON * const b, const cJSON_AS4CPP_bool case_sensitive);

/* Minify a strings, remove blank characters(such as ' ', '\t', '\r', '\n') from strings.
 * The input pointer json cannot point to a read-only address area, such as a string constant,
 * but should point to a readable and writable address area. */
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_Minify(char *json);

/* Helper functions for creating and adding items to an object at the same time.
 * They return the added item or NULL on failure. */
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddNullToObject(cJSON * const object, const char * const name);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddTrueToObject(cJSON * const object, const char * const name);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddFalseToObject(cJSON * const object, const char * const name);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_AS4CPP_bool boolean);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddNumberToObject(cJSON * const object, const char * const name, const double number);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddStringToObject(cJSON * const object, const char * const name, const char * const string);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddRawToObject(cJSON * const object, const char * const name, const char * const raw);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddObjectToObject(cJSON * const object, const char * const name);
CJSON_AS4CPP_PUBLIC(cJSON*) cJSON_AS4CPP_AddArrayToObject(cJSON * const object, const char * const name);

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define cJSON_AS4CPP_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* helper for the cJSON_AS4CPP_SetNumberValue macro */
CJSON_AS4CPP_PUBLIC(double) cJSON_AS4CPP_SetNumberHelper(cJSON *object, double number);
#define cJSON_AS4CPP_SetNumberValue(object, number) ((object != NULL) ? cJSON_AS4CPP_SetNumberHelper(object, (double)number) : (number))
/* Change the valuestring of a cJSON_AS4CPP_String object, only takes effect when type of object is cJSON_AS4CPP_String */
CJSON_AS4CPP_PUBLIC(char*) cJSON_AS4CPP_SetValuestring(cJSON *object, const char *valuestring);

/* Macro for iterating over an array or object */
#define cJSON_AS4CPP_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/* malloc/free objects using the malloc/free functions that have been set with cJSON_AS4CPP_InitHooks */
CJSON_AS4CPP_PUBLIC(void *) cJSON_AS4CPP_malloc(size_t size);
CJSON_AS4CPP_PUBLIC(void) cJSON_AS4CPP_free(void *object);

#ifdef __cplusplus
}
#endif

#endif
