crc32-vpmsum
============

A set of examples for accelerating CRC32 calculations using the vector
polynomial multiply sum (vpmsum) instructions introduced in POWER8. These
instructions implement byte, halfword, word and doubleword carryless
multiply/add.

Performance
-----------

An implementation of slice-by-8, one of the fastest lookup table methods
is included so we can compare performance against it. Testing 5000000
iterations of a CRC of 32 kB of data (to keep it L1 cache contained):

```
# time slice_by_8_bench 32768 5000000
122.220 seconds

# time crc32_bench 32768 5000000
2.937 seconds
```

The vpmsum accelerated CRC is just over 41x faster.

This test was run on a 4.1 GHz POWER8, so the algorithm sustains about
52 GiB/sec or 13.6 bytes/cycle. The theoretical limit is 16 bytes/cycle
since we can execute a maximum of one vpmsum instruction per cycle.

In another test, a version was added to the kernel and btrfs write
performance was shown to be 3.8x faster. The test was done to a ramdisk
to mitigate any I/O induced variability.

Quick start
-----------

There's two different versions of crc32. They are, basically, the same
algorithm. The only difference is that one is implemented in pure assembly
(crc32.S) and the other in C using gcc (power8) vector intrinsics and
builtins (vec_crc32.c) to make the compiler generate the asm instructions
instead.

- Modify CRC and OPTIONS in the Makefile. There are examples for the two most
  common crc32s.

- Type make to create the constants (crc32_constants.h)

**If you will use the pure asm version**

- Import the code into your application (crc32.S crc32_wrapper.c
  crc32_constants.h ppc-opcode.h)

**If you will use the C version**

- Import the code into your application (vec_crc32.c crc32_constants.h)

- Call the CRC:


```
unsigned int crc32_vpmsum(unsigned int crc, unsigned char *p, unsigned long len);
```

Advanced Usage
--------------

Occasionally you may have a number of CRC32 polynomial implementations.

To do this you'll need to compile the C or assembler implementation with a
different constants header file and change the function names to avoid linker
conflicts.

To facilitate this optional defines can be introduced:

- CRC32_CONSTANTS_HEADER to be set to the *quoted* header filename.

- CRC32_FUNCTION to be set to the crc32 function name (instead of crc32_vpmsum)

- CRC32_FUNCTION_ASM (asm version only) to be set to the assember function name used
by crc32_wrapper.c (defaults to __crc32_vpmsum).

An example of this is with crc32_two_implementations as found in the Makefile.

CRC background
--------------

For a good background on CRCs, check out:

http://www.ross.net/crc/download/crc_v3.txt

A few key points:

- A CRC is the remainder after dividing a message by the CRC polynomial,
  ie M mod CRC_POLY
- multiply/divide is carryless
- add/subtract is an xor
- n (where n is the order of the CRC) bits of zeroes are appended to the
  end of the message.

One more important piece of information - a CRC is a linear function, so:

```
	CRC(A xor B) = CRC(A) xor CRC(B)

	CRC(A . B) = CRC(A) . CRC(B)	(remember this is carryless multiply)
```

If we take 64bits of data, represented by two 32 bit chunks (AAAAAAAA
and BBBBBBBB):

```
CRC(AAAAAAAABBBBBBBB)
	= CRC(AAAAAAAA00000000 xor BBBBBBBB)
	= CRC(AAAAAAAA00000000) xor CRC(BBBBBBBB)
```

If we operate on AAAAAAAA:

```
CRC(AAAAAAAA00000000)
	= CRC(AAAAAAAA . 100000000)
	= CRC(AAAAAAAA) . CRC(100000000)
```

And CRC(100000000) is a constant which we can pre-calculate:

```
CRC(100000000)
	= 100000000 mod CRC_POLY
	= 2^32 mod CRC_POLY
```

Finally we can add our modified AAAAAAAA to BBBBBBBB:

```
CRC(AAAAAAAABBBBBBBB)
	= ((2^32 mod CRC_POLY) . CRC(AAAAAAAA)) xor CRC(BBBBBBBB)
```

In other words, with the right constants pre-calculated we can shift the
input data around and we can also calculate the CRC in as many parallel
chunks as we want.

No matter how much shifting we do, the final result will be be 64 bits of
data (63 actually, because there is no carry into the top bit). To reduce
it further we need a another trick, and that is Barrett reduction:

http://en.wikipedia.org/wiki/Barrett_reduction

Barrett reduction is a method of calculating a mod n. The idea is to
calculate q, the multiple of our polynomial that we need to subtract. By
doing the computation 2x bits higher (ie 64 bits) and shifting the
result back down 2x bits, we round down to the nearest multiple.

```
	k = 32
	m = floor((4^k)/n) = floor((4^32))/n)
	n = 64 bits of data
	a = 32 bit CRC

	q = floor(ma/(2^64))
	result = a - qn
```

An example in the floating point domain makes it clearer how this works:

```
a mod n = a - floor(am) * n
```

Let's use it to calculate 22 mod 10:

```
	a = 22
	n = 10
	m = 1/n = 1/10 = 0.1

22 mod 10
	= 22 - floor(22*0.1) * 10
	= 22 - 2 * 10
	= 22 - 20
	= 2
```

There is one more issue left - bit reflection. Some CRCs are defined to
operate on the least significant bit first (eg CRC32c). Lets look at
how this would get laid out in a register, and lets simplify it to just
two bytes (vs a 16 byte VMX register):

    [ 8..15 ] [ 0..7 ]

Notice how the bits and bytes are out of order. Since we are doing
multi word multiplication on these values we need them to both be
in order.

The simplest way to fix this is to reflect the bits in each byte:

    [ 15..8 ] [ 7..0 ]

However shuffling bits in a byte is expensive on most CPUs. It is
however relatively cheap to shuffle bytes around. What if we load
the bytes in reversed:

    [ 0..7 ] [ 8..15 ]

Now the bits and bytes are in order, except the least significant bit
of the register is now on the left and the most significant bit is on the
right. We operate as if the register is reflected, which normally we
cannot do. The reason we get away with this is our multiplies are carryless
and our addition and subtraction is xor, so our operations never create
carries.

The only trick is we have to shift the result of multiplies left one
because the high bit of the multiply is always 0, and we want that high bit
on the right not the left.

Implementation
--------------

The vpmsum instructions on POWER8 have a 6 cycle latency and we can
execute one every cycle. In light of this the main loop has 8 parallel
streams which consume 8 x 16 B each iteration. At the completion of this
loop we have taken 32 kB of data and reduced it to 8 x 16 B (128 B).

The next step is to take this 128 B and reduce it to 8 B. At this stage
we also add 32 bits of 0 to the end.

We then apply Barrett reduction to get our CRC.

Examples
--------
- barrett_reduction: An example of Barrett reduction

- final_fold: Starting with 128 bits, add 32 bits of zeros and reduce it to
  64 bits, then apply Barrett reduction

- final_fold2: A second method of reduction

Run time detection
------------------

The kernel sets the PPC_FEATURE2_VEC_CRYPTO bit in the HWCAP2 field
when the vpmsum instructions are available. An example of run time
detection:

```
#include <sys/auxv.h>

#ifndef PPC_FEATURE2_VEC_CRYPTO
#define PPC_FEATURE2_VEC_CRYPTO	0x02000000
#endif

#ifndef AT_HWCAP2
#define AT_HWCAP2	26
#endif

...

	if (getauxval(AT_HWCAP2) & PPC_FEATURE2_VEC_CRYPTO) {
		/* Use crc32-vpmsum optimised version */
	} else {
		/* fall back to non accelerated version */
	}
```

Acknowledgements
----------------

Thanks to Michael Gschwind, Jeff Derby, Lorena Pesantez and Stewart Smith
for their ideas and assistance.

Thanks Rogerio Alves for writing the C implementation.

Thanks Daniel Black for cleanup and testing.
