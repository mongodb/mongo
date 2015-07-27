/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#define BUILD_UX_CONS_TABLE
#define BASE_NAME		cons
#undef  MAKE_INCLUDE
#define MAKE_INCLUDE

#include "dpml_ux.h"

#define	PACKED		1
#define	UNPACKED	2

#define PRINT_IT(name, value)					\
		printf("ENUM#   define " name "\t%i\n", index);	\
		print_table_entries(value); index++

@divert -append divertText

#   include "mphoc_functions.h"

    procedure print_table_entries (value)
        {
        auto exponent, tmp;

        tmp = (value == 0) ? bldexp(1, F_MIN_BIN_EXP - 1) : value;
        tmp = as_int(tmp, BITS_PER_F_TYPE, F_EXP_WIDTH, MP_F_EXP_BIAS, MP_RN);
        printf("_X_CON\t/* %2i */ %#32.4.16i,\n", index, tmp);
        return;
        }
 
    /* Create a NaN value */

#   if (ARCHITECTURE == alpha) && (OP_SYSTEM == osf)
        fraction = 1 - 1/2^F_PRECISION;
#   elif (ARCHITECTURE == mips)
        fraction = 1.5 - 1/2^F_PRECISION;
#   elif (ARCHITECTURE == hp_pa)
        fraction = 1.25;
#   else
        fraction = 1.5 - 1/2^F_PRECISION;
#   endif

/*
    nan_value = bldexp(fraction, F_MAX_BIN_EXP + 2);
*/
    inf_value = bldexp(1, F_MAX_BIN_EXP + 1);
    
    index = 0;

    printf("_X_CON#if INSTANTIATE_TABLE\n"
           "_X_CON\n"
           "_X_CON    const TABLE_UNION PACKED_CONSTANT_TABLE[] = {\n"
           "_X_CON\n");

    printf("ENUM\n"
           "ENUM#if INSTANTIATE_DEFINES\n"
           "ENUM\n");

    PRINT_IT( "ZERO\t\t",               0 );
    PRINT_IT( "ONE\t\t",                1 );
    PRINT_IT( "TWO\t\t",                2 );
    PRINT_IT( "PI\t\t",                pi );
    PRINT_IT( "PI_OVER_2\t",         pi/2 );
    PRINT_IT( "PI_OVER_4\t",         pi/4 );
    PRINT_IT( "THREE_PI_OVER_4\t", 3*pi/4 );
    PRINT_IT( "NINETY\t",              90 );
    PRINT_IT( "ONE_EIGHTY\t",         180 );
    PRINT_IT( "INF\t\t",        inf_value );
    printf("ENUM#   define LAST_CONS_INDEX\t%i\n"
           "ENUM\n"
           "ENUM#endif\n"
           "ENUM\n", index);

    printf("_X_CON\t};\n"
           "_X_CON\n"
           "_X_CON#endif\n"
           "_X_CON\n");

@end_divert
@eval my $outText    = MphocEval( GetStream( divertText ) );	\
      my $enumText   = Egrep( "^ENUM", $outText );		\
         $enumText   =~ s|ENUM||g;				\
      my $xconText   = Egrep( "^_X_CON", $outText );		\
         $xconText   =~ s|_X_CON||g;				\
         $outText    = "$enumText\n$xconText";			\
      my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),	\
                        "Interesting x_float constants",	\
                           "dpml_ux_cons.c" );			\
      print "$headerText\n$outText";
