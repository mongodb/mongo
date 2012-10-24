// Copyright (c) 1999, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Revamped and reorganized by Craig Silverstein
//
// This file contains the implementation of all our command line flags
// stuff.  Here's how everything fits together
//
// * FlagRegistry owns CommandLineFlags owns FlagValue.
// * FlagSaver holds a FlagRegistry (saves it at construct time,
//     restores it at destroy time).
// * CommandLineFlagParser lives outside that hierarchy, but works on
//     CommandLineFlags (modifying the FlagValues).
// * Free functions like SetCommandLineOption() work via one of the
//     above (such as CommandLineFlagParser).
//
// In more detail:
//
// -- The main classes that hold flag data:
//
// FlagValue holds the current value of a flag.  It's
// pseudo-templatized: every operation on a FlagValue is typed.  It
// also deals with storage-lifetime issues (so flag values don't go
// away in a destructor), which is why we need a whole class to hold a
// variable's value.
//
// CommandLineFlag is all the information about a single command-line
// flag.  It has a FlagValue for the flag's current value, but also
// the flag's name, type, etc.
//
// FlagRegistry is a collection of CommandLineFlags.  There's the
// global registry, which is where flags defined via DEFINE_foo()
// live.  But it's possible to define your own flag, manually, in a
// different registry you create.  (In practice, multiple registries
// are used only by FlagSaver).
//
// A given FlagValue is owned by exactly one CommandLineFlag.  A given
// CommandLineFlag is owned by exactly one FlagRegistry.  FlagRegistry
// has a lock; any operation that writes to a FlagValue or
// CommandLineFlag owned by that registry must acquire the
// FlagRegistry lock before doing so.
//
// --- Some other classes and free functions:
//
// CommandLineFlagInfo is a client-exposed version of CommandLineFlag.
// Once it's instantiated, it has no dependencies or relationships
// with any other part of this file.
//
// FlagRegisterer is the helper class used by the DEFINE_* macros to
// allow work to be done at global initialization time.
//
// CommandLineFlagParser is the class that reads from the commandline
// and instantiates flag values based on that.  It needs to poke into
// the innards of the FlagValue->CommandLineFlag->FlagRegistry class
// hierarchy to do that.  It's careful to acquire the FlagRegistry
// lock before doing any writing or other non-const actions.
//
// GetCommandLineOption is just a hook into registry routines to
// retrieve a flag based on its name.  SetCommandLineOption, on the
// other hand, hooks into CommandLineFlagParser.  Other API functions
// are, similarly, mostly hooks into the functionality described above.

// This comes first to ensure we define __STDC_FORMAT_MACROS in time.
#ifdef _WIN32
#include "windows/config.h"
#define safe_vsnprintf _vsnprintf
#define snprintf _snprintf
#else
#include <config.h>
#endif

#if defined(HAVE_INTTYPES_H) && !defined(__STDC_FORMAT_MACROS)
# define __STDC_FORMAT_MACROS 1   // gcc requires this to get PRId64, etc.
#endif

#ifdef _WIN32
#include "windows/gflags/gflags.h"
#else
#include <gflags/gflags.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#ifdef HAVE_FNMATCH_H
# include <fnmatch.h>
#endif
#include <stdarg.h> // For va_list and related operations
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>     // for pair<>
#include <vector>
#include "mutex.h"
#include "util.h"

#ifndef PATH_SEPARATOR
#define PATH_SEPARATOR  '/'
#endif


// Special flags, type 1: the 'recursive' flags.  They set another flag's val.
DEFINE_string(flagfile, "",
              "load flags from file");
DEFINE_string(fromenv, "",
              "set flags from the environment"
              " [use 'export FLAGS_flag1=value']");
DEFINE_string(tryfromenv, "",
              "set flags from the environment if present");

// Special flags, type 2: the 'parsing' flags.  They modify how we parse.
DEFINE_string(undefok, "",
              "comma-separated list of flag names that it is okay to specify "
              "on the command line even if the program does not define a flag "
              "with that name.  IMPORTANT: flags in this list that have "
              "arguments MUST use the flag=value format");

_START_GOOGLE_NAMESPACE_

using std::map;
using std::pair;
using std::sort;
using std::string;
using std::vector;

// This is used by the unittest to test error-exit code
void GFLAGS_DLL_DECL (*gflags_exitfunc)(int) = &exit;  // from stdlib.h


// The help message indicating that the commandline flag has been
// 'stripped'. It will not show up when doing "-help" and its
// variants. The flag is stripped if STRIP_FLAG_HELP is set to 1
// before including base/gflags.h

// This is used by this file, and also in gflags_reporting.cc
const char kStrippedFlagHelp[] = "\001\002\003\004 (unknown) \004\003\002\001";

namespace {

// There are also 'reporting' flags, in gflags_reporting.cc.

static const char kError[] = "ERROR: ";

// Indicates that undefined options are to be ignored.
// Enables deferred processing of flags in dynamically loaded libraries.
static bool allow_command_line_reparsing = false;

static bool logging_is_probably_set_up = false;

// This is a 'prototype' validate-function.  'Real' validate
// functions, take a flag-value as an argument: ValidateFn(bool) or
// ValidateFn(uint64).  However, for easier storage, we strip off this
// argument and then restore it when actually calling the function on
// a flag value.
typedef bool (*ValidateFnProto)();

// Whether we should die when reporting an error.
enum DieWhenReporting { DIE, DO_NOT_DIE };

// Report Error and exit if requested.
static void ReportError(DieWhenReporting should_die, const char* format, ...) {
  char error_message[255];
  va_list ap;
  va_start(ap, format);
  vsnprintf(error_message, sizeof(error_message), format, ap);
  va_end(ap);
  fprintf(stderr, "%s", error_message);
  fflush(stderr);   // should be unnecessary, but cygwin's rxvt buffers stderr
  if (should_die == DIE) gflags_exitfunc(1);
}


// --------------------------------------------------------------------
// FlagValue
//    This represent the value a single flag might have.  The major
//    functionality is to convert from a string to an object of a
//    given type, and back.  Thread-compatible.
// --------------------------------------------------------------------

class CommandLineFlag;
class FlagValue {
 public:
  FlagValue(void* valbuf, const char* type, bool transfer_ownership_of_value);
  ~FlagValue();

  bool ParseFrom(const char* spec);
  string ToString() const;

 private:
  friend class CommandLineFlag;  // for many things, including Validate()
  friend class GOOGLE_NAMESPACE::FlagSaverImpl;  // calls New()
  friend class FlagRegistry;     // checks value_buffer_ for flags_by_ptr_ map
  template <typename T> friend T GetFromEnv(const char*, const char*, T);
  friend bool TryParseLocked(const CommandLineFlag*, FlagValue*,
                             const char*, string*);  // for New(), CopyFrom()

  enum ValueType {
    FV_BOOL = 0,
    FV_INT32 = 1,
    FV_INT64 = 2,
    FV_UINT64 = 3,
    FV_DOUBLE = 4,
    FV_STRING = 5,
    FV_MAX_INDEX = 5,
  };
  const char* TypeName() const;
  bool Equal(const FlagValue& x) const;
  FlagValue* New() const;   // creates a new one with default value
  void CopyFrom(const FlagValue& x);
  int ValueSize() const;

  // Calls the given validate-fn on value_buffer_, and returns
  // whatever it returns.  But first casts validate_fn_proto to a
  // function that takes our value as an argument (eg void
  // (*validate_fn)(bool) for a bool flag).
  bool Validate(const char* flagname, ValidateFnProto validate_fn_proto) const;

  void* value_buffer_;          // points to the buffer holding our data
  int8 type_;                   // how to interpret value_
  bool owns_value_;         // whether to free value on destruct

  FlagValue(const FlagValue&);   // no copying!
  void operator=(const FlagValue&);
};


// This could be a templated method of FlagValue, but doing so adds to the
// size of the .o.  Since there's no type-safety here anyway, macro is ok.
#define VALUE_AS(type)  *reinterpret_cast<type*>(value_buffer_)
#define OTHER_VALUE_AS(fv, type)  *reinterpret_cast<type*>(fv.value_buffer_)
#define SET_VALUE_AS(type, value)  VALUE_AS(type) = (value)

FlagValue::FlagValue(void* valbuf, const char* type,
                     bool transfer_ownership_of_value)
    : value_buffer_(valbuf),
      owns_value_(transfer_ownership_of_value) {
  for (type_ = 0; type_ <= FV_MAX_INDEX; ++type_) {
    if (!strcmp(type, TypeName())) {
      break;
    }
  }
  assert(type_ <= FV_MAX_INDEX);  // Unknown typename
}

FlagValue::~FlagValue() {
  if (!owns_value_) {
    return;
  }
  switch (type_) {
    case FV_BOOL: delete reinterpret_cast<bool*>(value_buffer_); break;
    case FV_INT32: delete reinterpret_cast<int32*>(value_buffer_); break;
    case FV_INT64: delete reinterpret_cast<int64*>(value_buffer_); break;
    case FV_UINT64: delete reinterpret_cast<uint64*>(value_buffer_); break;
    case FV_DOUBLE: delete reinterpret_cast<double*>(value_buffer_); break;
    case FV_STRING: delete reinterpret_cast<string*>(value_buffer_); break;
  }
}

bool FlagValue::ParseFrom(const char* value) {
  if (type_ == FV_BOOL) {
    const char* kTrue[] = { "1", "t", "true", "y", "yes" };
    const char* kFalse[] = { "0", "f", "false", "n", "no" };
    COMPILE_ASSERT(sizeof(kTrue) == sizeof(kFalse), true_false_equal);
    for (size_t i = 0; i < sizeof(kTrue)/sizeof(*kTrue); ++i) {
      if (strcasecmp(value, kTrue[i]) == 0) {
        SET_VALUE_AS(bool, true);
        return true;
      } else if (strcasecmp(value, kFalse[i]) == 0) {
        SET_VALUE_AS(bool, false);
        return true;
      }
    }
    return false;   // didn't match a legal input

  } else if (type_ == FV_STRING) {
    SET_VALUE_AS(string, value);
    return true;
  }

  // OK, it's likely to be numeric, and we'll be using a strtoXXX method.
  if (value[0] == '\0')   // empty-string is only allowed for string type.
    return false;
  char* end;
  // Leading 0x puts us in base 16.  But leading 0 does not put us in base 8!
  // It caused too many bugs when we had that behavior.
  int base = 10;    // by default
  if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    base = 16;
  errno = 0;

  switch (type_) {
    case FV_INT32: {
      const int64 r = strto64(value, &end, base);
      if (errno || end != value + strlen(value))  return false;  // bad parse
      if (static_cast<int32>(r) != r)  // worked, but number out of range
        return false;
      SET_VALUE_AS(int32, static_cast<int32>(r));
      return true;
    }
    case FV_INT64: {
      const int64 r = strto64(value, &end, base);
      if (errno || end != value + strlen(value))  return false;  // bad parse
      SET_VALUE_AS(int64, r);
      return true;
    }
    case FV_UINT64: {
      while (*value == ' ') value++;
      if (*value == '-') return false;  // negative number
      const uint64 r = strtou64(value, &end, base);
      if (errno || end != value + strlen(value))  return false;  // bad parse
      SET_VALUE_AS(uint64, r);
      return true;
    }
    case FV_DOUBLE: {
      const double r = strtod(value, &end);
      if (errno || end != value + strlen(value))  return false;  // bad parse
      SET_VALUE_AS(double, r);
      return true;
    }
    default: {
      assert(false);  // unknown type
      return false;
    }
  }
}

string FlagValue::ToString() const {
  char intbuf[64];    // enough to hold even the biggest number
  switch (type_) {
    case FV_BOOL:
      return VALUE_AS(bool) ? "true" : "false";
    case FV_INT32:
      snprintf(intbuf, sizeof(intbuf), "%"PRId32, VALUE_AS(int32));
      return intbuf;
    case FV_INT64:
      snprintf(intbuf, sizeof(intbuf), "%"PRId64, VALUE_AS(int64));
      return intbuf;
    case FV_UINT64:
      snprintf(intbuf, sizeof(intbuf), "%"PRIu64, VALUE_AS(uint64));
      return intbuf;
    case FV_DOUBLE:
      snprintf(intbuf, sizeof(intbuf), "%.17g", VALUE_AS(double));
      return intbuf;
    case FV_STRING:
      return VALUE_AS(string);
    default:
      assert(false);
      return "";  // unknown type
  }
}

bool FlagValue::Validate(const char* flagname,
                         ValidateFnProto validate_fn_proto) const {
  switch (type_) {
    case FV_BOOL:
      return reinterpret_cast<bool (*)(const char*, bool)>(
          validate_fn_proto)(flagname, VALUE_AS(bool));
    case FV_INT32:
      return reinterpret_cast<bool (*)(const char*, int32)>(
          validate_fn_proto)(flagname, VALUE_AS(int32));
    case FV_INT64:
      return reinterpret_cast<bool (*)(const char*, int64)>(
          validate_fn_proto)(flagname, VALUE_AS(int64));
    case FV_UINT64:
      return reinterpret_cast<bool (*)(const char*, uint64)>(
          validate_fn_proto)(flagname, VALUE_AS(uint64));
    case FV_DOUBLE:
      return reinterpret_cast<bool (*)(const char*, double)>(
          validate_fn_proto)(flagname, VALUE_AS(double));
    case FV_STRING:
      return reinterpret_cast<bool (*)(const char*, const string&)>(
          validate_fn_proto)(flagname, VALUE_AS(string));
    default:
      assert(false);  // unknown type
      return false;
  }
}

const char* FlagValue::TypeName() const {
  static const char types[] =
      "bool\0xx"
      "int32\0x"
      "int64\0x"
      "uint64\0"
      "double\0"
      "string";
  if (type_ > FV_MAX_INDEX) {
    assert(false);
    return "";
  }
  // Directly indexing the strigns in the 'types' string, each of them
  // is 7 bytes long.
  return &types[type_ * 7];
}

bool FlagValue::Equal(const FlagValue& x) const {
  if (type_ != x.type_)
    return false;
  switch (type_) {
    case FV_BOOL:   return VALUE_AS(bool) == OTHER_VALUE_AS(x, bool);
    case FV_INT32:  return VALUE_AS(int32) == OTHER_VALUE_AS(x, int32);
    case FV_INT64:  return VALUE_AS(int64) == OTHER_VALUE_AS(x, int64);
    case FV_UINT64: return VALUE_AS(uint64) == OTHER_VALUE_AS(x, uint64);
    case FV_DOUBLE: return VALUE_AS(double) == OTHER_VALUE_AS(x, double);
    case FV_STRING: return VALUE_AS(string) == OTHER_VALUE_AS(x, string);
    default: assert(false); return false;  // unknown type
  }
}

FlagValue* FlagValue::New() const {
  const char *type = TypeName();
  switch (type_) {
    case FV_BOOL:   return new FlagValue(new bool(false), type, true);
    case FV_INT32:  return new FlagValue(new int32(0), type, true);
    case FV_INT64:  return new FlagValue(new int64(0), type, true);
    case FV_UINT64: return new FlagValue(new uint64(0), type, true);
    case FV_DOUBLE: return new FlagValue(new double(0.0), type, true);
    case FV_STRING: return new FlagValue(new string, type, true);
    default: assert(false); return NULL;  // unknown type
  }
}

void FlagValue::CopyFrom(const FlagValue& x) {
  assert(type_ == x.type_);
  switch (type_) {
    case FV_BOOL:   SET_VALUE_AS(bool, OTHER_VALUE_AS(x, bool));      break;
    case FV_INT32:  SET_VALUE_AS(int32, OTHER_VALUE_AS(x, int32));    break;
    case FV_INT64:  SET_VALUE_AS(int64, OTHER_VALUE_AS(x, int64));    break;
    case FV_UINT64: SET_VALUE_AS(uint64, OTHER_VALUE_AS(x, uint64));  break;
    case FV_DOUBLE: SET_VALUE_AS(double, OTHER_VALUE_AS(x, double));  break;
    case FV_STRING: SET_VALUE_AS(string, OTHER_VALUE_AS(x, string));  break;
    default: assert(false);  // unknown type
  }
}

int FlagValue::ValueSize() const {
  if (type_ > FV_MAX_INDEX) {
    assert(false);  // unknown type
    return 0;
  }
  static const uint8 valuesize[] = {
    sizeof(bool),
    sizeof(int32),
    sizeof(int64),
    sizeof(uint64),
    sizeof(double),
    sizeof(string),
  };
  return valuesize[type_];
}

// --------------------------------------------------------------------
// CommandLineFlag
//    This represents a single flag, including its name, description,
//    default value, and current value.  Mostly this serves as a
//    struct, though it also knows how to register itself.
//       All CommandLineFlags are owned by a (exactly one)
//    FlagRegistry.  If you wish to modify fields in this class, you
//    should acquire the FlagRegistry lock for the registry that owns
//    this flag.
// --------------------------------------------------------------------

class CommandLineFlag {
 public:
  // Note: we take over memory-ownership of current_val and default_val.
  CommandLineFlag(const char* name, const char* help, const char* filename,
                  FlagValue* current_val, FlagValue* default_val);
  ~CommandLineFlag();

  const char* name() const { return name_; }
  const char* help() const { return help_; }
  const char* filename() const { return file_; }
  const char* CleanFileName() const;  // nixes irrelevant prefix such as homedir
  string current_value() const { return current_->ToString(); }
  string default_value() const { return defvalue_->ToString(); }
  const char* type_name() const { return defvalue_->TypeName(); }
  ValidateFnProto validate_function() const { return validate_fn_proto_; }
  const void* flag_ptr() const { return current_->value_buffer_; }

  void FillCommandLineFlagInfo(struct CommandLineFlagInfo* result);

  // If validate_fn_proto_ is non-NULL, calls it on value, returns result.
  bool Validate(const FlagValue& value) const;
  bool ValidateCurrent() const { return Validate(*current_); }

 private:
  // for SetFlagLocked() and setting flags_by_ptr_
  friend class FlagRegistry;
  friend class GOOGLE_NAMESPACE::FlagSaverImpl;  // for cloning the values
  // set validate_fn
  friend bool AddFlagValidator(const void*, ValidateFnProto);

  // This copies all the non-const members: modified, processed, defvalue, etc.
  void CopyFrom(const CommandLineFlag& src);

  void UpdateModifiedBit();

  const char* const name_;     // Flag name
  const char* const help_;     // Help message
  const char* const file_;     // Which file did this come from?
  bool modified_;              // Set after default assignment?
  FlagValue* defvalue_;        // Default value for flag
  FlagValue* current_;         // Current value for flag
  // This is a casted, 'generic' version of validate_fn, which actually
  // takes a flag-value as an arg (void (*validate_fn)(bool), say).
  // When we pass this to current_->Validate(), it will cast it back to
  // the proper type.  This may be NULL to mean we have no validate_fn.
  ValidateFnProto validate_fn_proto_;

  CommandLineFlag(const CommandLineFlag&);   // no copying!
  void operator=(const CommandLineFlag&);
};

CommandLineFlag::CommandLineFlag(const char* name, const char* help,
                                 const char* filename,
                                 FlagValue* current_val, FlagValue* default_val)
    : name_(name), help_(help), file_(filename), modified_(false),
      defvalue_(default_val), current_(current_val), validate_fn_proto_(NULL) {
}

CommandLineFlag::~CommandLineFlag() {
  delete current_;
  delete defvalue_;
}

const char* CommandLineFlag::CleanFileName() const {
  // Compute top-level directory & file that this appears in
  // search full path backwards.
  // Stop going backwards at kRootDir; and skip by the first slash.
  static const char kRootDir[] = "";    // can set this to root directory,

  if (sizeof(kRootDir)-1 == 0)          // no prefix to strip
    return filename();

  const char* clean_name = filename() + strlen(filename()) - 1;
  while ( clean_name > filename() ) {
    if (*clean_name == PATH_SEPARATOR) {
      if (strncmp(clean_name, kRootDir, sizeof(kRootDir)-1) == 0) {
        clean_name += sizeof(kRootDir)-1;    // past root-dir
        break;
      }
    }
    --clean_name;
  }
  while ( *clean_name == PATH_SEPARATOR ) ++clean_name;  // Skip any slashes
  return clean_name;
}

void CommandLineFlag::FillCommandLineFlagInfo(
    CommandLineFlagInfo* result) {
  result->name = name();
  result->type = type_name();
  result->description = help();
  result->current_value = current_value();
  result->default_value = default_value();
  result->filename = CleanFileName();
  UpdateModifiedBit();
  result->is_default = !modified_;
  result->has_validator_fn = validate_function() != NULL;
  result->flag_ptr = flag_ptr();
}

void CommandLineFlag::UpdateModifiedBit() {
  // Update the "modified" bit in case somebody bypassed the
  // Flags API and wrote directly through the FLAGS_name variable.
  if (!modified_ && !current_->Equal(*defvalue_)) {
    modified_ = true;
  }
}

void CommandLineFlag::CopyFrom(const CommandLineFlag& src) {
  // Note we only copy the non-const members; others are fixed at construct time
  if (modified_ != src.modified_) modified_ = src.modified_;
  if (!current_->Equal(*src.current_)) current_->CopyFrom(*src.current_);
  if (!defvalue_->Equal(*src.defvalue_)) defvalue_->CopyFrom(*src.defvalue_);
  if (validate_fn_proto_ != src.validate_fn_proto_)
    validate_fn_proto_ = src.validate_fn_proto_;
}

bool CommandLineFlag::Validate(const FlagValue& value) const {

  if (validate_function() == NULL)
    return true;
  else
    return value.Validate(name(), validate_function());
}


// --------------------------------------------------------------------
// FlagRegistry
//    A FlagRegistry singleton object holds all flag objects indexed
//    by their names so that if you know a flag's name (as a C
//    string), you can access or set it.  If the function is named
//    FooLocked(), you must own the registry lock before calling
//    the function; otherwise, you should *not* hold the lock, and
//    the function will acquire it itself if needed.
// --------------------------------------------------------------------

struct StringCmp {  // Used by the FlagRegistry map class to compare char*'s
  bool operator() (const char* s1, const char* s2) const {
    return (strcmp(s1, s2) < 0);
  }
};


class FlagRegistry {
 public:
  FlagRegistry() {
  }
  ~FlagRegistry() {
    // Not using STLDeleteElements as that resides in util and this
    // class is base.
    for (FlagMap::iterator p = flags_.begin(), e = flags_.end(); p != e; ++p) {
      CommandLineFlag* flag = p->second;
      delete flag;
    }
  }

  static void DeleteGlobalRegistry() {
    delete global_registry_;
    global_registry_ = NULL;
  }

  // Store a flag in this registry.  Takes ownership of the given pointer.
  void RegisterFlag(CommandLineFlag* flag);

  void Lock() { lock_.Lock(); }
  void Unlock() { lock_.Unlock(); }

  // Returns the flag object for the specified name, or NULL if not found.
  CommandLineFlag* FindFlagLocked(const char* name);

  // Returns the flag object whose current-value is stored at flag_ptr.
  // That is, for whom current_->value_buffer_ == flag_ptr
  CommandLineFlag* FindFlagViaPtrLocked(const void* flag_ptr);

  // A fancier form of FindFlag that works correctly if name is of the
  // form flag=value.  In that case, we set key to point to flag, and
  // modify v to point to the value (if present), and return the flag
  // with the given name.  If the flag does not exist, returns NULL
  // and sets error_message.
  CommandLineFlag* SplitArgumentLocked(const char* argument,
                                       string* key, const char** v,
                                       string* error_message);

  // Set the value of a flag.  If the flag was successfully set to
  // value, set msg to indicate the new flag-value, and return true.
  // Otherwise, set msg to indicate the error, leave flag unchanged,
  // and return false.  msg can be NULL.
  bool SetFlagLocked(CommandLineFlag* flag, const char* value,
                     FlagSettingMode set_mode, string* msg);

  static FlagRegistry* GlobalRegistry();   // returns a singleton registry

 private:
  friend class GOOGLE_NAMESPACE::FlagSaverImpl;  // reads all the flags in order to copy them
  friend class CommandLineFlagParser;    // for ValidateAllFlags
  friend void GOOGLE_NAMESPACE::GetAllFlags(vector<CommandLineFlagInfo>*);

  // The map from name to flag, for FindFlagLocked().
  typedef map<const char*, CommandLineFlag*, StringCmp> FlagMap;
  typedef FlagMap::iterator FlagIterator;
  typedef FlagMap::const_iterator FlagConstIterator;
  FlagMap flags_;

  // The map from current-value pointer to flag, fo FindFlagViaPtrLocked().
  typedef map<const void*, CommandLineFlag*> FlagPtrMap;
  FlagPtrMap flags_by_ptr_;

  static FlagRegistry* global_registry_;   // a singleton registry

  Mutex lock_;
  static Mutex global_registry_lock_;

  static void InitGlobalRegistry();

  // Disallow
  FlagRegistry(const FlagRegistry&);
  FlagRegistry& operator=(const FlagRegistry&);
};

class FlagRegistryLock {
 public:
  explicit FlagRegistryLock(FlagRegistry* fr) : fr_(fr) { fr_->Lock(); }
  ~FlagRegistryLock() { fr_->Unlock(); }
 private:
  FlagRegistry *const fr_;
};


void FlagRegistry::RegisterFlag(CommandLineFlag* flag) {
  Lock();
  pair<FlagIterator, bool> ins =
    flags_.insert(pair<const char*, CommandLineFlag*>(flag->name(), flag));
  if (ins.second == false) {   // means the name was already in the map
    if (strcmp(ins.first->second->filename(), flag->filename()) != 0) {
      ReportError(DIE, "ERROR: flag '%s' was defined more than once "
                  "(in files '%s' and '%s').\n",
                  flag->name(),
                  ins.first->second->filename(),
                  flag->filename());
    } else {
      ReportError(DIE, "ERROR: something wrong with flag '%s' in file '%s'.  "
                  "One possibility: file '%s' is being linked both statically "
                  "and dynamically into this executable.\n",
                  flag->name(),
                  flag->filename(), flag->filename());
    }
  }
  // Also add to the flags_by_ptr_ map.
  flags_by_ptr_[flag->current_->value_buffer_] = flag;
  Unlock();
}

CommandLineFlag* FlagRegistry::FindFlagLocked(const char* name) {
  FlagConstIterator i = flags_.find(name);
  if (i == flags_.end()) {
    return NULL;
  } else {
    return i->second;
  }
}

CommandLineFlag* FlagRegistry::FindFlagViaPtrLocked(const void* flag_ptr) {
  FlagPtrMap::const_iterator i = flags_by_ptr_.find(flag_ptr);
  if (i == flags_by_ptr_.end()) {
    return NULL;
  } else {
    return i->second;
  }
}

CommandLineFlag* FlagRegistry::SplitArgumentLocked(const char* arg,
                                                   string* key,
                                                   const char** v,
                                                   string* error_message) {
  // Find the flag object for this option
  const char* flag_name;
  const char* value = strchr(arg, '=');
  if (value == NULL) {
    key->assign(arg);
    *v = NULL;
  } else {
    // Strip out the "=value" portion from arg
    key->assign(arg, value-arg);
    *v = ++value;    // advance past the '='
  }
  flag_name = key->c_str();

  CommandLineFlag* flag = FindFlagLocked(flag_name);

  if (flag == NULL) {
    // If we can't find the flag-name, then we should return an error.
    // The one exception is if 1) the flag-name is 'nox', 2) there
    // exists a flag named 'x', and 3) 'x' is a boolean flag.
    // In that case, we want to return flag 'x'.
    if (!(flag_name[0] == 'n' && flag_name[1] == 'o')) {
      // flag-name is not 'nox', so we're not in the exception case.
      *error_message = StringPrintf("%sunknown command line flag '%s'\n",
                                    kError, key->c_str());
      return NULL;
    }
    flag = FindFlagLocked(flag_name+2);
    if (flag == NULL) {
      // No flag named 'x' exists, so we're not in the exception case.
      *error_message = StringPrintf("%sunknown command line flag '%s'\n",
                                    kError, key->c_str());
      return NULL;
    }
    if (strcmp(flag->type_name(), "bool") != 0) {
      // 'x' exists but is not boolean, so we're not in the exception case.
      *error_message = StringPrintf(
          "%sboolean value (%s) specified for %s command line flag\n",
          kError, key->c_str(), flag->type_name());
      return NULL;
    }
    // We're in the exception case!
    // Make up a fake value to replace the "no" we stripped out
    key->assign(flag_name+2);   // the name without the "no"
    *v = "0";
  }

  // Assign a value if this is a boolean flag
  if (*v == NULL && strcmp(flag->type_name(), "bool") == 0) {
    *v = "1";    // the --nox case was already handled, so this is the --x case
  }

  return flag;
}

bool TryParseLocked(const CommandLineFlag* flag, FlagValue* flag_value,
                    const char* value, string* msg) {
  // Use tenative_value, not flag_value, until we know value is valid.
  FlagValue* tentative_value = flag_value->New();
  if (!tentative_value->ParseFrom(value)) {
    if (msg) {
      StringAppendF(msg,
                    "%sillegal value '%s' specified for %s flag '%s'\n",
                    kError, value,
                    flag->type_name(), flag->name());
    }
    delete tentative_value;
    return false;
  } else if (!flag->Validate(*tentative_value)) {
    if (msg) {
      StringAppendF(msg,
          "%sfailed validation of new value '%s' for flag '%s'\n",
          kError, tentative_value->ToString().c_str(),
          flag->name());
    }
    delete tentative_value;
    return false;
  } else {
    flag_value->CopyFrom(*tentative_value);
    if (msg) {
      StringAppendF(msg, "%s set to %s\n",
                    flag->name(), flag_value->ToString().c_str());
    }
    delete tentative_value;
    return true;
  }
}

bool FlagRegistry::SetFlagLocked(CommandLineFlag* flag,
                                 const char* value,
                                 FlagSettingMode set_mode,
                                 string* msg) {
  flag->UpdateModifiedBit();
  switch (set_mode) {
    case SET_FLAGS_VALUE: {
      // set or modify the flag's value
      if (!TryParseLocked(flag, flag->current_, value, msg))
        return false;
      flag->modified_ = true;
      break;
    }
    case SET_FLAG_IF_DEFAULT: {
      // set the flag's value, but only if it hasn't been set by someone else
      if (!flag->modified_) {
        if (!TryParseLocked(flag, flag->current_, value, msg))
          return false;
        flag->modified_ = true;
      } else {
        *msg = StringPrintf("%s set to %s",
                            flag->name(), flag->current_value().c_str());
      }
      break;
    }
    case SET_FLAGS_DEFAULT: {
      // modify the flag's default-value
      if (!TryParseLocked(flag, flag->defvalue_, value, msg))
        return false;
      if (!flag->modified_) {
        // Need to set both defvalue *and* current, in this case
        TryParseLocked(flag, flag->current_, value, NULL);
      }
      break;
    }
    default: {
      // unknown set_mode
      assert(false);
      return false;
    }
  }

  return true;
}

// Get the singleton FlagRegistry object
FlagRegistry* FlagRegistry::global_registry_ = NULL;
Mutex FlagRegistry::global_registry_lock_(Mutex::LINKER_INITIALIZED);

FlagRegistry* FlagRegistry::GlobalRegistry() {
  MutexLock acquire_lock(&global_registry_lock_);
  if (!global_registry_) {
    global_registry_ = new FlagRegistry;
  }
  return global_registry_;
}

// --------------------------------------------------------------------
// CommandLineFlagParser
//    Parsing is done in two stages.  In the first, we go through
//    argv.  For every flag-like arg we can make sense of, we parse
//    it and set the appropriate FLAGS_* variable.  For every flag-
//    like arg we can't make sense of, we store it in a vector,
//    along with an explanation of the trouble.  In stage 2, we
//    handle the 'reporting' flags like --help and --mpm_version.
//    (This is via a call to HandleCommandLineHelpFlags(), in
//    gflags_reporting.cc.)
//    An optional stage 3 prints out the error messages.
//       This is a bit of a simplification.  For instance, --flagfile
//    is handled as soon as it's seen in stage 1, not in stage 2.
// --------------------------------------------------------------------

class CommandLineFlagParser {
 public:
  // The argument is the flag-registry to register the parsed flags in
  explicit CommandLineFlagParser(FlagRegistry* reg) : registry_(reg) {}
  ~CommandLineFlagParser() {}

  // Stage 1: Every time this is called, it reads all flags in argv.
  // However, it ignores all flags that have been successfully set
  // before.  Typically this is only called once, so this 'reparsing'
  // behavior isn't important.  It can be useful when trying to
  // reparse after loading a dll, though.
  uint32 ParseNewCommandLineFlags(int* argc, char*** argv, bool remove_flags);

  // Stage 2: print reporting info and exit, if requested.
  // In gflags_reporting.cc:HandleCommandLineHelpFlags().

  // Stage 3: validate all the commandline flags that have validators
  // registered.
  void ValidateAllFlags();

  // Stage 4: report any errors and return true if any were found.
  bool ReportErrors();

  // Set a particular command line option.  "newval" is a string
  // describing the new value that the option has been set to.  If
  // option_name does not specify a valid option name, or value is not
  // a valid value for option_name, newval is empty.  Does recursive
  // processing for --flagfile and --fromenv.  Returns the new value
  // if everything went ok, or empty-string if not.  (Actually, the
  // return-string could hold many flag/value pairs due to --flagfile.)
  // NB: Must have called registry_->Lock() before calling this function.
  string ProcessSingleOptionLocked(CommandLineFlag* flag,
                                   const char* value,
                                   FlagSettingMode set_mode);

  // Set a whole batch of command line options as specified by contentdata,
  // which is in flagfile format (and probably has been read from a flagfile).
  // Returns the new value if everything went ok, or empty-string if
  // not.  (Actually, the return-string could hold many flag/value
  // pairs due to --flagfile.)
  // NB: Must have called registry_->Lock() before calling this function.
  string ProcessOptionsFromStringLocked(const string& contentdata,
                                        FlagSettingMode set_mode);

  // These are the 'recursive' flags, defined at the top of this file.
  // Whenever we see these flags on the commandline, we must take action.
  // These are called by ProcessSingleOptionLocked and, similarly, return
  // new values if everything went ok, or the empty-string if not.
  string ProcessFlagfileLocked(const string& flagval, FlagSettingMode set_mode);
  // diff fromenv/tryfromenv
  string ProcessFromenvLocked(const string& flagval, FlagSettingMode set_mode,
                              bool errors_are_fatal);

 private:
  FlagRegistry* const registry_;
  map<string, string> error_flags_;      // map from name to error message
  // This could be a set<string>, but we reuse the map to minimize the .o size
  map<string, string> undefined_names_;  // --[flag] name was not registered
};


// Parse a list of (comma-separated) flags.
static void ParseFlagList(const char* value, vector<string>* flags) {
  for (const char *p = value; p && *p; value = p) {
    p = strchr(value, ',');
    size_t len;
    if (p) {
      len = p - value;
      p++;
    } else {
      len = strlen(value);
    }

    if (len == 0)
      ReportError(DIE, "ERROR: empty flaglist entry\n");
    if (value[0] == '-')
      ReportError(DIE, "ERROR: flag \"%*s\" begins with '-'\n", len, value);

    flags->push_back(string(value, len));
  }
}

// Snarf an entire file into a C++ string.  This is just so that we
// can do all the I/O in one place and not worry about it everywhere.
// Plus, it's convenient to have the whole file contents at hand.
// Adds a newline at the end of the file.
#define PFATAL(s)  do { perror(s); gflags_exitfunc(1); } while (0)

static string ReadFileIntoString(const char* filename) {
  const int kBufSize = 8092;
  char buffer[kBufSize];
  string s;
  FILE* fp = fopen(filename, "r");
  if (!fp)  PFATAL(filename);
  size_t n;
  while ( (n=fread(buffer, 1, kBufSize, fp)) > 0 ) {
    if (ferror(fp))  PFATAL(filename);
    s.append(buffer, n);
  }
  fclose(fp);
  return s;
}

uint32 CommandLineFlagParser::ParseNewCommandLineFlags(int* argc, char*** argv,
                                                       bool remove_flags) {
  const char *program_name = strrchr((*argv)[0], PATH_SEPARATOR);   // nix path
  program_name = (program_name == NULL ? (*argv)[0] : program_name+1);

  int first_nonopt = *argc;        // for non-options moved to the end

  registry_->Lock();
  for (int i = 1; i < first_nonopt; i++) {
    char* arg = (*argv)[i];

    // Like getopt(), we permute non-option flags to be at the end.
    if (arg[0] != '-' ||           // must be a program argument
        (arg[0] == '-' && arg[1] == '\0')) {  // "-" is an argument, not a flag
      memmove((*argv) + i, (*argv) + i+1, (*argc - (i+1)) * sizeof((*argv)[i]));
      (*argv)[*argc-1] = arg;      // we go last
      first_nonopt--;              // we've been pushed onto the stack
      i--;                         // to undo the i++ in the loop
      continue;
    }

    if (arg[0] == '-') arg++;      // allow leading '-'
    if (arg[0] == '-') arg++;      // or leading '--'

    // -- alone means what it does for GNU: stop options parsing
    if (*arg == '\0') {
      first_nonopt = i+1;
      break;
    }

    // Find the flag object for this option
    string key;
    const char* value;
    string error_message;
    CommandLineFlag* flag = registry_->SplitArgumentLocked(arg, &key, &value,
                                                           &error_message);
    if (flag == NULL) {
      undefined_names_[key] = "";    // value isn't actually used
      error_flags_[key] = error_message;
      continue;
    }

    if (value == NULL) {
      // Boolean options are always assigned a value by SplitArgumentLocked()
      assert(strcmp(flag->type_name(), "bool") != 0);
      if (i+1 >= first_nonopt) {
        // This flag needs a value, but there is nothing available
        error_flags_[key] = (string(kError) + "flag '" + (*argv)[i] + "'"
                             + " is missing its argument");
        if (flag->help() && flag->help()[0] > '\001') {
          // Be useful in case we have a non-stripped description.
          error_flags_[key] += string("; flag description: ") + flag->help();
        }
        error_flags_[key] += "\n";
        break;    // we treat this as an unrecoverable error
      } else {
        value = (*argv)[++i];                   // read next arg for value

        // Heuristic to detect the case where someone treats a string arg
        // like a bool:
        // --my_string_var --foo=bar
        // We look for a flag of string type, whose value begins with a
        // dash, and where the flag-name and value are separated by a
        // space rather than an '='.
        // To avoid false positives, we also require the word "true"
        // or "false" in the help string.  Without this, a valid usage
        // "-lat -30.5" would trigger the warning.  The common cases we
        // want to solve talk about true and false as values.
        if (value[0] == '-'
            && strcmp(flag->type_name(), "string") == 0
            && (strstr(flag->help(), "true")
                || strstr(flag->help(), "false"))) {
          LOG(WARNING) << "Did you really mean to set flag '"
                       << flag->name() << "' to the value '"
                       << value << "'?";
        }
      }
    }

    // TODO(csilvers): only set a flag if we hadn't set it before here
    ProcessSingleOptionLocked(flag, value, SET_FLAGS_VALUE);
  }
  registry_->Unlock();

  if (remove_flags) {   // Fix up argc and argv by removing command line flags
    (*argv)[first_nonopt-1] = (*argv)[0];
    (*argv) += (first_nonopt-1);
    (*argc) -= (first_nonopt-1);
    first_nonopt = 1;   // because we still don't count argv[0]
  }

  logging_is_probably_set_up = true;   // because we've parsed --logdir, etc.

  return first_nonopt;
}

string CommandLineFlagParser::ProcessFlagfileLocked(const string& flagval,
                                                    FlagSettingMode set_mode) {
  if (flagval.empty())
    return "";

  string msg;
  vector<string> filename_list;
  ParseFlagList(flagval.c_str(), &filename_list);  // take a list of filenames
  for (size_t i = 0; i < filename_list.size(); ++i) {
    const char* file = filename_list[i].c_str();
    msg += ProcessOptionsFromStringLocked(ReadFileIntoString(file), set_mode);
  }
  return msg;
}

string CommandLineFlagParser::ProcessFromenvLocked(const string& flagval,
                                                   FlagSettingMode set_mode,
                                                   bool errors_are_fatal) {
  if (flagval.empty())
    return "";

  string msg;
  vector<string> flaglist;
  ParseFlagList(flagval.c_str(), &flaglist);

  for (size_t i = 0; i < flaglist.size(); ++i) {
    const char* flagname = flaglist[i].c_str();
    CommandLineFlag* flag = registry_->FindFlagLocked(flagname);
    if (flag == NULL) {
      error_flags_[flagname] =
          StringPrintf("%sunknown command line flag '%s' "
                       "(via --fromenv or --tryfromenv)\n",
                       kError, flagname);
      undefined_names_[flagname] = "";
      continue;
    }

    const string envname = string("FLAGS_") + string(flagname);
    const char* envval = getenv(envname.c_str());
    if (!envval) {
      if (errors_are_fatal) {
        error_flags_[flagname] = (string(kError) + envname +
                                  " not found in environment\n");
      }
      continue;
    }

    // Avoid infinite recursion.
    if ((strcmp(envval, "fromenv") == 0) ||
        (strcmp(envval, "tryfromenv") == 0)) {
      error_flags_[flagname] =
          StringPrintf("%sinfinite recursion on environment flag '%s'\n",
                       kError, envval);
      continue;
    }

    msg += ProcessSingleOptionLocked(flag, envval, set_mode);
  }
  return msg;
}

string CommandLineFlagParser::ProcessSingleOptionLocked(
    CommandLineFlag* flag, const char* value, FlagSettingMode set_mode) {
  string msg;
  if (value && !registry_->SetFlagLocked(flag, value, set_mode, &msg)) {
    error_flags_[flag->name()] = msg;
    return "";
  }

  // The recursive flags, --flagfile and --fromenv and --tryfromenv,
  // must be dealt with as soon as they're seen.  They will emit
  // messages of their own.
  if (strcmp(flag->name(), "flagfile") == 0) {
    msg += ProcessFlagfileLocked(FLAGS_flagfile, set_mode);

  } else if (strcmp(flag->name(), "fromenv") == 0) {
    // last arg indicates envval-not-found is fatal (unlike in --tryfromenv)
    msg += ProcessFromenvLocked(FLAGS_fromenv, set_mode, true);

  } else if (strcmp(flag->name(), "tryfromenv") == 0) {
    msg += ProcessFromenvLocked(FLAGS_tryfromenv, set_mode, false);
  }

  return msg;
}

void CommandLineFlagParser::ValidateAllFlags() {
  FlagRegistryLock frl(registry_);
  for (FlagRegistry::FlagConstIterator i = registry_->flags_.begin();
       i != registry_->flags_.end(); ++i) {
    if (!i->second->ValidateCurrent()) {
      // only set a message if one isn't already there.  (If there's
      // an error message, our job is done, even if it's not exactly
      // the same error.)
      if (error_flags_[i->second->name()].empty())
        error_flags_[i->second->name()] =
            string(kError) + "--" + i->second->name() +
            " must be set on the commandline"
            " (default value fails validation)\n";
    }
  }
}

bool CommandLineFlagParser::ReportErrors() {
  // error_flags_ indicates errors we saw while parsing.
  // But we ignore undefined-names if ok'ed by --undef_ok
  if (!FLAGS_undefok.empty()) {
    vector<string> flaglist;
    ParseFlagList(FLAGS_undefok.c_str(), &flaglist);
    for (size_t i = 0; i < flaglist.size(); ++i) {
      // We also deal with --no<flag>, in case the flagname was boolean
      const string no_version = string("no") + flaglist[i];
      if (undefined_names_.find(flaglist[i]) != undefined_names_.end()) {
        error_flags_[flaglist[i]] = "";    // clear the error message
      } else if (undefined_names_.find(no_version) != undefined_names_.end()) {
        error_flags_[no_version] = "";
      }
    }
  }
  // Likewise, if they decided to allow reparsing, all undefined-names
  // are ok; we just silently ignore them now, and hope that a future
  // parse will pick them up somehow.
  if (allow_command_line_reparsing) {
    for (map<string, string>::const_iterator it = undefined_names_.begin();
         it != undefined_names_.end();  ++it)
      error_flags_[it->first] = "";      // clear the error message
  }

  bool found_error = false;
  string error_message;
  for (map<string, string>::const_iterator it = error_flags_.begin();
       it != error_flags_.end(); ++it) {
    if (!it->second.empty()) {
      error_message.append(it->second.data(), it->second.size());
      found_error = true;
    }
  }
  if (found_error)
    ReportError(DO_NOT_DIE, "%s", error_message.c_str());
  return found_error;
}

string CommandLineFlagParser::ProcessOptionsFromStringLocked(
    const string& contentdata, FlagSettingMode set_mode) {
  string retval;
  const char* flagfile_contents = contentdata.c_str();
  bool flags_are_relevant = true;   // set to false when filenames don't match
  bool in_filename_section = false;

  const char* line_end = flagfile_contents;
  // We read this file a line at a time.
  for (; line_end; flagfile_contents = line_end + 1) {
    while (*flagfile_contents && isspace(*flagfile_contents))
      ++flagfile_contents;
    line_end = strchr(flagfile_contents, '\n');
    size_t len = line_end ? line_end - flagfile_contents
                          : strlen(flagfile_contents);
    string line(flagfile_contents, len);

    // Each line can be one of four things:
    // 1) A comment line -- we skip it
    // 2) An empty line -- we skip it
    // 3) A list of filenames -- starts a new filenames+flags section
    // 4) A --flag=value line -- apply if previous filenames match
    if (line.empty() || line[0] == '#') {
      // comment or empty line; just ignore

    } else if (line[0] == '-') {    // flag
      in_filename_section = false;  // instead, it was a flag-line
      if (!flags_are_relevant)      // skip this flag; applies to someone else
        continue;

      const char* name_and_val = line.c_str() + 1;    // skip the leading -
      if (*name_and_val == '-')
        name_and_val++;                               // skip second - too
      string key;
      const char* value;
      string error_message;
      CommandLineFlag* flag = registry_->SplitArgumentLocked(name_and_val,
                                                             &key, &value,
                                                             &error_message);
      // By API, errors parsing flagfile lines are silently ignored.
      if (flag == NULL) {
        // "WARNING: flagname '" + key + "' not found\n"
      } else if (value == NULL) {
        // "WARNING: flagname '" + key + "' missing a value\n"
      } else {
        retval += ProcessSingleOptionLocked(flag, value, set_mode);
      }

    } else {                        // a filename!
      if (!in_filename_section) {   // start over: assume filenames don't match
        in_filename_section = true;
        flags_are_relevant = false;
      }

      // Split the line up at spaces into glob-patterns
      const char* space = line.c_str();   // just has to be non-NULL
      for (const char* word = line.c_str(); *space; word = space+1) {
        if (flags_are_relevant)     // we can stop as soon as we match
          break;
        space = strchr(word, ' ');
        if (space == NULL)
          space = word + strlen(word);
        const string glob(word, space - word);
        // We try matching both against the full argv0 and basename(argv0)
        if (glob == ProgramInvocationName()       // small optimization
            || glob == ProgramInvocationShortName()
#ifdef HAVE_FNMATCH_H
            || fnmatch(glob.c_str(),
                       ProgramInvocationName(),
                       FNM_PATHNAME) == 0
            || fnmatch(glob.c_str(),
                       ProgramInvocationShortName(),
                       FNM_PATHNAME) == 0
#endif
            ) {
          flags_are_relevant = true;
        }
      }
    }
  }
  return retval;
}

// --------------------------------------------------------------------
// GetFromEnv()
// AddFlagValidator()
//    These are helper functions for routines like BoolFromEnv() and
//    RegisterFlagValidator, defined below.  They're defined here so
//    they can live in the unnamed namespace (which makes friendship
//    declarations for these classes possible).
// --------------------------------------------------------------------

template<typename T>
T GetFromEnv(const char *varname, const char* type, T dflt) {
  const char* const valstr = getenv(varname);
  if (!valstr)
    return dflt;
  FlagValue ifv(new T, type, true);
  if (!ifv.ParseFrom(valstr))
    ReportError(DIE, "ERROR: error parsing env variable '%s' with value '%s'\n",
                varname, valstr);
  return OTHER_VALUE_AS(ifv, T);
}

bool AddFlagValidator(const void* flag_ptr, ValidateFnProto validate_fn_proto) {
  // We want a lock around this routine, in case two threads try to
  // add a validator (hopefully the same one!) at once.  We could use
  // our own thread, but we need to loook at the registry anyway, so
  // we just steal that one.
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);
  // First, find the flag whose current-flag storage is 'flag'.
  // This is the CommandLineFlag whose current_->value_buffer_ == flag
  CommandLineFlag* flag = registry->FindFlagViaPtrLocked(flag_ptr);
  if (!flag) {
    LOG(WARNING) << "Ignoring RegisterValidateFunction() for flag pointer "
                 << flag_ptr << ": no flag found at that address";
    return false;
  } else if (validate_fn_proto == flag->validate_function()) {
    return true;    // ok to register the same function over and over again
  } else if (validate_fn_proto != NULL && flag->validate_function() != NULL) {
    LOG(WARNING) << "Ignoring RegisterValidateFunction() for flag '"
                 << flag->name() << "': validate-fn already registered";
    return false;
  } else {
    flag->validate_fn_proto_ = validate_fn_proto;
    return true;
  }
}

}  // end unnamed namespaces


// Now define the functions that are exported via the .h file

// --------------------------------------------------------------------
// FlagRegisterer
//    This class exists merely to have a global constructor (the
//    kind that runs before main(), that goes an initializes each
//    flag that's been declared.  Note that it's very important we
//    don't have a destructor that deletes flag_, because that would
//    cause us to delete current_storage/defvalue_storage as well,
//    which can cause a crash if anything tries to access the flag
//    values in a global destructor.
// --------------------------------------------------------------------

FlagRegisterer::FlagRegisterer(const char* name, const char* type,
                               const char* help, const char* filename,
                               void* current_storage, void* defvalue_storage) {
  if (help == NULL)
    help = "";
  // FlagValue expects the type-name to not include any namespace
  // components, so we get rid of those, if any.
  if (strchr(type, ':'))
    type = strrchr(type, ':') + 1;
  FlagValue* current = new FlagValue(current_storage, type, false);
  FlagValue* defvalue = new FlagValue(defvalue_storage, type, false);
  // Importantly, flag_ will never be deleted, so storage is always good.
  CommandLineFlag* flag = new CommandLineFlag(name, help, filename,
                                              current, defvalue);
  FlagRegistry::GlobalRegistry()->RegisterFlag(flag);   // default registry
}

// --------------------------------------------------------------------
// GetAllFlags()
//    The main way the FlagRegistry class exposes its data.  This
//    returns, as strings, all the info about all the flags in
//    the main registry, sorted first by filename they are defined
//    in, and then by flagname.
// --------------------------------------------------------------------

struct FilenameFlagnameCmp {
  bool operator()(const CommandLineFlagInfo& a,
                  const CommandLineFlagInfo& b) const {
    int cmp = strcmp(a.filename.c_str(), b.filename.c_str());
    if (cmp == 0)
      cmp = strcmp(a.name.c_str(), b.name.c_str());  // secondary sort key
    return cmp < 0;
  }
};

void GetAllFlags(vector<CommandLineFlagInfo>* OUTPUT) {
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  registry->Lock();
  for (FlagRegistry::FlagConstIterator i = registry->flags_.begin();
       i != registry->flags_.end(); ++i) {
    CommandLineFlagInfo fi;
    i->second->FillCommandLineFlagInfo(&fi);
    OUTPUT->push_back(fi);
  }
  registry->Unlock();
  // Now sort the flags, first by filename they occur in, then alphabetically
  sort(OUTPUT->begin(), OUTPUT->end(), FilenameFlagnameCmp());
}

// --------------------------------------------------------------------
// SetArgv()
// GetArgvs()
// GetArgv()
// GetArgv0()
// ProgramInvocationName()
// ProgramInvocationShortName()
// SetUsageMessage()
// ProgramUsage()
//    Functions to set and get argv.  Typically the setter is called
//    by ParseCommandLineFlags.  Also can get the ProgramUsage string,
//    set by SetUsageMessage.
// --------------------------------------------------------------------

// These values are not protected by a Mutex because they are normally
// set only once during program startup.
static const char* argv0 = "UNKNOWN";      // just the program name
static const char* cmdline = "";           // the entire command-line
static vector<string> argvs;
static uint32 argv_sum = 0;
static const char* program_usage = NULL;

void SetArgv(int argc, const char** argv) {
  static bool called_set_argv = false;
  if (called_set_argv)         // we already have an argv for you
    return;

  called_set_argv = true;

  assert(argc > 0);            // every program has at least a progname
  argv0 = strdup(argv[0]);     // small memory leak, but fn only called once
  assert(argv0);

  string cmdline_string;       // easier than doing strcats
  for (int i = 0; i < argc; i++) {
    if (i != 0) {
      cmdline_string += " ";
    }
    cmdline_string += argv[i];
    argvs.push_back(argv[i]);
  }
  cmdline = strdup(cmdline_string.c_str());  // another small memory leak
  assert(cmdline);

  // Compute a simple sum of all the chars in argv
  for (const char* c = cmdline; *c; c++)
    argv_sum += *c;
}

const vector<string>& GetArgvs() { return argvs; }
const char* GetArgv()            { return cmdline; }
const char* GetArgv0()           { return argv0; }
uint32 GetArgvSum()              { return argv_sum; }
const char* ProgramInvocationName() {             // like the GNU libc fn
  return GetArgv0();
}
const char* ProgramInvocationShortName() {        // like the GNU libc fn
  const char* slash = strrchr(argv0, '/');
#ifdef OS_WINDOWS
  if (!slash)  slash = strrchr(argv0, '\\');
#endif
  return slash ? slash + 1 : argv0;
}

void SetUsageMessage(const string& usage) {
  if (program_usage != NULL)
    ReportError(DIE, "ERROR: SetUsageMessage() called twice\n");
  program_usage = strdup(usage.c_str());      // small memory leak
}

const char* ProgramUsage() {
  if (program_usage) {
    return program_usage;
  }
  return "Warning: SetUsageMessage() never called";
}

// --------------------------------------------------------------------
// SetVersionString()
// VersionString()
// --------------------------------------------------------------------

static const char* version_string = NULL;

void SetVersionString(const string& version) {
  if (version_string != NULL)
    ReportError(DIE, "ERROR: SetVersionString() called twice\n");
  version_string = strdup(version.c_str());   // small memory leak
}

const char* VersionString() {
  return version_string ? version_string : "";
}


// --------------------------------------------------------------------
// GetCommandLineOption()
// GetCommandLineFlagInfo()
// GetCommandLineFlagInfoOrDie()
// SetCommandLineOption()
// SetCommandLineOptionWithMode()
//    The programmatic way to set a flag's value, using a string
//    for its name rather than the variable itself (that is,
//    SetCommandLineOption("foo", x) rather than FLAGS_foo = x).
//    There's also a bit more flexibility here due to the various
//    set-modes, but typically these are used when you only have
//    that flag's name as a string, perhaps at runtime.
//    All of these work on the default, global registry.
//       For GetCommandLineOption, return false if no such flag
//    is known, true otherwise.  We clear "value" if a suitable
//    flag is found.
// --------------------------------------------------------------------


bool GetCommandLineOption(const char* name, string* value) {
  if (NULL == name)
    return false;
  assert(value);

  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);
  CommandLineFlag* flag = registry->FindFlagLocked(name);
  if (flag == NULL) {
    return false;
  } else {
    *value = flag->current_value();
    return true;
  }
}

bool GetCommandLineFlagInfo(const char* name, CommandLineFlagInfo* OUTPUT) {
  if (NULL == name) return false;
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);
  CommandLineFlag* flag = registry->FindFlagLocked(name);
  if (flag == NULL) {
    return false;
  } else {
    assert(OUTPUT);
    flag->FillCommandLineFlagInfo(OUTPUT);
    return true;
  }
}

CommandLineFlagInfo GetCommandLineFlagInfoOrDie(const char* name) {
  CommandLineFlagInfo info;
  if (!GetCommandLineFlagInfo(name, &info)) {
    fprintf(stderr, "FATAL ERROR: flag name '%s' doesn't exist\n", name);
    gflags_exitfunc(1);    // almost certainly gflags_exitfunc()
  }
  return info;
}

string SetCommandLineOptionWithMode(const char* name, const char* value,
                                    FlagSettingMode set_mode) {
  string result;
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);
  CommandLineFlag* flag = registry->FindFlagLocked(name);
  if (flag) {
    CommandLineFlagParser parser(registry);
    result = parser.ProcessSingleOptionLocked(flag, value, set_mode);
    if (!result.empty()) {   // in the error case, we've already logged
      // Could consider logging this change
    }
  }
  // The API of this function is that we return empty string on error
  return result;
}

string SetCommandLineOption(const char* name, const char* value) {
  return SetCommandLineOptionWithMode(name, value, SET_FLAGS_VALUE);
}

// --------------------------------------------------------------------
// FlagSaver
// FlagSaverImpl
//    This class stores the states of all flags at construct time,
//    and restores all flags to that state at destruct time.
//    Its major implementation challenge is that it never modifies
//    pointers in the 'main' registry, so global FLAG_* vars always
//    point to the right place.
// --------------------------------------------------------------------

class FlagSaverImpl {
 public:
  // Constructs an empty FlagSaverImpl object.
  explicit FlagSaverImpl(FlagRegistry* main_registry)
      : main_registry_(main_registry) { }
  ~FlagSaverImpl() {
    // reclaim memory from each of our CommandLineFlags
    vector<CommandLineFlag*>::const_iterator it;
    for (it = backup_registry_.begin(); it != backup_registry_.end(); ++it)
      delete *it;
  }

  // Saves the flag states from the flag registry into this object.
  // It's an error to call this more than once.
  // Must be called when the registry mutex is not held.
  void SaveFromRegistry() {
    FlagRegistryLock frl(main_registry_);
    assert(backup_registry_.empty());   // call only once!
    for (FlagRegistry::FlagConstIterator it = main_registry_->flags_.begin();
         it != main_registry_->flags_.end();
         ++it) {
      const CommandLineFlag* main = it->second;
      // Sets up all the const variables in backup correctly
      CommandLineFlag* backup = new CommandLineFlag(
          main->name(), main->help(), main->filename(),
          main->current_->New(), main->defvalue_->New());
      // Sets up all the non-const variables in backup correctly
      backup->CopyFrom(*main);
      backup_registry_.push_back(backup);   // add it to a convenient list
    }
  }

  // Restores the saved flag states into the flag registry.  We
  // assume no flags were added or deleted from the registry since
  // the SaveFromRegistry; if they were, that's trouble!  Must be
  // called when the registry mutex is not held.
  void RestoreToRegistry() {
    FlagRegistryLock frl(main_registry_);
    vector<CommandLineFlag*>::const_iterator it;
    for (it = backup_registry_.begin(); it != backup_registry_.end(); ++it) {
      CommandLineFlag* main = main_registry_->FindFlagLocked((*it)->name());
      if (main != NULL) {       // if NULL, flag got deleted from registry(!)
        main->CopyFrom(**it);
      }
    }
  }

 private:
  FlagRegistry* const main_registry_;
  vector<CommandLineFlag*> backup_registry_;

  FlagSaverImpl(const FlagSaverImpl&);  // no copying!
  void operator=(const FlagSaverImpl&);
};

FlagSaver::FlagSaver()
    : impl_(new FlagSaverImpl(FlagRegistry::GlobalRegistry())) {
  impl_->SaveFromRegistry();
}

FlagSaver::~FlagSaver() {
  impl_->RestoreToRegistry();
  delete impl_;
}


// --------------------------------------------------------------------
// CommandlineFlagsIntoString()
// ReadFlagsFromString()
// AppendFlagsIntoFile()
// ReadFromFlagsFile()
//    These are mostly-deprecated routines that stick the
//    commandline flags into a file/string and read them back
//    out again.  I can see a use for CommandlineFlagsIntoString,
//    for creating a flagfile, but the rest don't seem that useful
//    -- some, I think, are a poor-man's attempt at FlagSaver --
//    and are included only until we can delete them from callers.
//    Note they don't save --flagfile flags (though they do save
//    the result of having called the flagfile, of course).
// --------------------------------------------------------------------

static string TheseCommandlineFlagsIntoString(
    const vector<CommandLineFlagInfo>& flags) {
  vector<CommandLineFlagInfo>::const_iterator i;

  size_t retval_space = 0;
  for (i = flags.begin(); i != flags.end(); ++i) {
    // An (over)estimate of how much space it will take to print this flag
    retval_space += i->name.length() + i->current_value.length() + 5;
  }

  string retval;
  retval.reserve(retval_space);
  for (i = flags.begin(); i != flags.end(); ++i) {
    retval += "--";
    retval += i->name;
    retval += "=";
    retval += i->current_value;
    retval += "\n";
  }
  return retval;
}

string CommandlineFlagsIntoString() {
  vector<CommandLineFlagInfo> sorted_flags;
  GetAllFlags(&sorted_flags);
  return TheseCommandlineFlagsIntoString(sorted_flags);
}

bool ReadFlagsFromString(const string& flagfilecontents,
                         const char* /*prog_name*/,  // TODO(csilvers): nix this
                         bool errors_are_fatal) {
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagSaverImpl saved_states(registry);
  saved_states.SaveFromRegistry();

  CommandLineFlagParser parser(registry);
  registry->Lock();
  parser.ProcessOptionsFromStringLocked(flagfilecontents, SET_FLAGS_VALUE);
  registry->Unlock();
  // Should we handle --help and such when reading flags from a string?  Sure.
  //HandleCommandLineHelpFlags();
  if (parser.ReportErrors()) {
    // Error.  Restore all global flags to their previous values.
    if (errors_are_fatal)
      gflags_exitfunc(1);
    saved_states.RestoreToRegistry();
    return false;
  }
  return true;
}

// TODO(csilvers): nix prog_name in favor of ProgramInvocationShortName()
bool AppendFlagsIntoFile(const string& filename, const char *prog_name) {
  FILE *fp = fopen(filename.c_str(), "a");
  if (!fp) {
    return false;
  }

  if (prog_name)
    fprintf(fp, "%s\n", prog_name);

  vector<CommandLineFlagInfo> flags;
  GetAllFlags(&flags);
  // But we don't want --flagfile, which leads to weird recursion issues
  vector<CommandLineFlagInfo>::iterator i;
  for (i = flags.begin(); i != flags.end(); ++i) {
    if (strcmp(i->name.c_str(), "flagfile") == 0) {
      flags.erase(i);
      break;
    }
  }
  fprintf(fp, "%s", TheseCommandlineFlagsIntoString(flags).c_str());

  fclose(fp);
  return true;
}

bool ReadFromFlagsFile(const string& filename, const char* prog_name,
                       bool errors_are_fatal) {
  return ReadFlagsFromString(ReadFileIntoString(filename.c_str()),
                             prog_name, errors_are_fatal);
}


// --------------------------------------------------------------------
// BoolFromEnv()
// Int32FromEnv()
// Int64FromEnv()
// Uint64FromEnv()
// DoubleFromEnv()
// StringFromEnv()
//    Reads the value from the environment and returns it.
//    We use an FlagValue to make the parsing easy.
//    Example usage:
//       DEFINE_bool(myflag, BoolFromEnv("MYFLAG_DEFAULT", false), "whatever");
// --------------------------------------------------------------------

bool BoolFromEnv(const char *v, bool dflt) {
  return GetFromEnv(v, "bool", dflt);
}
int32 Int32FromEnv(const char *v, int32 dflt) {
  return GetFromEnv(v, "int32", dflt);
}
int64 Int64FromEnv(const char *v, int64 dflt)    {
  return GetFromEnv(v, "int64", dflt);
}
uint64 Uint64FromEnv(const char *v, uint64 dflt) {
  return GetFromEnv(v, "uint64", dflt);
}
double DoubleFromEnv(const char *v, double dflt) {
  return GetFromEnv(v, "double", dflt);
}
const char *StringFromEnv(const char *varname, const char *dflt) {
  const char* const val = getenv(varname);
  return val ? val : dflt;
}


// --------------------------------------------------------------------
// RegisterFlagValidator()
//    RegisterFlagValidator() is the function that clients use to
//    'decorate' a flag with a validation function.  Once this is
//    done, every time the flag is set (including when the flag
//    is parsed from argv), the validator-function is called.
//       These functions return true if the validator was added
//    successfully, or false if not: the flag already has a validator,
//    (only one allowed per flag), the 1st arg isn't a flag, etc.
//       This function is not thread-safe.
// --------------------------------------------------------------------

bool RegisterFlagValidator(const bool* flag,
                           bool (*validate_fn)(const char*, bool)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}
bool RegisterFlagValidator(const int32* flag,
                           bool (*validate_fn)(const char*, int32)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}
bool RegisterFlagValidator(const int64* flag,
                           bool (*validate_fn)(const char*, int64)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}
bool RegisterFlagValidator(const uint64* flag,
                           bool (*validate_fn)(const char*, uint64)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}
bool RegisterFlagValidator(const double* flag,
                           bool (*validate_fn)(const char*, double)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}
bool RegisterFlagValidator(const string* flag,
                           bool (*validate_fn)(const char*, const string&)) {
  return AddFlagValidator(flag, reinterpret_cast<ValidateFnProto>(validate_fn));
}


// --------------------------------------------------------------------
// ParseCommandLineFlags()
// ParseCommandLineNonHelpFlags()
// HandleCommandLineHelpFlags()
//    This is the main function called from main(), to actually
//    parse the commandline.  It modifies argc and argv as described
//    at the top of gflags.h.  You can also divide this
//    function into two parts, if you want to do work between
//    the parsing of the flags and the printing of any help output.
// --------------------------------------------------------------------

static uint32 ParseCommandLineFlagsInternal(int* argc, char*** argv,
                                            bool remove_flags, bool do_report) {
  SetArgv(*argc, const_cast<const char**>(*argv));    // save it for later

  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  CommandLineFlagParser parser(registry);

  // When we parse the commandline flags, we'll handle --flagfile,
  // --tryfromenv, etc. as we see them (since flag-evaluation order
  // may be important).  But sometimes apps set FLAGS_tryfromenv/etc.
  // manually before calling ParseCommandLineFlags.  We want to evaluate
  // those too, as if they were the first flags on the commandline.
  registry->Lock();
  parser.ProcessFlagfileLocked(FLAGS_flagfile, SET_FLAGS_VALUE);
  // Last arg here indicates whether flag-not-found is a fatal error or not
  parser.ProcessFromenvLocked(FLAGS_fromenv, SET_FLAGS_VALUE, true);
  parser.ProcessFromenvLocked(FLAGS_tryfromenv, SET_FLAGS_VALUE, false);
  registry->Unlock();

  // Now get the flags specified on the commandline
  const int r = parser.ParseNewCommandLineFlags(argc, argv, remove_flags);

  //if (do_report)
    //HandleCommandLineHelpFlags();   // may cause us to exit on --help, etc.

  // See if any of the unset flags fail their validation checks
  parser.ValidateAllFlags();

  if (parser.ReportErrors())        // may cause us to exit on illegal flags
    gflags_exitfunc(1);
  return r;
}

uint32 ParseCommandLineFlags(int* argc, char*** argv, bool remove_flags) {
  return ParseCommandLineFlagsInternal(argc, argv, remove_flags, true);
}

uint32 ParseCommandLineNonHelpFlags(int* argc, char*** argv,
                                    bool remove_flags) {
  return ParseCommandLineFlagsInternal(argc, argv, remove_flags, false);
}

// --------------------------------------------------------------------
// AllowCommandLineReparsing()
// ReparseCommandLineNonHelpFlags()
//    This is most useful for shared libraries.  The idea is if
//    a flag is defined in a shared library that is dlopen'ed
//    sometime after main(), you can ParseCommandLineFlags before
//    the dlopen, then ReparseCommandLineNonHelpFlags() after the
//    dlopen, to get the new flags.  But you have to explicitly
//    Allow() it; otherwise, you get the normal default behavior
//    of unrecognized flags calling a fatal error.
// TODO(csilvers): this isn't used.  Just delete it?
// --------------------------------------------------------------------

void AllowCommandLineReparsing() {
  allow_command_line_reparsing = true;
}

void ReparseCommandLineNonHelpFlags() {
  // We make a copy of argc and argv to pass in
  const vector<string>& argvs = GetArgvs();
  int tmp_argc = static_cast<int>(argvs.size());
  char** tmp_argv = new char* [tmp_argc + 1];
  for (int i = 0; i < tmp_argc; ++i)
    tmp_argv[i] = strdup(argvs[i].c_str());   // TODO(csilvers): don't dup

  ParseCommandLineNonHelpFlags(&tmp_argc, &tmp_argv, false);

  for (int i = 0; i < tmp_argc; ++i)
    free(tmp_argv[i]);
  delete[] tmp_argv;
}

void ShutDownCommandLineFlags() {
  FlagRegistry::DeleteGlobalRegistry();
}

_END_GOOGLE_NAMESPACE_
