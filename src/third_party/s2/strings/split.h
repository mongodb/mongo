// Copyright 2008 and onwards Google, Inc.
//
// Functions for splitting and parsing strings.  Functions may be migrated
// to this file from strutil.h in the future.
//
#ifndef STRINGS_SPLIT_H_
#define STRINGS_SPLIT_H_

#include <string>
using std::string;

#include <vector>
using std::vector;

#include <set>
using std::set;
using std::multiset;

#include <utility>
using std::pair;
using std::make_pair;

#include "base/definer.h"
#include "hash.h"

using namespace std;

// ----------------------------------------------------------------------
// SplitStringAllowEmpty()
// SplitStringToHashsetAllowEmpty()
// SplitStringToSetAllowEmpty()
// SplitStringToHashmapAllowEmpty()

//    Split a string using one or more character delimiters, presented
//    as a nul-terminated c string. Append the components to 'result'.
//    If there are consecutive delimiters, this function will return
//    corresponding empty strings.
//
//    If "full" is the empty string, yields an empty string as the only value.
// ----------------------------------------------------------------------
void SplitStringAllowEmpty(const string& full, const char* delim,
                           vector<string>* res);
void SplitStringToHashsetAllowEmpty(const string& full, const char* delim,
                                    hash_set<string>* res);
void SplitStringToSetAllowEmpty(const string& full, const char* delim,
                                set<string>* res);
// The even-positioned (0-based) components become the keys for the
// odd-positioned components that follow them. When there is an odd
// number of components, the value for the last key will be unchanged
// if the key was already present in the hash table, or will be the
// empty string if the key is a newly inserted key.
void SplitStringToHashmapAllowEmpty(const string& full, const char* delim,
                                    hash_map<string, string>* result);

// ----------------------------------------------------------------------
// SplitStringUsing()
// SplitStringToHashsetUsing()
// SplitStringToSetUsing()
// SplitStringToHashmapUsing()

//    Split a string using one or more character delimiters, presented
//    as a nul-terminated c string. Append the components to 'result'.
//    If there are consecutive delimiters, this function skips over
//    all of them.
// ----------------------------------------------------------------------
void SplitStringUsing(const string& full, const char* delim,
                      vector<string>* res);
void SplitStringToHashsetUsing(const string& full, const char* delim,
                               hash_set<string>* res);
void SplitStringToSetUsing(const string& full, const char* delim,
                           set<string>* res);
// The even-positioned (0-based) components become the keys for the
// odd-positioned components that follow them. When there is an odd
// number of components, the value for the last key will be unchanged
// if the key was already present in the hash table, or will be the
// empty string if the key is a newly inserted key.
void SplitStringToHashmapUsing(const string& full, const char* delim,
                               hash_map<string, string>* result);

// ----------------------------------------------------------------------
// SplitOneIntToken()
// SplitOneInt32Token()
// SplitOneUint32Token()
// SplitOneInt64Token()
// SplitOneUint64Token()
// SplitOneDoubleToken()
// SplitOneFloatToken()
//   Parse a single "delim" delimited number from "*source" into "*value".
//   Modify *source to point after the delimiter.
//   If no delimiter is present after the number, set *source to NULL.
//
//   If the start of *source is not an number, return false.
//   If the int is followed by the null character, return true.
//   If the int is not followed by a character from delim, return false.
//   If *source is NULL, return false.
//
//   They cannot handle decimal numbers with leading 0s, since they will be
//   treated as octal.
// ----------------------------------------------------------------------
bool SplitOneIntToken(const char** source, const char* delim,
                      int* value);
bool SplitOneInt32Token(const char** source, const char* delim,
                        int32* value);
bool SplitOneUint32Token(const char** source, const char* delim,
                         uint32* value);
bool SplitOneInt64Token(const char** source, const char* delim,
                        int64* value);
bool SplitOneUint64Token(const char** source, const char* delim,
                         uint64* value);
bool SplitOneDoubleToken(const char** source, const char* delim,
                         double* value);
bool SplitOneFloatToken(const char** source, const char* delim,
                        float* value);

// Some aliases, so that the function names are standardized against the names
// of the reflection setters/getters in proto2. This makes it easier to use
// certain macros with reflection when creating custom text formats for protos.

inline bool SplitOneUInt32Token(const char** source, const char* delim,
                         uint32* value) {
  return SplitOneUint32Token(source, delim, value);
}

inline bool SplitOneUInt64Token(const char** source, const char* delim,
                         uint64* value) {
  return SplitOneUint64Token(source, delim, value);
}

// ----------------------------------------------------------------------
// SplitOneDecimalIntToken()
// SplitOneDecimalInt32Token()
// SplitOneDecimalUint32Token()
// SplitOneDecimalInt64Token()
// SplitOneDecimalUint64Token()
// Parse a single "delim"-delimited number from "*source" into "*value".
// Unlike SplitOneIntToken, etc., this function always interprets
// the numbers as decimal.
bool SplitOneDecimalIntToken(const char** source, const char* delim,
                             int* value);
bool SplitOneDecimalInt32Token(const char** source, const char* delim,
                               int32* value);
bool SplitOneDecimalUint32Token(const char** source, const char* delim,
                                uint32* value);
bool SplitOneDecimalInt64Token(const char** source, const char* delim,
                               int64* value);
bool SplitOneDecimalUint64Token(const char** source, const char* delim,
                                uint64* value);

// ----------------------------------------------------------------------
// SplitOneHexUint32Token()
// SplitOneHexUint64Token()
// Once more, for hexadecimal numbers (unsigned only).
bool SplitOneHexUint32Token(const char** source, const char* delim,
                            uint32* value);
bool SplitOneHexUint64Token(const char** source, const char* delim,
                            uint64* value);


#endif  // STRINGS_SPLIT_H_
