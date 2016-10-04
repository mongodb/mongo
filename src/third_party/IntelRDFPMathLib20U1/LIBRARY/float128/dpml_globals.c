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

#undef	MAKE_INCLUDE
#define MAKE_INCLUDE

#undef	F_FLOAT
#define F_FLOAT
#define __F_SUFFIX  DPML_NULL_MACRO_TOKEN

#ifndef BUILD_FILE_NAME
#	define BUILD_FILE_NAME dpml_globals.h
#endif

#include "build.h"
#include "op_system.h"
#include "compiler.h"
#include "architecture.h"
#include "f_format.h"
#include "dpml_names.h"

/*
 * For each data type required by the system, this routine generates bit
 * patterns for the indicated values in the following order:
 */

#define	NAN_INDEX		0
#define	POS_ZERO_INDEX		1
#define	NEG_ZERO_INDEX		2
#define	POS_TINY_INDEX		3
#define	NEG_TINY_INDEX		4
#define	POS_HUGE_INDEX		5
#define	NEG_HUGE_INDEX		6
#define	POS_INFINITY_INDEX	7
#define	NEG_INFINITY_INDEX	8
#define	POS_ULP_FACTOR_INDEX	9
#define	NEG_ULP_FACTOR_INDEX	10
#define	POS_ONE_INDEX		11
#define	NEG_ONE_INDEX		12


/*
 *	The globals data is stored in the GLOBALS TABLE as a sequence of
 *	records.  If only IEEE values are needed, each record is 32 bytes in
 *	length; if VAX values are required in addition to the IEEE values, then
 *	each record is 64 bytes in length.  The fields in the records are
 *	arranged in the following format:
 *
 *		IEEE values only:
 *		+---+---+---+---+---+---+---+---+---+---+
 *		| s |	|   t	|	    x		|
 *	byte	+---+---+---+---+---+---+---+---+---+---+
 *	offset:	 0	 8	 16
 *
 *		Both IEEE and VAX values:
 *		+---+---+---+---+---+---+---+---+---+---+
 *		| s |	|   t	|	    x		|
 *	byte	+---+---+---+---+---+---+---+---+---+---+
 *	offset:	 0	 8	 16
 *		+---+---+---+---+---+---+---+---+---+---+
 *		| f |	|   g	|			|
 *	byte	+---+---+---+---+---+---+---+---+---+---+
 *	offset:	 32	 40
 *
 *
 *	The address of the global item with index N and data type T is given by
 *
 *	    GLOBAL_ADDR( T, N ) = ( char* )GLOBALS_TABLE +
 *				  8 * T +
 *				  BYTES_PER_TABLE_ENTRY * I
 *
 */

/*
 * In addition to the actual values, this routine also sets up a table
 * of address that allows type independent accessing of the values.
 * Values are generated only for those data types actually supported by
 * the platform.  The BIT_IS_SET macro is used to determine which 
 * "bits" are set in the FLOAT_TYPES macro
 */

#define	BIT_IS_SET(i,n) (((i) >> (n)) - (((i) >> ((n)+1)) << 1))

/*
 * Along with each set of constants, a symbolic constant for an enumerated
 * type is generated.  These constants have been chosen to allow for easy access
 * to the globals table.
 *
 *		#define	_s_TYPE	0
 *		#define	_t_TYPE	1
 *		#define	_x_TYPE	2
 *		#define	_f_TYPE	4
 *		#define	_g_TYPE	5
 *		#define	_d_TYPE	5
 *
 * (Note: These _{g,t,f,s,x}_TYPE constants are only referenced by
 * nt_exception.c, and the F_TYPE_ENUM macro (see below).
 * The F_TYPE_ENUM macro, in turn, is only referenced via the GLOBAL macro
 * (see below) and via ADD_ERR_CODE_TYPE defined in dpml_exception.h.)
 *
 * These enumerated types are used to provide type independent access of all
 * DPML global values.	For example, to access a positive one, use either:
 *
 *	*((F_TYPE*)GLOBAL_ADDR(PASTE_3(_,F_CHAR,_TYPE),POS_ONE_INDEX))
 * or
 *	GLOBAL(POS_ONE_INDEX)
 * or
 *	POS_ONE
 */

/*
 * These macros are used to print out the table values and ensure that
 * the values are printed out in the right order.
 *
 *	NOTE: Extending mphoc to deal with NaN's, infinities, signed
 *	zeros and ROP's would make life a lot simpler here.
 *
 *	NOTE: Although platforms that contain the VAX g and d float data type
 *	are little endian, the word containing the exponent comes first.
 */

#    define PR_SINGLE(a)      printf("\t"#a", 0, \n") ;

#if (ENDIANESS == big_endian)
#    define PR_DOUBLE(a,b)    printf("\t"#a", "#b", \n") ;
#    define PR_QUAD(a,b,x,y)  printf("\t"#a", "#b", "#x", "#y", \n") ;
#else
#    define PR_DOUBLE(a,b)    printf("\t"#b", "#a", \n") ;
#    define PR_QUAD(a,b,x,y)  printf("\t"#y", "#x", "#b", "#a", \n") ;
#endif

#if (BITS_PER_TABLE_WORD != 32)
#   error "BITS_PER_TABLE_WORD must be 32"
#endif

#include "mphoc_macros.h"


#   define TMP_FILE		ADD_EXTENSION(BUILD_FILE_NAME,tmp)

    @divert divertText

    printf( "#if !defined(GLOBALS_TABLE)\n" 
            "#   define GLOBALS_TABLE	__INTERNAL_NAME(globals_table)\n"
            "#endif\n\n" );

    do_f = (BIT_IS_SET(FLOAT_TYPES, f_floating));
    do_g = (BIT_IS_SET(FLOAT_TYPES, g_floating));
    do_s = (BIT_IS_SET(FLOAT_TYPES, s_floating));
    do_t = (BIT_IS_SET(FLOAT_TYPES, t_floating));
    do_x = (BIT_IS_SET(FLOAT_TYPES, x_floating));
    do_d = (BIT_IS_SET(FLOAT_TYPES, d_floating));

/*  We assume there are really only 2 different table formats:	{s, t, x}   */
/*  and {s, t, x, f, g}, which use 32 and 64 bytes respectively.	    */

    bytes_per_table_entry = 0 ;
    table_padding = 0 ;
    if ( do_s ) {
	printf( "#define _s_TYPE 0\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 8 ;
	}
    if ( do_t ) {
	printf( "#define _t_TYPE 1\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 8 ;
	}
    if ( do_x ) {
	printf( "#define _x_TYPE 2\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 16 ;
	}
    if ( do_f ) {
	printf( "#define _f_TYPE 4\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 8 ;
	}
    if ( do_g ) {
	printf( "#define _g_TYPE 5\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 8 ;
	}
    if ( do_d ) {
	printf( "#define _d_TYPE 5\n" ) ;
	bytes_per_table_entry = bytes_per_table_entry + 8 ;
	}
    if ( bytes_per_table_entry <= 32 ) {
	table_padding = 32 - bytes_per_table_entry ;
	bytes_per_table_entry  = 32 ;
	}
    else {
	table_padding = 64 - bytes_per_table_entry ;
	bytes_per_table_entry  = 64 ;
	}
    table_padding = table_padding / 4 ;

# define PAD_TABLE				\
    if ( table_padding ) {			\
	printf( "\t0," ) ;			\
	for ( i = 1 ; i < table_padding ; i++ )	\
	printf( " 0," ) ;			\
	printf( "\n" ) ;			\
	}					\

    printf("#ifdef GLOBAL_TABLE_VALUES\n\n");
    START_GLOBAL_TABLE(GLOBALS_TABLE, offset);

/*  NOTE:  the order in which the following statements occur *MUST* match   */
/*  the enumeration for the _{s,t,x,f,g,d}_TYPE constants above.	    */

/* NaNs and reserved operands */

    if	(do_s)
	PR_SINGLE(S_NAN_HI) 
    if	(do_t)	
	PR_DOUBLE(T_NAN_HI, NAN_LO)
    if	(do_x)	
	PR_QUAD(X_NAN_HI, NAN_LO, NAN_LO, NAN_LO)
    if	(do_f)
	PR_SINGLE(0x00008000) 
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00008000)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00008000)
    PAD_TABLE ;

/* POS ZERO */

    if	(do_s)
	PR_SINGLE(0x00000000) 
    if	(do_t)	
	PR_DOUBLE(0x00000000, 0x00000000) 
    if	(do_x)	
	PR_QUAD(0x00000000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x00000000) 
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00000000) 
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00000000) 
    PAD_TABLE ;

/* NEG ZERO */

    if	(do_s)
	PR_SINGLE(0x80000000)
    if	(do_t)	
	PR_DOUBLE(0x80000000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0x80000000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x00000000)
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00000000)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00000000)
    PAD_TABLE ;

/* POS TINY */

    if	(do_s)
	PR_SINGLE(0x00000001)
    if	(do_t)	
	PR_DOUBLE(0x00000000, 0x00000001)
    if	(do_x)	
	PR_QUAD(0x00000000, 0x00000000, 0x00000000, 0x00000001)
    if	(do_f)
	PR_SINGLE(0x00000080)
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00000010)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00000080)
    PAD_TABLE ;

/* NEG TINY */

    if	(do_s)
	PR_SINGLE(0x80000001)
    if	(do_t)	
	PR_DOUBLE(0x80000000, 0x00000001)
    if	(do_x)	
	PR_QUAD(0x80000000, 0x00000000, 0x00000000, 0x00000001)
    if	(do_f)
	PR_SINGLE(0x00008080)
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00008010)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00008080)
    PAD_TABLE ;

/* POS HUGE */

    if	(do_s)
	PR_SINGLE(0x7f7fffff)
    if	(do_t)	
	PR_DOUBLE(0x7fefffff, 0xffffffff) 
    if	(do_x)	
	PR_QUAD(0x7ffeffff, 0xffffffff, 0xffffffff, 0xffffffff)
    if	(do_f)
	PR_SINGLE(0xffff7fff)
    if	(do_g)	
	PR_DOUBLE(0xffffffff, 0xffff7fff) 
    if	(do_d)	
	PR_DOUBLE(0xffffffff, 0xffff7fff) 
    PAD_TABLE ;

/* NEG HUGE */

    if	(do_s)
	PR_SINGLE(0xff7fffff)
    if	(do_t)	
	PR_DOUBLE(0xffefffff, 0xffffffff)
    if	(do_x)	
	PR_QUAD(0xfffeffff, 0xffffffff, 0xffffffff, 0xffffffff)
    if	(do_f)
	PR_SINGLE(0xffffffff)
    if	(do_g)	
	PR_DOUBLE(0xffffffff, 0xffffffff)
    if	(do_d)	
	PR_DOUBLE(0xffffffff, 0xffffffff)
    PAD_TABLE ;

/* POS INFINITY */

    if	(do_s)
	PR_SINGLE(0x7f800000)
    if	(do_t)	
	PR_DOUBLE(0x7ff00000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0x7fff0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0xffff7fff)
    if	(do_g)	
	PR_DOUBLE(0xffffffff, 0xffff7fff)
    if	(do_d)	
	PR_DOUBLE(0xffffffff, 0xffff7fff)
    PAD_TABLE ;

/* NEG INFINITY */

    if	(do_s)
	PR_SINGLE(0xff800000)
    if	(do_t)	
	PR_DOUBLE(0xfff00000, 0x00000000) 
    if	(do_x)	
	PR_QUAD(0xffff0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0xffffffff)
    if	(do_g)	
	PR_DOUBLE(0xffffffff, 0xffffffff) 
    if	(do_d)	
	PR_DOUBLE(0xffffffff, 0xffffffff) 
    PAD_TABLE ;

/* POS ULP FACTOR */

    if	(do_s)
	PR_SINGLE(0x34000000) 
    if	(do_t)	
	PR_DOUBLE(0x3cb00000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0x3f8f0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x00003500) 
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00003cd0)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00002500)
    PAD_TABLE ;

/* NEG ULP FACTOR */

    if	(do_s)
	PR_SINGLE(0xb4000000) 
    if	(do_t)	
	PR_DOUBLE(0xbcb00000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0xbf8f0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x0000b500) 
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x0000bcd0)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x0000a500)
    PAD_TABLE ;

/* POS ONE */

    if	(do_s)
	PR_SINGLE(0x3f800000)
    if	(do_t)	
	PR_DOUBLE(0x3ff00000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0x3fff0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x00004080)
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x00004010)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x00004080)
    PAD_TABLE ;

/* NEG ONE */

    if	(do_s)
	PR_SINGLE(0xbf800000)
    if	(do_t)	
	PR_DOUBLE(0xbff00000, 0x00000000)
    if	(do_x)	
	PR_QUAD(0xbfff0000, 0x00000000, 0x00000000, 0x00000000)
    if	(do_f)
	PR_SINGLE(0x0000c080)
    if	(do_g)	
	PR_DOUBLE(0x00000000, 0x0000c010)
    if	(do_d)	
	PR_DOUBLE(0x00000000, 0x0000c080)
    PAD_TABLE ;

    END_TABLE;

    printf("#else\n\n");

    printf("	extern TABLE_UNION GLOBALS_TABLE[];\n");

    printf("#endif\n\n");

    /*
     * Print out defines so that other routines can access the tables
     * Specifically, for each generic value in the globals table  
     * generate a type independent symbolic constant (these are only used
     * by dpml_error_codes.c).
     */

#   define DEFINE_INDEX(n) printf("#define\t" STR(n) "_INDEX\t%i\n", PASTE_2(n, _INDEX))

    DEFINE_INDEX(NAN);
    DEFINE_INDEX(POS_ZERO);
    DEFINE_INDEX(NEG_ZERO);
    DEFINE_INDEX(POS_TINY);
    DEFINE_INDEX(NEG_TINY);
    DEFINE_INDEX(POS_HUGE);
    DEFINE_INDEX(NEG_HUGE);
    DEFINE_INDEX(POS_INFINITY);
    DEFINE_INDEX(NEG_INFINITY);
    DEFINE_INDEX(POS_ULP_FACTOR);
    DEFINE_INDEX(NEG_ULP_FACTOR);
    DEFINE_INDEX(POS_ONE);
    DEFINE_INDEX(NEG_ONE);


    printf("#define\tF_TYPE_ENUM\tPASTE_3(_, F_CHAR, _TYPE)\n");
    if ( bytes_per_table_entry == 32 )
	printf( "#define GLOBALS_OFFSET( t, n ) ( ( t << 3 ) + ( n << 5 ) )\n" ) ;
    else
	printf( "#define GLOBALS_OFFSET( t, n ) ( ( t << 3 ) + ( n << 6 ) )\n" ) ;
    printf("#define\tGLOBAL(n)\t*((F_TYPE *)"
       " ((char *) GLOBALS_TABLE + GLOBALS_OFFSET(F_TYPE_ENUM,n) ))\n");
    printf("#define\tGLOBAL_ADDR(t,n)\t((void *)"
       " ((char *) GLOBALS_TABLE + GLOBALS_OFFSET(t,n) ))\n");

#   define DEFINE_VALUE(n) printf("#define\t" STR(n) \
	"\tGLOBAL(" STR(n) "_INDEX)\n")

    DEFINE_VALUE(NAN);
    DEFINE_VALUE(POS_ZERO);
    DEFINE_VALUE(NEG_ZERO);
    DEFINE_VALUE(POS_TINY);
    DEFINE_VALUE(NEG_TINY);
    DEFINE_VALUE(POS_HUGE);
    DEFINE_VALUE(NEG_HUGE);
    DEFINE_VALUE(POS_INFINITY);
    DEFINE_VALUE(NEG_INFINITY);
    DEFINE_VALUE(POS_ULP_FACTOR);
    DEFINE_VALUE(NEG_ULP_FACTOR);
    DEFINE_VALUE(POS_ONE);
    DEFINE_VALUE(NEG_ONE);

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "DPML global constants", __FILE__ );		\
             print "$headerText\n\n$outText\n";

