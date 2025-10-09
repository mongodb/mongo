/*
******************************************************************************
* Copyright (C) 1999-2016, International Business Machines Corporation and
* others. All Rights Reserved.
******************************************************************************
*
* File unistr.cpp
*
* Modification History:
*
*   Date        Name        Description
*   09/25/98    stephen     Creation.
*   04/20/99    stephen     Overhauled per 4/16 code review.
*   07/09/99    stephen     Renamed {hi,lo},{byte,word} to icu_X for HP/UX
*   11/18/99    aliu        Added handleReplaceBetween() to make inherit from
*                           Replaceable.
*   06/25/01    grhoten     Removed the dependency on iostream
******************************************************************************
*/

#include "unicode/utypes.h"
#include "unicode/appendable.h"
#include "unicode/putil.h"
#include "cstring.h"
#include "cmemory.h"
#include "unicode/ustring.h"
#include "unicode/unistr.h"
#include "unicode/utf.h"
#include "unicode/utf16.h"
#include "uelement.h"
#include "ustr_imp.h"
#include "umutex.h"
#include "uassert.h"

#if 0

#include <iostream>
using namespace std;

//DEBUGGING
void
print(const UnicodeString& s,
      const char *name)
{
  UChar c;
  cout << name << ":|";
  for(int i = 0; i < s.length(); ++i) {
    c = s[i];
    if(c>= 0x007E || c < 0x0020)
      cout << "[0x" << hex << s[i] << "]";
    else
      cout << (char) s[i];
  }
  cout << '|' << endl;
}

void
print(const UChar *s,
      int32_t len,
      const char *name)
{
  UChar c;
  cout << name << ":|";
  for(int i = 0; i < len; ++i) {
    c = s[i];
    if(c>= 0x007E || c < 0x0020)
      cout << "[0x" << hex << s[i] << "]";
    else
      cout << (char) s[i];
  }
  cout << '|' << endl;
}
// END DEBUGGING
#endif

// Local function definitions for now

// need to copy areas that may overlap
static
inline void
us_arrayCopy(const UChar *src, int32_t srcStart,
         UChar *dst, int32_t dstStart, int32_t count)
{
  if(count>0) {
    uprv_memmove(dst+dstStart, src+srcStart, (size_t)(count*sizeof(*src)));
  }
}

// u_unescapeAt() callback to get a UChar from a UnicodeString
U_CDECL_BEGIN
static UChar U_CALLCONV
UnicodeString_charAt(int32_t offset, void *context) {
    return ((icu::UnicodeString*) context)->charAt(offset);
}
U_CDECL_END

U_NAMESPACE_BEGIN

/* The Replaceable virtual destructor can't be defined in the header
   due to how AIX works with multiple definitions of virtual functions.
*/
Replaceable::~Replaceable() {}

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(UnicodeString)

UnicodeString U_EXPORT2
operator+ (const UnicodeString &s1, const UnicodeString &s2) {
    return
        UnicodeString(s1.length()+s2.length()+1, (UChar32)0, 0).
            append(s1).
                append(s2);
}

//========================================
// Reference Counting functions, put at top of file so that optimizing compilers
//                               have a chance to automatically inline.
//========================================

void
UnicodeString::addRef() {
  umtx_atomic_inc((u_atomic_int32_t *)fUnion.fFields.fArray - 1);
}

int32_t
UnicodeString::removeRef() {
  return umtx_atomic_dec((u_atomic_int32_t *)fUnion.fFields.fArray - 1);
}

int32_t
UnicodeString::refCount() const {
  return umtx_loadAcquire(*((u_atomic_int32_t *)fUnion.fFields.fArray - 1));
}

void
UnicodeString::releaseArray() {
  if((fUnion.fFields.fLengthAndFlags & kRefCounted) && removeRef() == 0) {
    uprv_free((int32_t *)fUnion.fFields.fArray - 1);
  }
}



//========================================
// Constructors
//========================================

// The default constructor is inline in unistr.h.

UnicodeString::UnicodeString(int32_t capacity, UChar32 c, int32_t count) {
  fUnion.fFields.fLengthAndFlags = 0;
  if(count <= 0 || (uint32_t)c > 0x10ffff) {
    // just allocate and do not do anything else
    allocate(capacity);
  } else {
    // count > 0, allocate and fill the new string with count c's
    int32_t unitCount = U16_LENGTH(c), length = count * unitCount;
    if(capacity < length) {
      capacity = length;
    }
    if(allocate(capacity)) {
      UChar *array = getArrayStart();
      int32_t i = 0;

      // fill the new string with c
      if(unitCount == 1) {
        // fill with length UChars
        while(i < length) {
          array[i++] = (UChar)c;
        }
      } else {
        // get the code units for c
        UChar units[U16_MAX_LENGTH];
        U16_APPEND_UNSAFE(units, i, c);

        // now it must be i==unitCount
        i = 0;

        // for Unicode, unitCount can only be 1, 2, 3, or 4
        // 1 is handled above
        while(i < length) {
          int32_t unitIdx = 0;
          while(unitIdx < unitCount) {
            array[i++]=units[unitIdx++];
          }
        }
      }
    }
    setLength(length);
  }
}

UnicodeString::UnicodeString(UChar ch) {
  fUnion.fFields.fLengthAndFlags = kLength1 | kShortString;
  fUnion.fStackFields.fBuffer[0] = ch;
}

UnicodeString::UnicodeString(UChar32 ch) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  int32_t i = 0;
  UBool isError = FALSE;
  U16_APPEND(fUnion.fStackFields.fBuffer, i, US_STACKBUF_SIZE, ch, isError);
  // We test isError so that the compiler does not complain that we don't.
  // If isError then i==0 which is what we want anyway.
  if(!isError) {
    setShortLength(i);
  }
}

UnicodeString::UnicodeString(const UChar *text) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  doAppend(text, 0, -1);
}

UnicodeString::UnicodeString(const UChar *text,
                             int32_t textLength) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  doAppend(text, 0, textLength);
}

UnicodeString::UnicodeString(UBool isTerminated,
                             const UChar *text,
                             int32_t textLength) {
  fUnion.fFields.fLengthAndFlags = kReadonlyAlias;
  if(text == NULL) {
    // treat as an empty string, do not alias
    setToEmpty();
  } else if(textLength < -1 ||
            (textLength == -1 && !isTerminated) ||
            (textLength >= 0 && isTerminated && text[textLength] != 0)
  ) {
    setToBogus();
  } else {
    if(textLength == -1) {
      // text is terminated, or else it would have failed the above test
      textLength = u_strlen(text);
    }
    setArray((UChar *)text, textLength, isTerminated ? textLength + 1 : textLength);
  }
}

UnicodeString::UnicodeString(UChar *buff,
                             int32_t buffLength,
                             int32_t buffCapacity) {
  fUnion.fFields.fLengthAndFlags = kWritableAlias;
  if(buff == NULL) {
    // treat as an empty string, do not alias
    setToEmpty();
  } else if(buffLength < -1 || buffCapacity < 0 || buffLength > buffCapacity) {
    setToBogus();
  } else {
    if(buffLength == -1) {
      // fLength = u_strlen(buff); but do not look beyond buffCapacity
      const UChar *p = buff, *limit = buff + buffCapacity;
      while(p != limit && *p != 0) {
        ++p;
      }
      buffLength = (int32_t)(p - buff);
    }
    setArray(buff, buffLength, buffCapacity);
  }
}

UnicodeString::UnicodeString(const char *src, int32_t length, EInvariant) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  if(src==NULL) {
    // treat as an empty string
  } else {
    if(length<0) {
      length=(int32_t)uprv_strlen(src);
    }
    if(cloneArrayIfNeeded(length, length, FALSE)) {
      u_charsToUChars(src, getArrayStart(), length);
      setLength(length);
    } else {
      setToBogus();
    }
  }
}

#if U_CHARSET_IS_UTF8

UnicodeString::UnicodeString(const char *codepageData) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  if(codepageData != 0) {
    setToUTF8(codepageData);
  }
}

UnicodeString::UnicodeString(const char *codepageData, int32_t dataLength) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  // if there's nothing to convert, do nothing
  if(codepageData == 0 || dataLength == 0 || dataLength < -1) {
    return;
  }
  if(dataLength == -1) {
    dataLength = (int32_t)uprv_strlen(codepageData);
  }
  setToUTF8(StringPiece(codepageData, dataLength));
}

// else see unistr_cnv.cpp
#endif

UnicodeString::UnicodeString(const UnicodeString& that) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  copyFrom(that);
}

#if U_HAVE_RVALUE_REFERENCES
UnicodeString::UnicodeString(UnicodeString &&src) U_NOEXCEPT {
  fUnion.fFields.fLengthAndFlags = kShortString;
  moveFrom(src);
}
#endif

UnicodeString::UnicodeString(const UnicodeString& that,
                             int32_t srcStart) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  setTo(that, srcStart);
}

UnicodeString::UnicodeString(const UnicodeString& that,
                             int32_t srcStart,
                             int32_t srcLength) {
  fUnion.fFields.fLengthAndFlags = kShortString;
  setTo(that, srcStart, srcLength);
}

// Replaceable base class clone() default implementation, does not clone
Replaceable *
Replaceable::clone() const {
  return NULL;
}

// UnicodeString overrides clone() with a real implementation
Replaceable *
UnicodeString::clone() const {
  return new UnicodeString(*this);
}

//========================================
// array allocation
//========================================

UBool
UnicodeString::allocate(int32_t capacity) {
  if(capacity <= US_STACKBUF_SIZE) {
    fUnion.fFields.fLengthAndFlags = kShortString;
  } else {
    // count bytes for the refCounter and the string capacity, and
    // round up to a multiple of 16; then divide by 4 and allocate int32_t's
    // to be safely aligned for the refCount
    // the +1 is for the NUL terminator, to avoid reallocation in getTerminatedBuffer()
    int32_t words = (int32_t)(((sizeof(int32_t) + (capacity + 1) * U_SIZEOF_UCHAR + 15) & ~15) >> 2);
    int32_t *array = (int32_t*) uprv_malloc( sizeof(int32_t) * words );
    if(array != 0) {
      // set initial refCount and point behind the refCount
      *array++ = 1;

      // have fArray point to the first UChar
      fUnion.fFields.fArray = (UChar *)array;
      fUnion.fFields.fCapacity = (int32_t)((words - 1) * (sizeof(int32_t) / U_SIZEOF_UCHAR));
      fUnion.fFields.fLengthAndFlags = kLongString;
    } else {
      fUnion.fFields.fLengthAndFlags = kIsBogus;
      fUnion.fFields.fArray = 0;
      fUnion.fFields.fCapacity = 0;
      return FALSE;
    }
  }
  return TRUE;
}

//========================================
// Destructor
//========================================

#ifdef UNISTR_COUNT_FINAL_STRING_LENGTHS
static u_atomic_int32_t finalLengthCounts[0x400];  // UnicodeString::kMaxShortLength+1
static u_atomic_int32_t beyondCount(0);

U_CAPI void unistr_printLengths() {
  int32_t i;
  for(i = 0; i <= 59; ++i) {
    printf("%2d,  %9d\n", i, (int32_t)finalLengthCounts[i]);
  }
  int32_t beyond = beyondCount;
  for(; i < UPRV_LENGTHOF(finalLengthCounts); ++i) {
    beyond += finalLengthCounts[i];
  }
  printf(">59, %9d\n", beyond);
}
#endif

UnicodeString::~UnicodeString()
{
#ifdef UNISTR_COUNT_FINAL_STRING_LENGTHS
  // Count lengths of strings at the end of their lifetime.
  // Useful for discussion of a desirable stack buffer size.
  // Count the contents length, not the optional NUL terminator nor further capacity.
  // Ignore open-buffer strings and strings which alias external storage.
  if((fUnion.fFields.fLengthAndFlags&(kOpenGetBuffer|kReadonlyAlias|kWritableAlias)) == 0) {
    if(hasShortLength()) {
      umtx_atomic_inc(finalLengthCounts + getShortLength());
    } else {
      umtx_atomic_inc(&beyondCount);
    }
  }
#endif

  releaseArray();
}

//========================================
// Factory methods
//========================================

UnicodeString UnicodeString::fromUTF8(const StringPiece &utf8) {
  UnicodeString result;
  result.setToUTF8(utf8);
  return result;
}

UnicodeString UnicodeString::fromUTF32(const UChar32 *utf32, int32_t length) {
  UnicodeString result;
  int32_t capacity;
  // Most UTF-32 strings will be BMP-only and result in a same-length
  // UTF-16 string. We overestimate the capacity just slightly,
  // just in case there are a few supplementary characters.
  if(length <= US_STACKBUF_SIZE) {
    capacity = US_STACKBUF_SIZE;
  } else {
    capacity = length + (length >> 4) + 4;
  }
  do {
    UChar *utf16 = result.getBuffer(capacity);
    int32_t length16;
    UErrorCode errorCode = U_ZERO_ERROR;
    u_strFromUTF32WithSub(utf16, result.getCapacity(), &length16,
        utf32, length,
        0xfffd,  // Substitution character.
        NULL,    // Don't care about number of substitutions.
        &errorCode);
    result.releaseBuffer(length16);
    if(errorCode == U_BUFFER_OVERFLOW_ERROR) {
      capacity = length16 + 1;  // +1 for the terminating NUL.
      continue;
    } else if(U_FAILURE(errorCode)) {
      result.setToBogus();
    }
    break;
  } while(TRUE);
  return result;
}

//========================================
// Assignment
//========================================

UnicodeString &
UnicodeString::operator=(const UnicodeString &src) {
  return copyFrom(src);
}

UnicodeString &
UnicodeString::fastCopyFrom(const UnicodeString &src) {
  return copyFrom(src, TRUE);
}

UnicodeString &
UnicodeString::copyFrom(const UnicodeString &src, UBool fastCopy) {
  // if assigning to ourselves, do nothing
  if(this == &src) {
    return *this;
  }

  // is the right side bogus?
  if(src.isBogus()) {
    setToBogus();
    return *this;
  }

  // delete the current contents
  releaseArray();

  if(src.isEmpty()) {
    // empty string - use the stack buffer
    setToEmpty();
    return *this;
  }

  // fLength>0 and not an "open" src.getBuffer(minCapacity)
  fUnion.fFields.fLengthAndFlags = src.fUnion.fFields.fLengthAndFlags;
  switch(src.fUnion.fFields.fLengthAndFlags & kAllStorageFlags) {
  case kShortString:
    // short string using the stack buffer, do the same
    uprv_memcpy(fUnion.fStackFields.fBuffer, src.fUnion.fStackFields.fBuffer,
                getShortLength() * U_SIZEOF_UCHAR);
    break;
  case kLongString:
    // src uses a refCounted string buffer, use that buffer with refCount
    // src is const, use a cast - we don't actually change it
    ((UnicodeString &)src).addRef();
    // copy all fields, share the reference-counted buffer
    fUnion.fFields.fArray = src.fUnion.fFields.fArray;
    fUnion.fFields.fCapacity = src.fUnion.fFields.fCapacity;
    if(!hasShortLength()) {
      fUnion.fFields.fLength = src.fUnion.fFields.fLength;
    }
    break;
  case kReadonlyAlias:
    if(fastCopy) {
      // src is a readonly alias, do the same
      // -> maintain the readonly alias as such
      fUnion.fFields.fArray = src.fUnion.fFields.fArray;
      fUnion.fFields.fCapacity = src.fUnion.fFields.fCapacity;
      if(!hasShortLength()) {
        fUnion.fFields.fLength = src.fUnion.fFields.fLength;
      }
      break;
    }
    // else if(!fastCopy) fall through to case kWritableAlias
    // -> allocate a new buffer and copy the contents
    U_FALLTHROUGH;
  case kWritableAlias: {
    // src is a writable alias; we make a copy of that instead
    int32_t srcLength = src.length();
    if(allocate(srcLength)) {
      uprv_memcpy(getArrayStart(), src.getArrayStart(), srcLength * U_SIZEOF_UCHAR);
      setLength(srcLength);
      break;
    }
    // if there is not enough memory, then fall through to setting to bogus
    U_FALLTHROUGH;
  }
  default:
    // if src is bogus, set ourselves to bogus
    // do not call setToBogus() here because fArray and flags are not consistent here
    fUnion.fFields.fLengthAndFlags = kIsBogus;
    fUnion.fFields.fArray = 0;
    fUnion.fFields.fCapacity = 0;
    break;
  }

  return *this;
}

UnicodeString &UnicodeString::moveFrom(UnicodeString &src) U_NOEXCEPT {
  // No explicit check for self move assignment, consistent with standard library.
  // Self move assignment causes no crash nor leak but might make the object bogus.
  releaseArray();
  copyFieldsFrom(src, TRUE);
  return *this;
}

// Same as moveFrom() except without memory management.
void UnicodeString::copyFieldsFrom(UnicodeString &src, UBool setSrcToBogus) U_NOEXCEPT {
  int16_t lengthAndFlags = fUnion.fFields.fLengthAndFlags = src.fUnion.fFields.fLengthAndFlags;
  if(lengthAndFlags & kUsingStackBuffer) {
    // Short string using the stack buffer, copy the contents.
    // Check for self assignment to prevent "overlap in memcpy" warnings,
    // although it should be harmless to copy a buffer to itself exactly.
    if(this != &src) {
      uprv_memcpy(fUnion.fStackFields.fBuffer, src.fUnion.fStackFields.fBuffer,
                  getShortLength() * U_SIZEOF_UCHAR);
    }
  } else {
    // In all other cases, copy all fields.
    fUnion.fFields.fArray = src.fUnion.fFields.fArray;
    fUnion.fFields.fCapacity = src.fUnion.fFields.fCapacity;
    if(!hasShortLength()) {
      fUnion.fFields.fLength = src.fUnion.fFields.fLength;
    }
    if(setSrcToBogus) {
      // Set src to bogus without releasing any memory.
      src.fUnion.fFields.fLengthAndFlags = kIsBogus;
      src.fUnion.fFields.fArray = NULL;
      src.fUnion.fFields.fCapacity = 0;
    }
  }
}

void UnicodeString::swap(UnicodeString &other) U_NOEXCEPT {
  UnicodeString temp;  // Empty short string: Known not to need releaseArray().
  // Copy fields without resetting source values in between.
  temp.copyFieldsFrom(*this, FALSE);
  this->copyFieldsFrom(other, FALSE);
  other.copyFieldsFrom(temp, FALSE);
  // Set temp to an empty string so that other's memory is not released twice.
  temp.fUnion.fFields.fLengthAndFlags = kShortString;
}

//========================================
// Miscellaneous operations
//========================================

UnicodeString UnicodeString::unescape() const {
    UnicodeString result(length(), (UChar32)0, (int32_t)0); // construct with capacity
    if (result.isBogus()) {
        return result;
    }
    const UChar *array = getBuffer();
    int32_t len = length();
    int32_t prev = 0;
    for (int32_t i=0;;) {
        if (i == len) {
            result.append(array, prev, len - prev);
            break;
        }
        if (array[i++] == 0x5C /*'\\'*/) {
            result.append(array, prev, (i - 1) - prev);
            UChar32 c = unescapeAt(i); // advances i
            if (c < 0) {
                result.remove(); // return empty string
                break; // invalid escape sequence
            }
            result.append(c);
            prev = i;
        }
    }
    return result;
}

UChar32 UnicodeString::unescapeAt(int32_t &offset) const {
    return u_unescapeAt(UnicodeString_charAt, &offset, length(), (void*)this);
}

//========================================
// Read-only implementation
//========================================
UBool
UnicodeString::doEquals(const UnicodeString &text, int32_t len) const {
  // Requires: this & text not bogus and have same lengths.
  // Byte-wise comparison works for equality regardless of endianness.
  return uprv_memcmp(getArrayStart(), text.getArrayStart(), len * U_SIZEOF_UCHAR) == 0;
}

int8_t
UnicodeString::doCompare( int32_t start,
              int32_t length,
              const UChar *srcChars,
              int32_t srcStart,
              int32_t srcLength) const
{
  // compare illegal string values
  if(isBogus()) {
    return -1;
  }
  
  // pin indices to legal values
  pinIndices(start, length);

  if(srcChars == NULL) {
    // treat const UChar *srcChars==NULL as an empty string
    return length == 0 ? 0 : 1;
  }

  // get the correct pointer
  const UChar *chars = getArrayStart();

  chars += start;
  srcChars += srcStart;

  int32_t minLength;
  int8_t lengthResult;

  // get the srcLength if necessary
  if(srcLength < 0) {
    srcLength = u_strlen(srcChars + srcStart);
  }

  // are we comparing different lengths?
  if(length != srcLength) {
    if(length < srcLength) {
      minLength = length;
      lengthResult = -1;
    } else {
      minLength = srcLength;
      lengthResult = 1;
    }
  } else {
    minLength = length;
    lengthResult = 0;
  }

  /*
   * note that uprv_memcmp() returns an int but we return an int8_t;
   * we need to take care not to truncate the result -
   * one way to do this is to right-shift the value to
   * move the sign bit into the lower 8 bits and making sure that this
   * does not become 0 itself
   */

  if(minLength > 0 && chars != srcChars) {
    int32_t result;

#   if U_IS_BIG_ENDIAN 
      // big-endian: byte comparison works
      result = uprv_memcmp(chars, srcChars, minLength * sizeof(UChar));
      if(result != 0) {
        return (int8_t)(result >> 15 | 1);
      }
#   else
      // little-endian: compare UChar units
      do {
        result = ((int32_t)*(chars++) - (int32_t)*(srcChars++));
        if(result != 0) {
          return (int8_t)(result >> 15 | 1);
        }
      } while(--minLength > 0);
#   endif
  }
  return lengthResult;
}

/* String compare in code point order - doCompare() compares in code unit order. */
int8_t
UnicodeString::doCompareCodePointOrder(int32_t start,
                                       int32_t length,
                                       const UChar *srcChars,
                                       int32_t srcStart,
                                       int32_t srcLength) const
{
  // compare illegal string values
  // treat const UChar *srcChars==NULL as an empty string
  if(isBogus()) {
    return -1;
  }

  // pin indices to legal values
  pinIndices(start, length);

  if(srcChars == NULL) {
    srcStart = srcLength = 0;
  }

  int32_t diff = uprv_strCompare(getArrayStart() + start, length, (srcChars!=NULL)?(srcChars + srcStart):NULL, srcLength, FALSE, TRUE);
  /* translate the 32-bit result into an 8-bit one */
  if(diff!=0) {
    return (int8_t)(diff >> 15 | 1);
  } else {
    return 0;
  }
}

int32_t
UnicodeString::getLength() const {
    return length();
}

UChar
UnicodeString::getCharAt(int32_t offset) const {
  return charAt(offset);
}

UChar32
UnicodeString::getChar32At(int32_t offset) const {
  return char32At(offset);
}

UChar32
UnicodeString::char32At(int32_t offset) const
{
  int32_t len = length();
  if((uint32_t)offset < (uint32_t)len) {
    const UChar *array = getArrayStart();
    UChar32 c;
    U16_GET(array, 0, offset, len, c);
    return c;
  } else {
    return kInvalidUChar;
  }
}

int32_t
UnicodeString::getChar32Start(int32_t offset) const {
  if((uint32_t)offset < (uint32_t)length()) {
    const UChar *array = getArrayStart();
    U16_SET_CP_START(array, 0, offset);
    return offset;
  } else {
    return 0;
  }
}

int32_t
UnicodeString::getChar32Limit(int32_t offset) const {
  int32_t len = length();
  if((uint32_t)offset < (uint32_t)len) {
    const UChar *array = getArrayStart();
    U16_SET_CP_LIMIT(array, 0, offset, len);
    return offset;
  } else {
    return len;
  }
}

int32_t
UnicodeString::countChar32(int32_t start, int32_t length) const {
  pinIndices(start, length);
  // if(isBogus()) then fArray==0 and start==0 - u_countChar32() checks for NULL
  return u_countChar32(getArrayStart()+start, length);
}

UBool
UnicodeString::hasMoreChar32Than(int32_t start, int32_t length, int32_t number) const {
  pinIndices(start, length);
  // if(isBogus()) then fArray==0 and start==0 - u_strHasMoreChar32Than() checks for NULL
  return u_strHasMoreChar32Than(getArrayStart()+start, length, number);
}

int32_t
UnicodeString::moveIndex32(int32_t index, int32_t delta) const {
  // pin index
  int32_t len = length();
  if(index<0) {
    index=0;
  } else if(index>len) {
    index=len;
  }

  const UChar *array = getArrayStart();
  if(delta>0) {
    U16_FWD_N(array, index, len, delta);
  } else {
    U16_BACK_N(array, 0, index, -delta);
  }

  return index;
}

void
UnicodeString::doExtract(int32_t start,
             int32_t length,
             UChar *dst,
             int32_t dstStart) const
{
  // pin indices to legal values
  pinIndices(start, length);

  // do not copy anything if we alias dst itself
  const UChar *array = getArrayStart();
  if(array + start != dst + dstStart) {
    us_arrayCopy(array, start, dst, dstStart, length);
  }
}

int32_t
UnicodeString::extract(UChar *dest, int32_t destCapacity,
                       UErrorCode &errorCode) const {
  int32_t len = length();
  if(U_SUCCESS(errorCode)) {
    if(isBogus() || destCapacity<0 || (destCapacity>0 && dest==0)) {
      errorCode=U_ILLEGAL_ARGUMENT_ERROR;
    } else {
      const UChar *array = getArrayStart();
      if(len>0 && len<=destCapacity && array!=dest) {
        uprv_memcpy(dest, array, len*U_SIZEOF_UCHAR);
      }
      return u_terminateUChars(dest, destCapacity, len, &errorCode);
    }
  }

  return len;
}

int32_t
UnicodeString::extract(int32_t start,
                       int32_t length,
                       char *target,
                       int32_t targetCapacity,
                       enum EInvariant) const
{
  // if the arguments are illegal, then do nothing
  if(targetCapacity < 0 || (targetCapacity > 0 && target == NULL)) {
    return 0;
  }

  // pin the indices to legal values
  pinIndices(start, length);

  if(length <= targetCapacity) {
    u_UCharsToChars(getArrayStart() + start, target, length);
  }
  UErrorCode status = U_ZERO_ERROR;
  return u_terminateChars(target, targetCapacity, length, &status);
}

UnicodeString
UnicodeString::tempSubString(int32_t start, int32_t len) const {
  pinIndices(start, len);
  const UChar *array = getBuffer();  // not getArrayStart() to check kIsBogus & kOpenGetBuffer
  if(array==NULL) {
    array=fUnion.fStackFields.fBuffer;  // anything not NULL because that would make an empty string
    len=-2;  // bogus result string
  }
  return UnicodeString(FALSE, array + start, len);
}

int32_t
UnicodeString::toUTF8(int32_t start, int32_t len,
                      char *target, int32_t capacity) const {
  pinIndices(start, len);
  int32_t length8;
  UErrorCode errorCode = U_ZERO_ERROR;
  u_strToUTF8WithSub(target, capacity, &length8,
                     getBuffer() + start, len,
                     0xFFFD,  // Standard substitution character.
                     NULL,    // Don't care about number of substitutions.
                     &errorCode);
  return length8;
}

#if U_CHARSET_IS_UTF8

int32_t
UnicodeString::extract(int32_t start, int32_t len,
                       char *target, uint32_t dstSize) const {
  // if the arguments are illegal, then do nothing
  if(/*dstSize < 0 || */(dstSize > 0 && target == 0)) {
    return 0;
  }
  return toUTF8(start, len, target, dstSize <= 0x7fffffff ? (int32_t)dstSize : 0x7fffffff);
}

// else see unistr_cnv.cpp
#endif

void 
UnicodeString::extractBetween(int32_t start,
                  int32_t limit,
                  UnicodeString& target) const {
  pinIndex(start);
  pinIndex(limit);
  doExtract(start, limit - start, target);
}

// When converting from UTF-16 to UTF-8, the result will have at most 3 times
// as many bytes as the source has UChars.
// The "worst cases" are writing systems like Indic, Thai and CJK with
// 3:1 bytes:UChars.
void
UnicodeString::toUTF8(ByteSink &sink) const {
  int32_t length16 = length();
  if(length16 != 0) {
    char stackBuffer[1024];
    int32_t capacity = (int32_t)sizeof(stackBuffer);
    UBool utf8IsOwned = FALSE;
    char *utf8 = sink.GetAppendBuffer(length16 < capacity ? length16 : capacity,
                                      3*length16,
                                      stackBuffer, capacity,
                                      &capacity);
    int32_t length8 = 0;
    UErrorCode errorCode = U_ZERO_ERROR;
    u_strToUTF8WithSub(utf8, capacity, &length8,
                       getBuffer(), length16,
                       0xFFFD,  // Standard substitution character.
                       NULL,    // Don't care about number of substitutions.
                       &errorCode);
    if(errorCode == U_BUFFER_OVERFLOW_ERROR) {
      utf8 = (char *)uprv_malloc(length8);
      if(utf8 != NULL) {
        utf8IsOwned = TRUE;
        errorCode = U_ZERO_ERROR;
        u_strToUTF8WithSub(utf8, length8, &length8,
                           getBuffer(), length16,
                           0xFFFD,  // Standard substitution character.
                           NULL,    // Don't care about number of substitutions.
                           &errorCode);
      } else {
        errorCode = U_MEMORY_ALLOCATION_ERROR;
      }
    }
    if(U_SUCCESS(errorCode)) {
      sink.Append(utf8, length8);
      sink.Flush();
    }
    if(utf8IsOwned) {
      uprv_free(utf8);
    }
  }
}

int32_t
UnicodeString::toUTF32(UChar32 *utf32, int32_t capacity, UErrorCode &errorCode) const {
  int32_t length32=0;
  if(U_SUCCESS(errorCode)) {
    // getBuffer() and u_strToUTF32WithSub() check for illegal arguments.
    u_strToUTF32WithSub(utf32, capacity, &length32,
        getBuffer(), length(),
        0xfffd,  // Substitution character.
        NULL,    // Don't care about number of substitutions.
        &errorCode);
  }
  return length32;
}

int32_t 
UnicodeString::indexOf(const UChar *srcChars,
               int32_t srcStart,
               int32_t srcLength,
               int32_t start,
               int32_t length) const
{
  if(isBogus() || srcChars == 0 || srcStart < 0 || srcLength == 0) {
    return -1;
  }

  // UnicodeString does not find empty substrings
  if(srcLength < 0 && srcChars[srcStart] == 0) {
    return -1;
  }

  // get the indices within bounds
  pinIndices(start, length);

  // find the first occurrence of the substring
  const UChar *array = getArrayStart();
  const UChar *match = u_strFindFirst(array + start, length, srcChars + srcStart, srcLength);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

int32_t
UnicodeString::doIndexOf(UChar c,
             int32_t start,
             int32_t length) const
{
  // pin indices
  pinIndices(start, length);

  // find the first occurrence of c
  const UChar *array = getArrayStart();
  const UChar *match = u_memchr(array + start, c, length);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

int32_t
UnicodeString::doIndexOf(UChar32 c,
                         int32_t start,
                         int32_t length) const {
  // pin indices
  pinIndices(start, length);

  // find the first occurrence of c
  const UChar *array = getArrayStart();
  const UChar *match = u_memchr32(array + start, c, length);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

int32_t 
UnicodeString::lastIndexOf(const UChar *srcChars,
               int32_t srcStart,
               int32_t srcLength,
               int32_t start,
               int32_t length) const
{
  if(isBogus() || srcChars == 0 || srcStart < 0 || srcLength == 0) {
    return -1;
  }

  // UnicodeString does not find empty substrings
  if(srcLength < 0 && srcChars[srcStart] == 0) {
    return -1;
  }

  // get the indices within bounds
  pinIndices(start, length);

  // find the last occurrence of the substring
  const UChar *array = getArrayStart();
  const UChar *match = u_strFindLast(array + start, length, srcChars + srcStart, srcLength);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

int32_t
UnicodeString::doLastIndexOf(UChar c,
                 int32_t start,
                 int32_t length) const
{
  if(isBogus()) {
    return -1;
  }

  // pin indices
  pinIndices(start, length);

  // find the last occurrence of c
  const UChar *array = getArrayStart();
  const UChar *match = u_memrchr(array + start, c, length);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

int32_t
UnicodeString::doLastIndexOf(UChar32 c,
                             int32_t start,
                             int32_t length) const {
  // pin indices
  pinIndices(start, length);

  // find the last occurrence of c
  const UChar *array = getArrayStart();
  const UChar *match = u_memrchr32(array + start, c, length);
  if(match == NULL) {
    return -1;
  } else {
    return (int32_t)(match - array);
  }
}

//========================================
// Write implementation
//========================================

UnicodeString& 
UnicodeString::findAndReplace(int32_t start,
                  int32_t length,
                  const UnicodeString& oldText,
                  int32_t oldStart,
                  int32_t oldLength,
                  const UnicodeString& newText,
                  int32_t newStart,
                  int32_t newLength)
{
  if(isBogus() || oldText.isBogus() || newText.isBogus()) {
    return *this;
  }

  pinIndices(start, length);
  oldText.pinIndices(oldStart, oldLength);
  newText.pinIndices(newStart, newLength);

  if(oldLength == 0) {
    return *this;
  }

  while(length > 0 && length >= oldLength) {
    int32_t pos = indexOf(oldText, oldStart, oldLength, start, length);
    if(pos < 0) {
      // no more oldText's here: done
      break;
    } else {
      // we found oldText, replace it by newText and go beyond it
      replace(pos, oldLength, newText, newStart, newLength);
      length -= pos + oldLength - start;
      start = pos + newLength;
    }
  }

  return *this;
}


void
UnicodeString::setToBogus()
{
  releaseArray();

  fUnion.fFields.fLengthAndFlags = kIsBogus;
  fUnion.fFields.fArray = 0;
  fUnion.fFields.fCapacity = 0;
}

// turn a bogus string into an empty one
void
UnicodeString::unBogus() {
  if(fUnion.fFields.fLengthAndFlags & kIsBogus) {
    setToEmpty();
  }
}

const UChar *
UnicodeString::getTerminatedBuffer() {
  if(!isWritable()) {
    return 0;
  }
  UChar *array = getArrayStart();
  int32_t len = length();
  if(len < getCapacity()) {
    if(fUnion.fFields.fLengthAndFlags & kBufferIsReadonly) {
      // If len<capacity on a read-only alias, then array[len] is
      // either the original NUL (if constructed with (TRUE, s, length))
      // or one of the original string contents characters (if later truncated),
      // therefore we can assume that array[len] is initialized memory.
      if(array[len] == 0) {
        return array;
      }
    } else if(((fUnion.fFields.fLengthAndFlags & kRefCounted) == 0 || refCount() == 1)) {
      // kRefCounted: Do not write the NUL if the buffer is shared.
      // That is mostly safe, except when the length of one copy was modified
      // without copy-on-write, e.g., via truncate(newLength) or remove(void).
      // Then the NUL would be written into the middle of another copy's string.

      // Otherwise, the buffer is fully writable and it is anyway safe to write the NUL.
      // Do not test if there is a NUL already because it might be uninitialized memory.
      // (That would be safe, but tools like valgrind & Purify would complain.)
      array[len] = 0;
      return array;
    }
  }
  if(cloneArrayIfNeeded(len+1)) {
    array = getArrayStart();
    array[len] = 0;
    return array;
  } else {
    return NULL;
  }
}

// setTo() analogous to the readonly-aliasing constructor with the same signature
UnicodeString &
UnicodeString::setTo(UBool isTerminated,
                     const UChar *text,
                     int32_t textLength)
{
  if(fUnion.fFields.fLengthAndFlags & kOpenGetBuffer) {
    // do not modify a string that has an "open" getBuffer(minCapacity)
    return *this;
  }

  if(text == NULL) {
    // treat as an empty string, do not alias
    releaseArray();
    setToEmpty();
    return *this;
  }

  if( textLength < -1 ||
      (textLength == -1 && !isTerminated) ||
      (textLength >= 0 && isTerminated && text[textLength] != 0)
  ) {
    setToBogus();
    return *this;
  }

  releaseArray();

  if(textLength == -1) {
    // text is terminated, or else it would have failed the above test
    textLength = u_strlen(text);
  }
  fUnion.fFields.fLengthAndFlags = kReadonlyAlias;
  setArray((UChar *)text, textLength, isTerminated ? textLength + 1 : textLength);
  return *this;
}

// setTo() analogous to the writable-aliasing constructor with the same signature
UnicodeString &
UnicodeString::setTo(UChar *buffer,
                     int32_t buffLength,
                     int32_t buffCapacity) {
  if(fUnion.fFields.fLengthAndFlags & kOpenGetBuffer) {
    // do not modify a string that has an "open" getBuffer(minCapacity)
    return *this;
  }

  if(buffer == NULL) {
    // treat as an empty string, do not alias
    releaseArray();
    setToEmpty();
    return *this;
  }

  if(buffLength < -1 || buffCapacity < 0 || buffLength > buffCapacity) {
    setToBogus();
    return *this;
  } else if(buffLength == -1) {
    // buffLength = u_strlen(buff); but do not look beyond buffCapacity
    const UChar *p = buffer, *limit = buffer + buffCapacity;
    while(p != limit && *p != 0) {
      ++p;
    }
    buffLength = (int32_t)(p - buffer);
  }

  releaseArray();

  fUnion.fFields.fLengthAndFlags = kWritableAlias;
  setArray(buffer, buffLength, buffCapacity);
  return *this;
}

UnicodeString &UnicodeString::setToUTF8(const StringPiece &utf8) {
  unBogus();
  int32_t length = utf8.length();
  int32_t capacity;
  // The UTF-16 string will be at most as long as the UTF-8 string.
  if(length <= US_STACKBUF_SIZE) {
    capacity = US_STACKBUF_SIZE;
  } else {
    capacity = length + 1;  // +1 for the terminating NUL.
  }
  UChar *utf16 = getBuffer(capacity);
  int32_t length16;
  UErrorCode errorCode = U_ZERO_ERROR;
  u_strFromUTF8WithSub(utf16, getCapacity(), &length16,
      utf8.data(), length,
      0xfffd,  // Substitution character.
      NULL,    // Don't care about number of substitutions.
      &errorCode);
  releaseBuffer(length16);
  if(U_FAILURE(errorCode)) {
    setToBogus();
  }
  return *this;
}

UnicodeString&
UnicodeString::setCharAt(int32_t offset,
             UChar c)
{
  int32_t len = length();
  if(cloneArrayIfNeeded() && len > 0) {
    if(offset < 0) {
      offset = 0;
    } else if(offset >= len) {
      offset = len - 1;
    }

    getArrayStart()[offset] = c;
  }
  return *this;
}

UnicodeString&
UnicodeString::replace(int32_t start,
               int32_t _length,
               UChar32 srcChar) {
  UChar buffer[U16_MAX_LENGTH];
  int32_t count = 0;
  UBool isError = FALSE;
  U16_APPEND(buffer, count, U16_MAX_LENGTH, srcChar, isError);
  // We test isError so that the compiler does not complain that we don't.
  // If isError (srcChar is not a valid code point) then count==0 which means
  // we remove the source segment rather than replacing it with srcChar.
  return doReplace(start, _length, buffer, 0, isError ? 0 : count);
}

UnicodeString&
UnicodeString::append(UChar32 srcChar) {
  UChar buffer[U16_MAX_LENGTH];
  int32_t _length = 0;
  UBool isError = FALSE;
  U16_APPEND(buffer, _length, U16_MAX_LENGTH, srcChar, isError);
  // We test isError so that the compiler does not complain that we don't.
  // If isError then _length==0 which turns the doAppend() into a no-op anyway.
  return isError ? *this : doAppend(buffer, 0, _length);
}

UnicodeString&
UnicodeString::doReplace( int32_t start,
              int32_t length,
              const UnicodeString& src,
              int32_t srcStart,
              int32_t srcLength)
{
  // pin the indices to legal values
  src.pinIndices(srcStart, srcLength);

  // get the characters from src
  // and replace the range in ourselves with them
  return doReplace(start, length, src.getArrayStart(), srcStart, srcLength);
}

UnicodeString&
UnicodeString::doReplace(int32_t start,
             int32_t length,
             const UChar *srcChars,
             int32_t srcStart,
             int32_t srcLength)
{
  if(!isWritable()) {
    return *this;
  }

  int32_t oldLength = this->length();

  // optimize (read-only alias).remove(0, start) and .remove(start, end)
  if((fUnion.fFields.fLengthAndFlags&kBufferIsReadonly) && srcLength == 0) {
    if(start == 0) {
      // remove prefix by adjusting the array pointer
      pinIndex(length);
      fUnion.fFields.fArray += length;
      fUnion.fFields.fCapacity -= length;
      setLength(oldLength - length);
      return *this;
    } else {
      pinIndex(start);
      if(length >= (oldLength - start)) {
        // remove suffix by reducing the length (like truncate())
        setLength(start);
        fUnion.fFields.fCapacity = start;  // not NUL-terminated any more
        return *this;
      }
    }
  }

  if(start == oldLength) {
    return doAppend(srcChars, srcStart, srcLength);
  }

  if(srcChars == 0) {
    srcStart = srcLength = 0;
  } else if(srcLength < 0) {
    // get the srcLength if necessary
    srcLength = u_strlen(srcChars + srcStart);
  }

  // pin the indices to legal values
  pinIndices(start, length);

  // calculate the size of the string after the replace
  int32_t newLength = oldLength - length + srcLength;

  // cloneArrayIfNeeded(doCopyArray=FALSE) may change fArray but will not copy the current contents;
  // therefore we need to keep the current fArray
  UChar oldStackBuffer[US_STACKBUF_SIZE];
  UChar *oldArray;
  if((fUnion.fFields.fLengthAndFlags&kUsingStackBuffer) && (newLength > US_STACKBUF_SIZE)) {
    // copy the stack buffer contents because it will be overwritten with
    // fUnion.fFields values
    u_memcpy(oldStackBuffer, fUnion.fStackFields.fBuffer, oldLength);
    oldArray = oldStackBuffer;
  } else {
    oldArray = getArrayStart();
  }

  // clone our array and allocate a bigger array if needed
  int32_t *bufferToDelete = 0;
  if(!cloneArrayIfNeeded(newLength, newLength + (newLength >> 2) + kGrowSize,
                         FALSE, &bufferToDelete)
  ) {
    return *this;
  }

  // now do the replace

  UChar *newArray = getArrayStart();
  if(newArray != oldArray) {
    // if fArray changed, then we need to copy everything except what will change
    us_arrayCopy(oldArray, 0, newArray, 0, start);
    us_arrayCopy(oldArray, start + length,
                 newArray, start + srcLength,
                 oldLength - (start + length));
  } else if(length != srcLength) {
    // fArray did not change; copy only the portion that isn't changing, leaving a hole
    us_arrayCopy(oldArray, start + length,
                 newArray, start + srcLength,
                 oldLength - (start + length));
  }

  // now fill in the hole with the new string
  us_arrayCopy(srcChars, srcStart, newArray, start, srcLength);

  setLength(newLength);

  // delayed delete in case srcChars == fArray when we started, and
  // to keep oldArray alive for the above operations
  if (bufferToDelete) {
    uprv_free(bufferToDelete);
  }

  return *this;
}

// Versions of doReplace() only for append() variants.
// doReplace() and doAppend() optimize for different cases.

UnicodeString&
UnicodeString::doAppend(const UnicodeString& src, int32_t srcStart, int32_t srcLength) {
  if(srcLength == 0) {
    return *this;
  }

  // pin the indices to legal values
  src.pinIndices(srcStart, srcLength);
  return doAppend(src.getArrayStart(), srcStart, srcLength);
}

UnicodeString&
UnicodeString::doAppend(const UChar *srcChars, int32_t srcStart, int32_t srcLength) {
  if(!isWritable() || srcLength == 0 || srcChars == NULL) {
    return *this;
  }

  if(srcLength < 0) {
    // get the srcLength if necessary
    if((srcLength = u_strlen(srcChars + srcStart)) == 0) {
      return *this;
    }
  }

  int32_t oldLength = length();
  int32_t newLength;
  if (uprv_add32_overflow(oldLength, srcLength, &newLength)) {
    setToBogus();
    return *this;
  }
  // optimize append() onto a large-enough, owned string
  if((newLength <= getCapacity() && isBufferWritable()) ||
      cloneArrayIfNeeded(newLength, newLength + (newLength >> 2) + kGrowSize)) {
    UChar *newArray = getArrayStart();
    // Do not copy characters when
    //   UChar *buffer=str.getAppendBuffer(...);
    // is followed by
    //   str.append(buffer, length);
    // or
    //   str.appendString(buffer, length)
    // or similar.
    if(srcChars + srcStart != newArray + oldLength) {
      us_arrayCopy(srcChars, srcStart, newArray, oldLength, srcLength);
    }
    setLength(newLength);
  }
  return *this;
}

/**
 * Replaceable API
 */
void
UnicodeString::handleReplaceBetween(int32_t start,
                                    int32_t limit,
                                    const UnicodeString& text) {
    replaceBetween(start, limit, text);
}

/**
 * Replaceable API
 */
void 
UnicodeString::copy(int32_t start, int32_t limit, int32_t dest) {
    if (limit <= start) {
        return; // Nothing to do; avoid bogus malloc call
    }
    UChar* text = (UChar*) uprv_malloc( sizeof(UChar) * (limit - start) );
    // Check to make sure text is not null.
    if (text != NULL) {
	    extractBetween(start, limit, text, 0);
	    insert(dest, text, 0, limit - start);    
	    uprv_free(text);
    }
}

/**
 * Replaceable API
 *
 * NOTE: This is for the Replaceable class.  There is no rep.cpp,
 * so we implement this function here.
 */
UBool Replaceable::hasMetaData() const {
    return TRUE;
}

/**
 * Replaceable API
 */
UBool UnicodeString::hasMetaData() const {
    return FALSE;
}

UnicodeString&
UnicodeString::doReverse(int32_t start, int32_t length) {
  if(length <= 1 || !cloneArrayIfNeeded()) {
    return *this;
  }

  // pin the indices to legal values
  pinIndices(start, length);
  if(length <= 1) {  // pinIndices() might have shrunk the length
    return *this;
  }

  UChar *left = getArrayStart() + start;
  UChar *right = left + length - 1;  // -1 for inclusive boundary (length>=2)
  UChar swap;
  UBool hasSupplementary = FALSE;

  // Before the loop we know left<right because length>=2.
  do {
    hasSupplementary |= (UBool)U16_IS_LEAD(swap = *left);
    hasSupplementary |= (UBool)U16_IS_LEAD(*left++ = *right);
    *right-- = swap;
  } while(left < right);
  // Make sure to test the middle code unit of an odd-length string.
  // Redundant if the length is even.
  hasSupplementary |= (UBool)U16_IS_LEAD(*left);

  /* if there are supplementary code points in the reversed range, then re-swap their surrogates */
  if(hasSupplementary) {
    UChar swap2;

    left = getArrayStart() + start;
    right = left + length - 1; // -1 so that we can look at *(left+1) if left<right
    while(left < right) {
      if(U16_IS_TRAIL(swap = *left) && U16_IS_LEAD(swap2 = *(left + 1))) {
        *left++ = swap2;
        *left++ = swap;
      } else {
        ++left;
      }
    }
  }

  return *this;
}

UBool 
UnicodeString::padLeading(int32_t targetLength,
                          UChar padChar)
{
  int32_t oldLength = length();
  if(oldLength >= targetLength || !cloneArrayIfNeeded(targetLength)) {
    return FALSE;
  } else {
    // move contents up by padding width
    UChar *array = getArrayStart();
    int32_t start = targetLength - oldLength;
    us_arrayCopy(array, 0, array, start, oldLength);

    // fill in padding character
    while(--start >= 0) {
      array[start] = padChar;
    }
    setLength(targetLength);
    return TRUE;
  }
}

UBool 
UnicodeString::padTrailing(int32_t targetLength,
                           UChar padChar)
{
  int32_t oldLength = length();
  if(oldLength >= targetLength || !cloneArrayIfNeeded(targetLength)) {
    return FALSE;
  } else {
    // fill in padding character
    UChar *array = getArrayStart();
    int32_t length = targetLength;
    while(--length >= oldLength) {
      array[length] = padChar;
    }
    setLength(targetLength);
    return TRUE;
  }
}

//========================================
// Hashing
//========================================
int32_t
UnicodeString::doHashCode() const
{
    /* Delegate hash computation to uhash.  This makes UnicodeString
     * hashing consistent with UChar* hashing.  */
    int32_t hashCode = ustr_hashUCharsN(getArrayStart(), length());
    if (hashCode == kInvalidHashCode) {
        hashCode = kEmptyHashCode;
    }
    return hashCode;
}

//========================================
// External Buffer
//========================================

UChar *
UnicodeString::getBuffer(int32_t minCapacity) {
  if(minCapacity>=-1 && cloneArrayIfNeeded(minCapacity)) {
    fUnion.fFields.fLengthAndFlags|=kOpenGetBuffer;
    setZeroLength();
    return getArrayStart();
  } else {
    return 0;
  }
}

void
UnicodeString::releaseBuffer(int32_t newLength) {
  if(fUnion.fFields.fLengthAndFlags&kOpenGetBuffer && newLength>=-1) {
    // set the new fLength
    int32_t capacity=getCapacity();
    if(newLength==-1) {
      // the new length is the string length, capped by fCapacity
      const UChar *array=getArrayStart(), *p=array, *limit=array+capacity;
      while(p<limit && *p!=0) {
        ++p;
      }
      newLength=(int32_t)(p-array);
    } else if(newLength>capacity) {
      newLength=capacity;
    }
    setLength(newLength);
    fUnion.fFields.fLengthAndFlags&=~kOpenGetBuffer;
  }
}

//========================================
// Miscellaneous
//========================================
UBool
UnicodeString::cloneArrayIfNeeded(int32_t newCapacity,
                                  int32_t growCapacity,
                                  UBool doCopyArray,
                                  int32_t **pBufferToDelete,
                                  UBool forceClone) {
  // default parameters need to be static, therefore
  // the defaults are -1 to have convenience defaults
  if(newCapacity == -1) {
    newCapacity = getCapacity();
  }

  // while a getBuffer(minCapacity) is "open",
  // prevent any modifications of the string by returning FALSE here
  // if the string is bogus, then only an assignment or similar can revive it
  if(!isWritable()) {
    return FALSE;
  }

  /*
   * We need to make a copy of the array if
   * the buffer is read-only, or
   * the buffer is refCounted (shared), and refCount>1, or
   * the buffer is too small.
   * Return FALSE if memory could not be allocated.
   */
  if(forceClone ||
     fUnion.fFields.fLengthAndFlags & kBufferIsReadonly ||
     (fUnion.fFields.fLengthAndFlags & kRefCounted && refCount() > 1) ||
     newCapacity > getCapacity()
  ) {
    // check growCapacity for default value and use of the stack buffer
    if(growCapacity < 0) {
      growCapacity = newCapacity;
    } else if(newCapacity <= US_STACKBUF_SIZE && growCapacity > US_STACKBUF_SIZE) {
      growCapacity = US_STACKBUF_SIZE;
    }

    // save old values
    UChar oldStackBuffer[US_STACKBUF_SIZE];
    UChar *oldArray;
    int32_t oldLength = length();
    int16_t flags = fUnion.fFields.fLengthAndFlags;

    if(flags&kUsingStackBuffer) {
      U_ASSERT(!(flags&kRefCounted)); /* kRefCounted and kUsingStackBuffer are mutally exclusive */
      if(doCopyArray && growCapacity > US_STACKBUF_SIZE) {
        // copy the stack buffer contents because it will be overwritten with
        // fUnion.fFields values
        us_arrayCopy(fUnion.fStackFields.fBuffer, 0, oldStackBuffer, 0, oldLength);
        oldArray = oldStackBuffer;
      } else {
        oldArray = NULL; // no need to copy from the stack buffer to itself
      }
    } else {
      oldArray = fUnion.fFields.fArray;
      U_ASSERT(oldArray!=NULL); /* when stack buffer is not used, oldArray must have a non-NULL reference */
    }

    // allocate a new array
    if(allocate(growCapacity) ||
       (newCapacity < growCapacity && allocate(newCapacity))
    ) {
      if(doCopyArray) {
        // copy the contents
        // do not copy more than what fits - it may be smaller than before
        int32_t minLength = oldLength;
        newCapacity = getCapacity();
        if(newCapacity < minLength) {
          minLength = newCapacity;
        }
        if(oldArray != NULL) {
          us_arrayCopy(oldArray, 0, getArrayStart(), 0, minLength);
        }
        setLength(minLength);
      } else {
        setZeroLength();
      }

      // release the old array
      if(flags & kRefCounted) {
        // the array is refCounted; decrement and release if 0
        u_atomic_int32_t *pRefCount = ((u_atomic_int32_t *)oldArray - 1);
        if(umtx_atomic_dec(pRefCount) == 0) {
          if(pBufferToDelete == 0) {
              // Note: cast to (void *) is needed with MSVC, where u_atomic_int32_t
              // is defined as volatile. (Volatile has useful non-standard behavior
              //   with this compiler.)
            uprv_free((void *)pRefCount);
          } else {
            // the caller requested to delete it himself
            *pBufferToDelete = (int32_t *)pRefCount;
          }
        }
      }
    } else {
      // not enough memory for growCapacity and not even for the smaller newCapacity
      // reset the old values for setToBogus() to release the array
      if(!(flags&kUsingStackBuffer)) {
        fUnion.fFields.fArray = oldArray;
      }
      fUnion.fFields.fLengthAndFlags = flags;
      setToBogus();
      return FALSE;
    }
  }
  return TRUE;
}

// UnicodeStringAppendable ------------------------------------------------- ***

UnicodeStringAppendable::~UnicodeStringAppendable() {}

UBool
UnicodeStringAppendable::appendCodeUnit(UChar c) {
  return str.doAppend(&c, 0, 1).isWritable();
}

UBool
UnicodeStringAppendable::appendCodePoint(UChar32 c) {
  UChar buffer[U16_MAX_LENGTH];
  int32_t cLength = 0;
  UBool isError = FALSE;
  U16_APPEND(buffer, cLength, U16_MAX_LENGTH, c, isError);
  return !isError && str.doAppend(buffer, 0, cLength).isWritable();
}

UBool
UnicodeStringAppendable::appendString(const UChar *s, int32_t length) {
  return str.doAppend(s, 0, length).isWritable();
}

UBool
UnicodeStringAppendable::reserveAppendCapacity(int32_t appendCapacity) {
  return str.cloneArrayIfNeeded(str.length() + appendCapacity);
}

UChar *
UnicodeStringAppendable::getAppendBuffer(int32_t minCapacity,
                                         int32_t desiredCapacityHint,
                                         UChar *scratch, int32_t scratchCapacity,
                                         int32_t *resultCapacity) {
  if(minCapacity < 1 || scratchCapacity < minCapacity) {
    *resultCapacity = 0;
    return NULL;
  }
  int32_t oldLength = str.length();
  if(str.cloneArrayIfNeeded(oldLength + minCapacity, oldLength + desiredCapacityHint)) {
    *resultCapacity = str.getCapacity() - oldLength;
    return str.getArrayStart() + oldLength;
  }
  *resultCapacity = scratchCapacity;
  return scratch;
}

U_NAMESPACE_END

U_NAMESPACE_USE

U_CAPI int32_t U_EXPORT2
uhash_hashUnicodeString(const UElement key) {
    const UnicodeString *str = (const UnicodeString*) key.pointer;
    return (str == NULL) ? 0 : str->hashCode();
}

// Moved here from uhash_us.cpp so that using a UVector of UnicodeString*
// does not depend on hashtable code.
U_CAPI UBool U_EXPORT2
uhash_compareUnicodeString(const UElement key1, const UElement key2) {
    const UnicodeString *str1 = (const UnicodeString*) key1.pointer;
    const UnicodeString *str2 = (const UnicodeString*) key2.pointer;
    if (str1 == str2) {
        return TRUE;
    }
    if (str1 == NULL || str2 == NULL) {
        return FALSE;
    }
    return *str1 == *str2;
}

#ifdef U_STATIC_IMPLEMENTATION
/*
This should never be called. It is defined here to make sure that the
virtual vector deleting destructor is defined within unistr.cpp.
The vector deleting destructor is already a part of UObject,
but defining it here makes sure that it is included with this object file.
This makes sure that static library dependencies are kept to a minimum.
*/
static void uprv_UnicodeStringDummy(void) {
    delete [] (new UnicodeString[2]);
}
#endif
