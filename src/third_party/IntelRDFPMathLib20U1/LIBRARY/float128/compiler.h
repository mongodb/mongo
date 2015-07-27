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

#ifndef COMPILER_H
#define COMPILER_H

#undef	INLINE
#define	INLINE_DIRECTIVE


#if ((defined(epc_cc) || defined(EPC_CC)) || defined(ecc) || defined(__ECC) || \
      defined(__ICC) || defined(icc) )
#   define __ecc__
#endif




#if (defined(dec_cc) || defined(DEC_CC) || defined(__DECC))

#   define __xxx_dec_cc

#elif (defined(mips_cc) || defined(MIPS_CC) || defined(_CFE) \
	|| (defined(host_mips) && !defined(__GNUC__)) )

#   define __xxx_mips_cc

#elif (defined(vax_cc) || defined(VAX_CC) \
	|| defined(vaxc) || defined(VAXC) || defined(__VAXC))

#   define __xxx_vax_cc

#elif (defined(msc_cc) || defined(MSC_CC) || defined(_MSC_VER)) && !defined(__ICL)

#   define __xxx_msc_cc

#elif (defined(hp_cc) || defined(HP_CC))

#   define __xxx_hp_cc

#elif ((defined(gnu_cc) || defined(GNU_CC) || defined(__GNUC__)) \
         && !defined(__ecc__))

#   define __xxx_gnu_cc

#elif defined(__ecc__)

#   define __xxx_intel_icc

#elif ( defined(__ICL) || defined(icl) )

#   define __xxx_intel_icl

#else

#   define __xxx_just_cc

#endif

#undef  dec_cc
#undef  gnu_cc
#undef  mips_cc
#undef  vax_cc
#undef  msc_cc
#undef  hp_cc
#undef  intel_icc
#undef  intel_icl
#undef  just_cc

#if defined __xxx_dec_cc

#   undef  __xxx_dec_cc 1
#   define dec_cc 1
#   define COMPILER dec_cc

#   define INLINE
#   undef  INLINE_DIRECTIVE
#   define INLINE_DIRECTIVE	static

#elif defined __xxx_mips_cc

#   undef  __xxx_mips_cc
#   define mips_cc 2
#   define COMPILER mips_cc

#elif defined __xxx_vax_cc

#   undef  __xxx_vax_cc
#   define vax_cc 3
#   define COMPILER vax_cc

#elif defined __xxx_msc_cc

#   undef  __xxx_msc_cc
#   define msc_cc 4
#   define COMPILER msc_cc

#elif defined __xxx_hp_cc

#   undef  __xxx_hp_cc
#   define hp_cc 5
#   define COMPILER hp_cc

#elif defined __xxx_gnu_cc

#   define gnu_cc 6
#   define COMPILER gnu_cc

#   define INLINE
#   undef  INLINE_DIRECTIVE
#   define INLINE_DIRECTIVE	static __inline__

#elif defined __xxx_intel_icc

#   undef  __xxx_intel_icc
#   define intel_icc 7 
#   define COMPILER intel_icc

#elif defined __xxx_intel_icl

#   undef  __xxx_intel_icl
#   define intel_icl 8 
#   define COMPILER intel_icl

#else

#   undef  __xxx_just_cc
#   define just_cc 9 
#   define COMPILER just_cc

#endif


#define NULL_MACRO(a) a


#define tmp_TRY 0
#define tmp_THIS + 0
#define tmp_TRYtmp_THIS 1


#ifndef GLUE
#	define GLUE(a,b) a/**/b
#	if (GLUE(tmp_TRY,tmp_THIS) != tmp_TRYtmp_THIS)
#		undef GLUE
#	endif
#endif

#ifndef GLUE
#	define GLUE(a,b) a ## b
#	if (GLUE(tmp_TRY,tmp_THIS) != tmp_TRYtmp_THIS)
#		undef GLUE
#	endif
#endif

#ifndef GLUE
#	define GLUE(a,b) NULL_MACRO(a)b
#	if (GLUE(tmp_TRY,tmp_THIS) != tmp_TRYtmp_THIS)
#		undef GLUE
#	endif
#endif

#ifndef GLUE
#	error GLUE macro not defined
#endif

#undef tmp_TRY
#undef tmp_THIS
#undef tmp_TRYtmp_THIS

#define PASTE(a,b) GLUE(a,b)
#define PASTE_2(a,b) PASTE(a,b)
#define PASTE_3(a,b,c) PASTE(PASTE(a,b),c)
#define PASTE_4(a,b,c,d) PASTE(PASTE(PASTE(a,b),c),d)
#define PASTE_5(a,b,c,d,e) PASTE(PASTE(PASTE(PASTE(a,b),c),d),e)
#define PASTE_6(a,b,c,d,e,f) PASTE(PASTE(PASTE(PASTE(PASTE(a,b),c),d),e),f)
#define PASTE_7(a,b,c,d,e,f,g) PASTE(PASTE(PASTE(PASTE(PASTE(PASTE(a,b),c),d),e),f),g)


#define QUOTE_IT(s) #s

/* Defining QUOTE_IT(s) to be "s" might work with some compilers. */

#define STR(s) QUOTE_IT(s)


// =============================================================================
// At higher optimization levels, some compilers will ignore parenthesis and 
// re-arrange floating point calculation. Doing so will break some of the
// algorithms in the DPML (eg. divide). The intel compiler in particular has
// this problem. However, the Intel compiler has an opperator to avoid 
// reassociations.  
// =============================================================================
 
#if  COMPILER == intel_icc || COMPILER == intel_icl
#   define GROUP(x)     __fence(x)
#endif


#if (COMPILER == dec_cc)

/* Declare decc linkages for various platforms */



#	if ((OP_SYSTEM == osf) || (OP_SYSTEM == linux))

#		if ((OP_SYSTEM == linux) || ( __DECC_VER >= 60000000 ))
#			pragma message disable (nofntpdefdecl)
#		endif


#		if 0
		/*
		 * For reference, the Alpha AXP Calling Standard has:
		 */
#		pragma linkage standard_linkage = (
			parameters(	r16, r17, r18, r19, r20, r21,
					f16, f17, f18, f19, f20, f21 ),
			result(		r0, f0, f1 ),
			/**/
			nopreserve (	r0, r1, r2, r3, r4, r5, r6, r7, r8 ),
			  preserved(	r9, r10, r11, r12, r13, r14 ),
			  preserved(	r15 ),				/* Frame Pointer */
			nopreserve (	r16, r17, r18, r19, r20, r21 ),	/* Parameters */
			nopreserve (	r22, r23, r24, r25 ),
			  preserved(	r26 ),				/* Return Address */
			nopreserve (	r27 ),				/* Bound Procedure Value */
			nopreserve (	r28 ),				/* Volatile Scratch */
			nopreserve (	r29 ), 				/* Global Pointer */
			  preserved(	r30 ),				/* Stack Pointer */
			/*preserved(	r31 ),*/			/* Read as Zero */
			nopreserve (	f0, f1 ),
			  preserved(	f2, f3, f4, f5, f6, f7, f8, f9 ),
			nopreserve (	f10, f11, f12, f13, f14, f15 ),
			nopreserve (	f16, f17, f18, f19, f20, f21 ),	/* Parameters */
			nopreserve (	f22, f23, f24, f25, f26, f27 ),
			nopreserve (	f28, f29, f30 ),
			/*preserved(	f31 ),*/			/* Read as Zero */
                        notneeded(ai) )
#		endif


#		pragma linkage complex_linkage = ( result (f0, f1) )

#		pragma use_linkage complex_linkage ( \
			F_sincosd, F_sincosdf, \
			F_sincos, F_sincosf, ccos, ccosf, cdiv, cdivf, cexp, cexpf, clog, clogf, \
			cmul, cmulf, cpow, cpowf, cpowi, cpowif, csin, csinf, csqrt, csqrtf, \
			r_ccos, r_ccosf, r_cexp, r_cexpf, r_clog, r_clogf, r_cmplx, r_cmplxf, \
			r_conjg, r_conjgf, r_csin, r_csinf, r_csqrt, r_csqrtf, sincos, sincos_vo, \
			sincosd, sincosdf, sincosf, sincosf_vo, sinhcosh, sinhcoshf, \
			csinh, ctan, ctanh, ccosh, catanh, catan, casin, casinh, cacos, \
			cacosh, conj, cproj, ccoshf, catanf, csinhf,\
			ctanf, ctanhf,catanhf, casinf, casinhf, cacosf, cacoshf, conjf, cprojf \
		)


#		pragma linkage res_vec_4_linkage = ( result (f20, f21, f22, f23) )

#		pragma use_linkage res_vec_4_linkage ( \
			__F_sqrt4, __F_sqrt4f, \
			__rsqrt4,  __rsqrt4f, \
			__sqrt4,   __sqrt4f \
		)


                /*
                **  The trig reduce functions can have a bad effect on routines
                **  that call them, because the DECC compiler saves registers
                **  for _any_ path through the routine, rather than deferring
                **  until it's known whether a call to a trig reduce function
                **  is actually needed.
                **
                **  To avoid this problem (which is likely to be inherent in most
		**  compilers), we specify a linkage for the trig reduce functions
		**  that allows their callers (nearly) maximal freedom in register use.
		**  I.e., specify that they preserve (nearly) all registers.
                */
#               pragma linkage trig_reduce_linkage = ( \
                        parameters (f0, r0, r1, r2), \
                        result (r0), \
		       	  preserved(	r16, r17, r18, r19, r20, r21 ),	/* Parameters */	\
		       	  preserved(	f16, f17, f18, f19, f20, f21 ),	/* Parameters */	\
		       	  preserved(	f22, f23, f24, f25, f26, f27 ),				\
		       	  preserved(	f28, f29, f30 ),					\
                        notneeded(ai)								\
                )
#               pragma linkage trigd_reduce_linkage = ( \
                        parameters (f0, r0, r1), \
                        result (r0), \
		       	  preserved(	r16, r17, r18, r19, r20, r21 ),	/* Parameters */	\
		       	  preserved(	f16, f17, f18, f19, f20, f21 ),	/* Parameters */	\
		       	  preserved(	f22, f23, f24, f25, f26, f27 ),				\
		       	  preserved(	f28, f29, f30 ),					\
                        notneeded(ai)								\
                )
#               pragma linkage trig_reduce_linkage_l = ( \
                        parameters (r3, r0, r1, r2), \
                        result (r0), \
		       	  preserved(	r16, r17, r18, r19, r20, r21 ),	/* Parameters */	\
		       	  preserved(	f16, f17, f18, f19, f20, f21 ),	/* Parameters */	\
		       	  preserved(	f22, f23, f24, f25, f26, f27 ),				\
		       	  preserved(	f28, f29, f30 ),					\
                        notneeded(ai)								\
                )

#               pragma use_linkage trig_reduce_linkage ( \
                        __trig_reduce, \
                        __trig_reducef \
		)
		
#               pragma use_linkage trigd_reduce_linkage ( \
                        __trigd_reduce, \
                        __trigd_reducef \
		)
		
		/* some recent decc compilers can not do this */
/* #               pragma use_linkage trig_reduce_linkage_l ( __trig_reducel, __trigd_reducel ) */

#	endif  /* OSF */


	/*
	** NOTE: the "&&" clause is to turn off the pragma definitions for iVMS when
	** compiling f, g or d floating types because the compiler issues an error
	*/

#	if (OP_SYSTEM == vms)

#		if ( __DECC_VER >= 60260000 )
#			pragma message disable (nofntpdefdecl)
#		endif
#		if ( __ia64__ )
#			pragma message disable (showmaplinkage,mapregignored)
#		endif

#               if __ia64__ && !__IEEE_FLOAT
#		    pragma linkage complex_linkage = ( result (r0, r1) )
#               else
#		    pragma linkage complex_linkage = ( result (f0, f1) )
#               endif

#		pragma use_linkage complex_linkage   ( \
                        math$cacos_f, math$cacos_g, math$cacos_s, math$cacos_t, \
                        math$cacosh_f, math$cacosh_g, math$cacosh_s, math$cacosh_t, \
                        math$casin_f, math$casin_g, math$casin_s, math$casin_t, \
                        math$casinh_f, math$casinh_g, math$casinh_s, math$casinh_t, \
                        math$catan_f, math$catan_g, math$catan_s, math$catan_t, \
                        math$catanh_f, math$catanh_g, math$catanh_s, math$catanh_t, \
                        math$ccosh_f, math$ccosh_g, math$ccosh_s, math$ccosh_t, \
                        math$ctanh_f, math$ctanh_g, math$ctanh_s, math$ctanh_t, \
                        math$csinh_f, math$csinh_g, math$csinh_s, math$csinh_t, \
                        math$ctan_f, math$ctan_g, math$ctan_s, math$ctan_t, \
                        math$conj_f, math$conj_g, math$conj_s, math$conj_t, \
                        math$cproj_f, math$cproj_g, math$cproj_s, math$cproj_t, \
			math$F_sincosd_f, math$F_sincosd_g, math$F_sincosd_s, math$F_sincosd_t, \
			math$F_sincos_f, math$F_sincos_g, math$F_sincos_s, math$F_sincos_t, \
			math$ccos_f, math$ccos_g, math$ccos_s, math$ccos_t, math$cdiv_f, \
			math$cdiv_g, math$cdiv_s, math$cdiv_t, math$cexp_f, math$cexp_g, \
			math$cexp_s, math$cexp_t, math$clog_f, math$clog_g, math$clog_s, \
			math$clog_t, math$cmul_f, math$cmul_g, math$cmul_s, math$cmul_t, \
			math$cpow_f, math$cpow_g, math$cpow_s, math$cpow_t, math$cpow_fq, \
			math$cpow_gq, math$cpow_sq, math$cpow_tq, math$cpowi_f, math$cpowi_g, \
			math$cpowi_s, math$cpowi_t, math$csin_f, math$csin_g, math$csin_s, \
			math$csin_t, math$csqrt_f, math$csqrt_g, math$csqrt_s, math$csqrt_t, \
			math$sincos_f, math$sincos_g, math$sincos_s, math$sincos_t, \
			math$sincos_vo_f, math$sincos_vo_g, math$sincos_vo_s, math$sincos_vo_t, \
			math$sincosd_f, math$sincosd_g, math$sincosd_s, math$sincosd_t, \
			math$sinhcosh_f, math$sinhcosh_g, math$sinhcosh_s, math$sinhcosh_t, \
			mth$ccos, mth$cdcos, mth$cdexp, mth$cdlog, mth$cdsin, mth$cdsqrt, mth$cexp, \
			mth$cgcos, mth$cgexp, mth$cglog, mth$cgsin, mth$cgsqrt, mth$clog, \
			mth$cmplx, mth$conjg, mth$cscos, mth$csexp, mth$csin, mth$cslog, mth$csqrt, \
			mth$cssin, mth$cssqrt, mth$ctcos, mth$ctexp, mth$ctlog, mth$ctsin, \
			mth$ctsqrt, mth$dcmplx, mth$dconjg, mth$gcmplx, mth$gconjg, mth$scmplx, \
			mth$sconjg, mth$tcmplx, mth$tconjg, ots$divc, ots$divcd_r3, ots$divcg_r3, \
			ots$mulc, ots$mulcd_r3, ots$mulcg_r3, ots$powcc, ots$powcc_r3, \
			ots$powcdcd_r3, ots$powcdj, ots$powcgcg_r3, ots$powcgj, ots$powcj \
		)


#		define PRESERVED_REGISTERS preserved( \
			r1, \
			r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, \
			f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, \
			f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, \
			f30 \
		)

#               if !__ia64__ || __IEEE_FLOAT
	
#			pragma linkage trig_reduce_linkage = ( \
				parameters (f0, r0, r1, r16), \
				result (r0), \
				PRESERVED_REGISTERS, \
				notneeded(ai) \
			)
	
#			pragma use_linkage trig_reduce_linkage ( \
				math$trig_reduce_f, \
				math$trig_reduce_g, \
				math$trig_reduce_s, \
				math$trig_reduce_t  \
			)
	
#			pragma linkage trigd_reduce_linkage = ( \
				parameters (f0, r0, r1), \
				result (r0), \
				PRESERVED_REGISTERS, \
				notneeded(ai) \
			)
	
#			pragma use_linkage trigd_reduce_linkage ( \
				math$trigd_reduce_f, \
				math$trigd_reduce_g, \
				math$trigd_reduce_s, \
				math$trigd_reduce_t  \
			)
#	               endif

#	endif  /* VMS */

#endif  /* (COMPILER == dec_cc) */

#endif  /* COMPILER_H */
