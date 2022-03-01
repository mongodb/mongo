// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2006, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  genres32.c
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003sep10
*   created by: Markus W. Scherer
*
*   Write an ICU resource bundle with a table whose
*   number of key characters and number of items both exceed 64k.
*   Writing it as the root table tests also that
*   the new table type is recognized for the root resource by the reader code.
*/
#include <stdio.h>
#include "unicode/putil.h"
#include "cstring.h"
#include "gentest.h"

static void
incKey(char *key, char *limit) {
    char c;

    while(limit>key) {
        c=*--limit;
        if(c=='o') {
            *limit='1';
            break;
        } else {
            *limit='o';
        }
    }
}

U_CFUNC int
genres32(const char *prog, const char *path) {
    /*
     * key string, gets incremented binary numbers
     * letter 'o'=0 and digit '1'=1 so that data swapping can be tested
     * with reordering (ASCII: '1'<'o' EBCDIC: '1'>'o')
     *
     * need 17 digits for >64k unique items
     */
    char key[20]="ooooooooooooooooo";
    char *limit;
    int i;
    char file[512];
    FILE *out;

    uprv_strcpy(file,path);
    if(file[strlen(file)-1]!=U_FILE_SEP_CHAR) {
        uprv_strcat(file,U_FILE_SEP_STRING);
    }
    uprv_strcat(file,"testtable32.txt");
    out = fopen(file, "w");
    /*puts(file);*/
    puts("Generating testtable32.txt");
    if(out == NULL) {
        fprintf(stderr, "%s: Couldn't create resource test file %s\n",
                prog, file);
        return 1;
    }
    
    /* find the limit of the key string */
    for(limit=key; *limit!=0; ++limit) {
    }

    /* output the beginning of the bundle */
    fputs(
          "testtable32 {", out
    );

    /* output the table entries */
    for(i=0; i<66000; ++i) {
        if(i%10==0) {
            /*
             * every 10th entry contains a string with
             * the entry index as its code point
             */
            fprintf(out, "%s{\"\\U%08x\"}\n", key, i);
        } else {
            /* other entries contain their index as an integer */
            fprintf(out, "%s:int{%d}\n", key, i);
        }

        incKey(key, limit);
    }

    /* output the end of the bundle */
    fputs(
          "}", out
    );

    fclose(out);
    return 0;
}
