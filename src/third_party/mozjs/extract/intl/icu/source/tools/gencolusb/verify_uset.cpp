// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/**
 * Copyright (c) 1999-2012, International Business Machines Corporation and
 * others. All Rights Reserved.
 *
 * Test for source/i18n/collunsafe.h
 */

#include <stdio.h>
#include "unicode/ucol.h"
#include "unicode/uniset.h"
#include "unicode/coll.h"
#include "collation.h"

#include "collunsafe.h"


int main(int argc, const char *argv[]) {
  puts("verify");
  UErrorCode errorCode = U_ZERO_ERROR;
#if defined (COLLUNSAFE_PATTERN)
  puts("verify pattern");
  const UnicodeString unsafeBackwardPattern(FALSE, collunsafe_pattern, collunsafe_len);
  fprintf(stderr, "\n -- pat '%c%c%c%c%c'\n",
          collunsafe_pattern[0],
          collunsafe_pattern[1],
          collunsafe_pattern[2],
          collunsafe_pattern[3],
          collunsafe_pattern[4]);
  if(U_SUCCESS(errorCode)) {
    UnicodeSet us(unsafeBackwardPattern, errorCode);
    fprintf(stderr, "\n%s:%d: err creating set %s\n", __FILE__, __LINE__, u_errorName(errorCode));
  }
#endif

#if defined (COLLUNSAFE_RANGE)
  {
    puts("verify range");
    UnicodeSet u;
    for(int32_t i=0;i<unsafe_rangeCount*2;i+=2) {
      u.add(unsafe_ranges[i+0],unsafe_ranges[i+1]);
    }
    printf("Finished with %d ranges\n", u.getRangeCount());
  }
#endif

#if defined (COLLUNSAFE_SERIALIZE)
  {
    puts("verify serialize");
    UnicodeSet u(unsafe_serializedData, unsafe_serializedCount, UnicodeSet::kSerialized, errorCode);
    fprintf(stderr, "\n%s:%d: err creating set %s\n", __FILE__, __LINE__, u_errorName(errorCode));
    printf("Finished deserialize with %d ranges\n", u.getRangeCount());
  }
#endif
// if(tailoring.unsafeBackwardSet == NULL) {
  //   errorCode = U_MEMORY_ALLOCATION_ERROR;
  //   fprintf(stderr, "\n%s:%d: err %s\n", __FILE__, __LINE__, u_errorName(errorCode));
  // }
  puts("verify col UCA");
  if(U_SUCCESS(errorCode)) {
    Collator *col = Collator::createInstance(Locale::getEnglish(), errorCode);
    fprintf(stderr, "\n%s:%d: err %s creating collator\n", __FILE__, __LINE__, u_errorName(errorCode));
  }
  
  if(U_FAILURE(errorCode)) {
    return 1;
  } else {
    return 0;
  }
}
