// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
 * %W% %E%
 *
 * (C) Copyright IBM Corp. 2001-2016 - All Rights Reserved
 *
 */

#include "unicode/utypes.h"
#include "unicode/uscript.h"

#include "cmemory.h"
#include "scrptrun.h"

#include <stdio.h>

UChar testChars[] = {
            0x0020, 0x0946, 0x0939, 0x093F, 0x0928, 0x094D, 0x0926, 0x0940, 0x0020,
            0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064A, 0x0629, 0x0020,
            0x0420, 0x0443, 0x0441, 0x0441, 0x043A, 0x0438, 0x0439, 0x0020,
            'E', 'n', 'g', 'l', 'i', 's', 'h',  0x0020,
            0x6F22, 0x5B75, 0x3068, 0x3072, 0x3089, 0x304C, 0x306A, 0x3068, 
            0x30AB, 0x30BF, 0x30AB, 0x30CA,
            0xD801, 0xDC00, 0xD801, 0xDC01, 0xD801, 0xDC02, 0xD801, 0xDC03
};

int32_t testLength = UPRV_LENGTHOF(testChars);

int main()
{
    icu::ScriptRun scriptRun(testChars, 0, testLength);

    while (scriptRun.next()) {
        int32_t     start = scriptRun.getScriptStart();
        int32_t     end   = scriptRun.getScriptEnd();
        UScriptCode code  = scriptRun.getScriptCode();

        printf("Script '%s' from %d to %d.\n", uscript_getName(code), start, end);
    }
    return 0;
}
