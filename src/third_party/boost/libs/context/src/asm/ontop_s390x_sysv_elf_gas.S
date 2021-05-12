/*******************************************************
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |    24     |  *
 *  -------------------------------------------------  *
 *  |   t.fctx  |   t.data  |    r2     |    r6     |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |     r7    |     r8    |     r9    |    r10    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |     64    |     72    |     80    |     88    |  *
 *  -------------------------------------------------  *
 *  |    r11    |    r12    |    r13    |    r14    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 | 29  |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |     96    |    104    |    112    |    120    |  *
 *  -------------------------------------------------  *
 *  |     f8    |     f9    |    f10    |    f11    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  |    128    |    136    |    144    |    152    |  *
 *  -------------------------------------------------  *
 *  |    f12    |    f13    |    f14    |    f15    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  40 |  41 |  42 |  43 |  44 |  45 |  46 |  47 |  *
 *  -------------------------------------------------  *
 *  |    160    |    168    |    176    |           |  *
 *  -------------------------------------------------  *
 *  |    fpc    |     pc    |           |           |  *
 *  -------------------------------------------------  *
 *******************************************************/

.text
.align  8
.global ontop_fcontext
.type   ontop_fcontext, @function

#define ARG_OFFSET         0
#define GR_OFFSET	   16
#define R14_OFFSET	   88
#define FP_OFFSET	   96
#define FPC_OFFSET	   160
#define PC_OFFSET	   168
#define CONTEXT_SIZE	   176


/*

typedef void*   fcontext_t;

struct transfer_t {
   fcontext_t  fctx;
   void    *   data;
};

transfer_t ontop_fcontext( fcontext_t const to,
			   void * vp,
			   transfer_t (* fn)( transfer_t) );

Incoming args
r2 - Hidden argument to the location where the return transfer_t needs to be returned
r3 - Target context
r4 - Data pointer
r5 - Function to be executed

This implementation assumes that ontop_fcontext will never be called with target contexts
created via make_fcontext.

*/

ontop_fcontext:
	/* Reserve stack space to store the current context.  */
	aghi	%r15,-CONTEXT_SIZE

	/* Save the argument register holding the location of the return value.  */
	stg	%r2,GR_OFFSET(%r15)

	/* Save the call-saved general purpose registers.  */
	stmg	%r6,%r14,GR_OFFSET+8(%r15)

	/* Save call-saved floating point registers.  */
	std	%f8,FP_OFFSET(%r15)
	std	%f9,FP_OFFSET+8(%r15)
	std	%f10,FP_OFFSET+16(%r15)
	std	%f11,FP_OFFSET+24(%r15)
	std	%f12,FP_OFFSET+32(%r15)
	std	%f13,FP_OFFSET+40(%r15)
	std	%f14,FP_OFFSET+48(%r15)
	std	%f15,FP_OFFSET+56(%r15)

	/* Save the return address as current pc.  */
	stg	%r14,PC_OFFSET(%r15)

	/* Save the floating point control register.  */
	stfpc   FPC_OFFSET(%r15)

	/* Backup the stack pointer pointing to the old context-data into r1.  */
	lgr	%r1,%r15

	/* Load the new context pointer as stack pointer.  */
	lgr	%r15,%r3

	/* Restore the call-saved GPRs from the new context.  */
	lmg	%r6,%r14,GR_OFFSET+8(%r15)

	/* Restore call-saved floating point registers.  */
	ld	%f8,FP_OFFSET(%r15)
	ld	%f9,FP_OFFSET+8(%r15)
	ld	%f10,FP_OFFSET+16(%r15)
	ld	%f11,FP_OFFSET+24(%r15)
	ld	%f12,FP_OFFSET+32(%r15)
	ld	%f13,FP_OFFSET+40(%r15)
	ld	%f14,FP_OFFSET+48(%r15)
	ld	%f15,FP_OFFSET+56(%r15)

	/* Load the floating point control register.  */
	lfpc   FPC_OFFSET(%r15)

	/* Store the transfer_t values located in the saved context.  */
	stg	%r1,0(%r1)	       /* transfer_t.fctx = old context */
	stg	%r4,8(%r1)             /* transfer_t.data = data */

	/* Set up the arguments for the target function.  */
	lg	%r2,GR_OFFSET(%r15)
	lgr	%r3,%r1

	/* Deallocate the context.  */
	aghi	%r15,CONTEXT_SIZE

	br	%r5

.size   ontop_fcontext,.-ontop_fcontext
.section .note.GNU-stack,"",%progbits
