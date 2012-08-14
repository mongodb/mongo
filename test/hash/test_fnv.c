/*
 * test_fnv - FNV test suite
 *
 * @(#) $Revision: 5.3 $
 * @(#) $Id: test_fnv.c,v 5.3 2009/06/30 11:50:41 chongo Exp $
 * @(#) $Source: /usr/local/src/cmd/fnv/RCS/test_fnv.c,v $
 *
 ***
 *
 * Fowler/Noll/Vo hash
 *
 * The basis of this hash algorithm was taken from an idea sent
 * as reviewer comments to the IEEE POSIX P1003.2 committee by:
 *
 *      Phong Vo (http://www.research.att.com/info/kpv/)
 *      Glenn Fowler (http://www.research.att.com/~gsf/)
 *
 * In a subsequent ballot round:
 *
 *      Landon Curt Noll (http://www.isthe.com/chongo/)
 *
 * improved on their algorithm.  Some people tried this hash
 * and found that it worked rather well.  In an EMail message
 * to Landon, they named it the ``Fowler/Noll/Vo'' or FNV hash.
 *
 * FNV hashes are designed to be fast while maintaining a low
 * collision rate. The FNV speed allows one to quickly hash lots
 * of data while maintaining a reasonable collision rate.  See:
 *
 *      http://www.isthe.com/chongo/tech/comp/fnv/index.html
 *
 * for more details as well as other forms of the FNV hash.
 *
 ***
 *
 * Please do not copyright this code.  This code is in the public domain.
 *
 * LANDON CURT NOLL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
 * EVENT SHALL LANDON CURT NOLL BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * By:
 *	chongo <Landon Curt Noll> /\oo/\
 *      http://www.isthe.com/chongo/
 *
 * Share and Enjoy!	:-)
 */

#include <stdio.h>
#include <stdlib.h>
#include "wt_internal.h"
#include "hash.h"

#define	LEN(x) (sizeof(x)-1)
/* TEST macro does not include trailing NUL byte in the test vector */
#define	TEST(x) {x, LEN(x)}
/* TEST0 macro includes the trailing NUL byte in the test vector */
#define	TEST0(x) {x, sizeof(x)}
/* REPEAT500 - repeat a string 500 times */
#define	R500(x) R100(x)R100(x)R100(x)R100(x)R100(x)
#define	R100(x) R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)
#define	R10(x) x x x x x x x x x x

struct test_vector {
    void *buf;		/* start of test vector buffer */
    int len;		/* length of test vector */
};
struct fnv1a_64_test_vector {
    struct test_vector *test;	/* test vector buffer to hash */
    Fnv64_t fnv1a_64;		/* expected FNV-1a 64 bit hash value */
};
#define	FNV_VERSION "5.0.2"	/* @(#) FNV Version */
extern struct test_vector fnv_test_str[];
extern struct fnv1a_64_test_vector fnv1a_64_vector[];

/*
 * FNV test vectors
 *
 * NOTE: A NULL pointer marks beyond the end of the test vectors.
 *
 * NOTE: The order of the fnv_test_str[] test vectors is 1-to-1 with:
 *
 *	struct fnv0_32_test_vector fnv0_32_vector[];
 *	struct fnv1_32_test_vector fnv1_32_vector[];
 *	struct fnv1a_32_test_vector fnv1a_32_vector[];
 *	struct fnv0_64_test_vector fnv0_64_vector[];
 *	struct fnv1_64_test_vector fnv1_64_vector[];
 *	struct fnv1a_64_test_vector fnv1a_64_vector[];
 *
 * IMPORTANT NOTE:
 *
 *	If you change the fnv_test_str[] array, you need
 *	to also change ALL of the above fnv*_vector arrays!!!
 *
 *	To rebuild, try:
 *
 *		make vector.c
 *
 *	and then fold the results into the source file.
 *	Of course, you better make sure that the vaules
 *	produced by the above command are valid, otherwise
 * 	you will be testing against invalid vectors!
 */
struct test_vector fnv_test_str[] = {
	TEST(""),
	TEST("a"),
	TEST("b"),
	TEST("c"),
	TEST("d"),
	TEST("e"),
	TEST("f"),
	TEST("fo"),
	TEST("foo"),
	TEST("foob"),
	TEST("fooba"),
	TEST("foobar"),
	TEST0(""),
	TEST0("a"),
	TEST0("b"),
	TEST0("c"),
	TEST0("d"),
	TEST0("e"),
	TEST0("f"),
	TEST0("fo"),
	TEST0("foo"),
	TEST0("foob"),
	TEST0("fooba"),
	TEST0("foobar"),
	TEST("ch"),
	TEST("cho"),
	TEST("chon"),
	TEST("chong"),
	TEST("chongo"),
	TEST("chongo "),
	TEST("chongo w"),
	TEST("chongo wa"),
	TEST("chongo was"),
	TEST("chongo was "),
	TEST("chongo was h"),
	TEST("chongo was he"),
	TEST("chongo was her"),
	TEST("chongo was here"),
	TEST("chongo was here!"),
	TEST("chongo was here!\n"),
	TEST0("ch"),
	TEST0("cho"),
	TEST0("chon"),
	TEST0("chong"),
	TEST0("chongo"),
	TEST0("chongo "),
	TEST0("chongo w"),
	TEST0("chongo wa"),
	TEST0("chongo was"),
	TEST0("chongo was "),
	TEST0("chongo was h"),
	TEST0("chongo was he"),
	TEST0("chongo was her"),
	TEST0("chongo was here"),
	TEST0("chongo was here!"),
	TEST0("chongo was here!\n"),
	TEST("cu"),
	TEST("cur"),
	TEST("curd"),
	TEST("curds"),
	TEST("curds "),
	TEST("curds a"),
	TEST("curds an"),
	TEST("curds and"),
	TEST("curds and "),
	TEST("curds and w"),
	TEST("curds and wh"),
	TEST("curds and whe"),
	TEST("curds and whey"),
	TEST("curds and whey\n"),
	TEST0("cu"),
	TEST0("cur"),
	TEST0("curd"),
	TEST0("curds"),
	TEST0("curds "),
	TEST0("curds a"),
	TEST0("curds an"),
	TEST0("curds and"),
	TEST0("curds and "),
	TEST0("curds and w"),
	TEST0("curds and wh"),
	TEST0("curds and whe"),
	TEST0("curds and whey"),
	TEST0("curds and whey\n"),
	TEST("hi"), TEST0("hi"),
	TEST("hello"), TEST0("hello"),
	TEST("\xff\x00\x00\x01"), TEST("\x01\x00\x00\xff"),
	TEST("\xff\x00\x00\x02"), TEST("\x02\x00\x00\xff"),
	TEST("\xff\x00\x00\x03"), TEST("\x03\x00\x00\xff"),
	TEST("\xff\x00\x00\x04"), TEST("\x04\x00\x00\xff"),
	TEST("\x40\x51\x4e\x44"), TEST("\x44\x4e\x51\x40"),
	TEST("\x40\x51\x4e\x4a"), TEST("\x4a\x4e\x51\x40"),
	TEST("\x40\x51\x4e\x54"), TEST("\x54\x4e\x51\x40"),
	TEST("127.0.0.1"), TEST0("127.0.0.1"),
	TEST("127.0.0.2"), TEST0("127.0.0.2"),
	TEST("127.0.0.3"), TEST0("127.0.0.3"),
	TEST("64.81.78.68"), TEST0("64.81.78.68"),
	TEST("64.81.78.74"), TEST0("64.81.78.74"),
	TEST("64.81.78.84"), TEST0("64.81.78.84"),
	TEST("feedface"), TEST0("feedface"),
	TEST("feedfacedaffdeed"), TEST0("feedfacedaffdeed"),
	TEST("feedfacedeadbeef"), TEST0("feedfacedeadbeef"),
	TEST("line 1\nline 2\nline 3"),
	TEST("chongo <Landon Curt Noll> /\\../\\"),
	TEST0("chongo <Landon Curt Noll> /\\../\\"),
	TEST("chongo (Landon Curt Noll) /\\../\\"),
	TEST0("chongo (Landon Curt Noll) /\\../\\"),
	TEST("http://antwrp.gsfc.nasa.gov/apod/astropix.html"),
	TEST("http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash"),
	TEST("http://epod.usra.edu/"),
	TEST("http://exoplanet.eu/"),
	TEST("http://hvo.wr.usgs.gov/cam3/"),
	TEST("http://hvo.wr.usgs.gov/cams/HMcam/"),
	TEST("http://hvo.wr.usgs.gov/kilauea/update/deformation.html"),
	TEST("http://hvo.wr.usgs.gov/kilauea/update/images.html"),
	TEST("http://hvo.wr.usgs.gov/kilauea/update/maps.html"),
	TEST("http://hvo.wr.usgs.gov/volcanowatch/current_issue.html"),
	TEST("http://neo.jpl.nasa.gov/risk/"),
	TEST("http://norvig.com/21-days.html"),
	TEST("http://primes.utm.edu/curios/home.php"),
	TEST("http://slashdot.org/"),
	TEST("http://tux.wr.usgs.gov/Maps/155.25-19.5.html"),
	TEST("http://volcano.wr.usgs.gov/kilaueastatus.php"),
	TEST("http://www.avo.alaska.edu/activity/Redoubt.php"),
	TEST("http://www.dilbert.com/fast/"),
	TEST("http://www.fourmilab.ch/gravitation/orbits/"),
	TEST("http://www.fpoa.net/"),
	TEST("http://www.ioccc.org/index.html"),
	TEST("http://www.isthe.com/cgi-bin/number.cgi"),
	TEST("http://www.isthe.com/chongo/bio.html"),
	TEST("http://www.isthe.com/chongo/index.html"),
	TEST("http://www.isthe.com/chongo/src/calc/lucas-calc"),
	TEST("http://www.isthe.com/chongo/tech/astro/venus2004.html"),
	TEST("http://www.isthe.com/chongo/tech/astro/vita.html"),
	TEST("http://www.isthe.com/chongo/tech/comp/c/expert.html"),
	TEST("http://www.isthe.com/chongo/tech/comp/calc/index.html"),
	TEST("http://www.isthe.com/chongo/tech/comp/fnv/index.html"),
	TEST("http://www.isthe.com/chongo/tech/math/number/howhigh.html"),
	TEST("http://www.isthe.com/chongo/tech/math/number/number.html"),
	TEST("http://www.isthe.com/chongo/tech/math/prime/mersenne.html"),
	TEST(
"http://www.isthe.com/chongo/tech/math/prime/mersenne.html#largest"),
	TEST("http://www.lavarnd.org/cgi-bin/corpspeak.cgi"),
	TEST("http://www.lavarnd.org/cgi-bin/haiku.cgi"),
	TEST("http://www.lavarnd.org/cgi-bin/rand-none.cgi"),
	TEST("http://www.lavarnd.org/cgi-bin/randdist.cgi"),
	TEST("http://www.lavarnd.org/index.html"),
	TEST("http://www.lavarnd.org/what/nist-test.html"),
	TEST("http://www.macosxhints.com/"),
	TEST("http://www.mellis.com/"),
	TEST(
"http://www.nature.nps.gov/air/webcams/parks/havoso2alert/havoalert.cfm"),
	TEST(
"http://www.nature.nps.gov/air/webcams/parks/havoso2alert/timelines_24.cfm"),
	TEST("http://www.paulnoll.com/"),
	TEST("http://www.pepysdiary.com/"),
	TEST("http://www.sciencenews.org/index/home/activity/view"),
	TEST("http://www.skyandtelescope.com/"),
	TEST("http://www.sput.nl/~rob/sirius.html"),
	TEST("http://www.systemexperts.com/"),
	TEST("http://www.tq-international.com/phpBB3/index.php"),
	TEST("http://www.travelquesttours.com/index.htm"),
	TEST("http://www.wunderground.com/global/stations/89606.html"),
	TEST(R10("21701")),
	TEST(R10("M21701")),
	TEST(R10("2^21701-1")),
	TEST(R10("\x54\xc5")),
	TEST(R10("\xc5\x54")),
	TEST(R10("23209")),
	TEST(R10("M23209")),
	TEST(R10("2^23209-1")),
	TEST(R10("\x5a\xa9")),
	TEST(R10("\xa9\x5a")),
	TEST(R10("391581216093")),
	TEST(R10("391581*2^216093-1")),
	TEST(R10("\x05\xf9\x9d\x03\x4c\x81")),
	TEST(R10("FEDCBA9876543210")),
	TEST(R10("\xfe\xdc\xba\x98\x76\x54\x32\x10")),
	TEST(R10("EFCDAB8967452301")),
	TEST(R10("\xef\xcd\xab\x89\x67\x45\x23\x01")),
	TEST(R10("0123456789ABCDEF")),
	TEST(R10("\x01\x23\x45\x67\x89\xab\xcd\xef")),
	TEST(R10("1032547698BADCFE")),
	TEST(R10("\x10\x32\x54\x76\x98\xba\xdc\xfe")),
	TEST(R500("\x00")),
	TEST(R500("\x07")),
	TEST(R500("~")),
	TEST(R500("\x7f")),
	{NULL, 0}	/* MUST BE LAST */
};

/*
 * insert the contents of vector.c below
 *
 *	make vector.c
 *	:r vector.c
 */
/* start of output generated by make vector.c */

/* FNV-1a 64 bit test vectors */
struct fnv1a_64_test_vector fnv1a_64_vector[] = {
	{ &fnv_test_str[0], (Fnv64_t) 0xcbf29ce484222325ULL },
	{ &fnv_test_str[1], (Fnv64_t) 0xaf63dc4c8601ec8cULL },
	{ &fnv_test_str[2], (Fnv64_t) 0xaf63df4c8601f1a5ULL },
	{ &fnv_test_str[3], (Fnv64_t) 0xaf63de4c8601eff2ULL },
	{ &fnv_test_str[4], (Fnv64_t) 0xaf63d94c8601e773ULL },
	{ &fnv_test_str[5], (Fnv64_t) 0xaf63d84c8601e5c0ULL },
	{ &fnv_test_str[6], (Fnv64_t) 0xaf63db4c8601ead9ULL },
	{ &fnv_test_str[7], (Fnv64_t) 0x08985907b541d342ULL },
	{ &fnv_test_str[8], (Fnv64_t) 0xdcb27518fed9d577ULL },
	{ &fnv_test_str[9], (Fnv64_t) 0xdd120e790c2512afULL },
	{ &fnv_test_str[10], (Fnv64_t) 0xcac165afa2fef40aULL },
	{ &fnv_test_str[11], (Fnv64_t) 0x85944171f73967e8ULL },
	{ &fnv_test_str[12], (Fnv64_t) 0xaf63bd4c8601b7dfULL },
	{ &fnv_test_str[13], (Fnv64_t) 0x089be207b544f1e4ULL },
	{ &fnv_test_str[14], (Fnv64_t) 0x08a61407b54d9b5fULL },
	{ &fnv_test_str[15], (Fnv64_t) 0x08a2ae07b54ab836ULL },
	{ &fnv_test_str[16], (Fnv64_t) 0x0891b007b53c4869ULL },
	{ &fnv_test_str[17], (Fnv64_t) 0x088e4a07b5396540ULL },
	{ &fnv_test_str[18], (Fnv64_t) 0x08987c07b5420ebbULL },
	{ &fnv_test_str[19], (Fnv64_t) 0xdcb28a18fed9f926ULL },
	{ &fnv_test_str[20], (Fnv64_t) 0xdd1270790c25b935ULL },
	{ &fnv_test_str[21], (Fnv64_t) 0xcac146afa2febf5dULL },
	{ &fnv_test_str[22], (Fnv64_t) 0x8593d371f738acfeULL },
	{ &fnv_test_str[23], (Fnv64_t) 0x34531ca7168b8f38ULL },
	{ &fnv_test_str[24], (Fnv64_t) 0x08a25607b54a22aeULL },
	{ &fnv_test_str[25], (Fnv64_t) 0xf5faf0190cf90df3ULL },
	{ &fnv_test_str[26], (Fnv64_t) 0xf27397910b3221c7ULL },
	{ &fnv_test_str[27], (Fnv64_t) 0x2c8c2b76062f22e0ULL },
	{ &fnv_test_str[28], (Fnv64_t) 0xe150688c8217b8fdULL },
	{ &fnv_test_str[29], (Fnv64_t) 0xf35a83c10e4f1f87ULL },
	{ &fnv_test_str[30], (Fnv64_t) 0xd1edd10b507344d0ULL },
	{ &fnv_test_str[31], (Fnv64_t) 0x2a5ee739b3ddb8c3ULL },
	{ &fnv_test_str[32], (Fnv64_t) 0xdcfb970ca1c0d310ULL },
	{ &fnv_test_str[33], (Fnv64_t) 0x4054da76daa6da90ULL },
	{ &fnv_test_str[34], (Fnv64_t) 0xf70a2ff589861368ULL },
	{ &fnv_test_str[35], (Fnv64_t) 0x4c628b38aed25f17ULL },
	{ &fnv_test_str[36], (Fnv64_t) 0x9dd1f6510f78189fULL },
	{ &fnv_test_str[37], (Fnv64_t) 0xa3de85bd491270ceULL },
	{ &fnv_test_str[38], (Fnv64_t) 0x858e2fa32a55e61dULL },
	{ &fnv_test_str[39], (Fnv64_t) 0x46810940eff5f915ULL },
	{ &fnv_test_str[40], (Fnv64_t) 0xf5fadd190cf8edaaULL },
	{ &fnv_test_str[41], (Fnv64_t) 0xf273ed910b32b3e9ULL },
	{ &fnv_test_str[42], (Fnv64_t) 0x2c8c5276062f6525ULL },
	{ &fnv_test_str[43], (Fnv64_t) 0xe150b98c821842a0ULL },
	{ &fnv_test_str[44], (Fnv64_t) 0xf35aa3c10e4f55e7ULL },
	{ &fnv_test_str[45], (Fnv64_t) 0xd1ed680b50729265ULL },
	{ &fnv_test_str[46], (Fnv64_t) 0x2a5f0639b3dded70ULL },
	{ &fnv_test_str[47], (Fnv64_t) 0xdcfbaa0ca1c0f359ULL },
	{ &fnv_test_str[48], (Fnv64_t) 0x4054ba76daa6a430ULL },
	{ &fnv_test_str[49], (Fnv64_t) 0xf709c7f5898562b0ULL },
	{ &fnv_test_str[50], (Fnv64_t) 0x4c62e638aed2f9b8ULL },
	{ &fnv_test_str[51], (Fnv64_t) 0x9dd1a8510f779415ULL },
	{ &fnv_test_str[52], (Fnv64_t) 0xa3de2abd4911d62dULL },
	{ &fnv_test_str[53], (Fnv64_t) 0x858e0ea32a55ae0aULL },
	{ &fnv_test_str[54], (Fnv64_t) 0x46810f40eff60347ULL },
	{ &fnv_test_str[55], (Fnv64_t) 0xc33bce57bef63eafULL },
	{ &fnv_test_str[56], (Fnv64_t) 0x08a24307b54a0265ULL },
	{ &fnv_test_str[57], (Fnv64_t) 0xf5b9fd190cc18d15ULL },
	{ &fnv_test_str[58], (Fnv64_t) 0x4c968290ace35703ULL },
	{ &fnv_test_str[59], (Fnv64_t) 0x07174bd5c64d9350ULL },
	{ &fnv_test_str[60], (Fnv64_t) 0x5a294c3ff5d18750ULL },
	{ &fnv_test_str[61], (Fnv64_t) 0x05b3c1aeb308b843ULL },
	{ &fnv_test_str[62], (Fnv64_t) 0xb92a48da37d0f477ULL },
	{ &fnv_test_str[63], (Fnv64_t) 0x73cdddccd80ebc49ULL },
	{ &fnv_test_str[64], (Fnv64_t) 0xd58c4c13210a266bULL },
	{ &fnv_test_str[65], (Fnv64_t) 0xe78b6081243ec194ULL },
	{ &fnv_test_str[66], (Fnv64_t) 0xb096f77096a39f34ULL },
	{ &fnv_test_str[67], (Fnv64_t) 0xb425c54ff807b6a3ULL },
	{ &fnv_test_str[68], (Fnv64_t) 0x23e520e2751bb46eULL },
	{ &fnv_test_str[69], (Fnv64_t) 0x1a0b44ccfe1385ecULL },
	{ &fnv_test_str[70], (Fnv64_t) 0xf5ba4b190cc2119fULL },
	{ &fnv_test_str[71], (Fnv64_t) 0x4c962690ace2baafULL },
	{ &fnv_test_str[72], (Fnv64_t) 0x0716ded5c64cda19ULL },
	{ &fnv_test_str[73], (Fnv64_t) 0x5a292c3ff5d150f0ULL },
	{ &fnv_test_str[74], (Fnv64_t) 0x05b3e0aeb308ecf0ULL },
	{ &fnv_test_str[75], (Fnv64_t) 0xb92a5eda37d119d9ULL },
	{ &fnv_test_str[76], (Fnv64_t) 0x73ce41ccd80f6635ULL },
	{ &fnv_test_str[77], (Fnv64_t) 0xd58c2c132109f00bULL },
	{ &fnv_test_str[78], (Fnv64_t) 0xe78baf81243f47d1ULL },
	{ &fnv_test_str[79], (Fnv64_t) 0xb0968f7096a2ee7cULL },
	{ &fnv_test_str[80], (Fnv64_t) 0xb425a84ff807855cULL },
	{ &fnv_test_str[81], (Fnv64_t) 0x23e4e9e2751b56f9ULL },
	{ &fnv_test_str[82], (Fnv64_t) 0x1a0b4eccfe1396eaULL },
	{ &fnv_test_str[83], (Fnv64_t) 0x54abd453bb2c9004ULL },
	{ &fnv_test_str[84], (Fnv64_t) 0x08ba5f07b55ec3daULL },
	{ &fnv_test_str[85], (Fnv64_t) 0x337354193006cb6eULL },
	{ &fnv_test_str[86], (Fnv64_t) 0xa430d84680aabd0bULL },
	{ &fnv_test_str[87], (Fnv64_t) 0xa9bc8acca21f39b1ULL },
	{ &fnv_test_str[88], (Fnv64_t) 0x6961196491cc682dULL },
	{ &fnv_test_str[89], (Fnv64_t) 0xad2bb1774799dfe9ULL },
	{ &fnv_test_str[90], (Fnv64_t) 0x6961166491cc6314ULL },
	{ &fnv_test_str[91], (Fnv64_t) 0x8d1bb3904a3b1236ULL },
	{ &fnv_test_str[92], (Fnv64_t) 0x6961176491cc64c7ULL },
	{ &fnv_test_str[93], (Fnv64_t) 0xed205d87f40434c7ULL },
	{ &fnv_test_str[94], (Fnv64_t) 0x6961146491cc5faeULL },
	{ &fnv_test_str[95], (Fnv64_t) 0xcd3baf5e44f8ad9cULL },
	{ &fnv_test_str[96], (Fnv64_t) 0xe3b36596127cd6d8ULL },
	{ &fnv_test_str[97], (Fnv64_t) 0xf77f1072c8e8a646ULL },
	{ &fnv_test_str[98], (Fnv64_t) 0xe3b36396127cd372ULL },
	{ &fnv_test_str[99], (Fnv64_t) 0x6067dce9932ad458ULL },
	{ &fnv_test_str[100], (Fnv64_t) 0xe3b37596127cf208ULL },
	{ &fnv_test_str[101], (Fnv64_t) 0x4b7b10fa9fe83936ULL },
	{ &fnv_test_str[102], (Fnv64_t) 0xaabafe7104d914beULL },
	{ &fnv_test_str[103], (Fnv64_t) 0xf4d3180b3cde3edaULL },
	{ &fnv_test_str[104], (Fnv64_t) 0xaabafd7104d9130bULL },
	{ &fnv_test_str[105], (Fnv64_t) 0xf4cfb20b3cdb5bb1ULL },
	{ &fnv_test_str[106], (Fnv64_t) 0xaabafc7104d91158ULL },
	{ &fnv_test_str[107], (Fnv64_t) 0xf4cc4c0b3cd87888ULL },
	{ &fnv_test_str[108], (Fnv64_t) 0xe729bac5d2a8d3a7ULL },
	{ &fnv_test_str[109], (Fnv64_t) 0x74bc0524f4dfa4c5ULL },
	{ &fnv_test_str[110], (Fnv64_t) 0xe72630c5d2a5b352ULL },
	{ &fnv_test_str[111], (Fnv64_t) 0x6b983224ef8fb456ULL },
	{ &fnv_test_str[112], (Fnv64_t) 0xe73042c5d2ae266dULL },
	{ &fnv_test_str[113], (Fnv64_t) 0x8527e324fdeb4b37ULL },
	{ &fnv_test_str[114], (Fnv64_t) 0x0a83c86fee952abcULL },
	{ &fnv_test_str[115], (Fnv64_t) 0x7318523267779d74ULL },
	{ &fnv_test_str[116], (Fnv64_t) 0x3e66d3d56b8caca1ULL },
	{ &fnv_test_str[117], (Fnv64_t) 0x956694a5c0095593ULL },
	{ &fnv_test_str[118], (Fnv64_t) 0xcac54572bb1a6fc8ULL },
	{ &fnv_test_str[119], (Fnv64_t) 0xa7a4c9f3edebf0d8ULL },
	{ &fnv_test_str[120], (Fnv64_t) 0x7829851fac17b143ULL },
	{ &fnv_test_str[121], (Fnv64_t) 0x2c8f4c9af81bcf06ULL },
	{ &fnv_test_str[122], (Fnv64_t) 0xd34e31539740c732ULL },
	{ &fnv_test_str[123], (Fnv64_t) 0x3605a2ac253d2db1ULL },
	{ &fnv_test_str[124], (Fnv64_t) 0x08c11b8346f4a3c3ULL },
	{ &fnv_test_str[125], (Fnv64_t) 0x6be396289ce8a6daULL },
	{ &fnv_test_str[126], (Fnv64_t) 0xd9b957fb7fe794c5ULL },
	{ &fnv_test_str[127], (Fnv64_t) 0x05be33da04560a93ULL },
	{ &fnv_test_str[128], (Fnv64_t) 0x0957f1577ba9747cULL },
	{ &fnv_test_str[129], (Fnv64_t) 0xda2cc3acc24fba57ULL },
	{ &fnv_test_str[130], (Fnv64_t) 0x74136f185b29e7f0ULL },
	{ &fnv_test_str[131], (Fnv64_t) 0xb2f2b4590edb93b2ULL },
	{ &fnv_test_str[132], (Fnv64_t) 0xb3608fce8b86ae04ULL },
	{ &fnv_test_str[133], (Fnv64_t) 0x4a3a865079359063ULL },
	{ &fnv_test_str[134], (Fnv64_t) 0x5b3a7ef496880a50ULL },
	{ &fnv_test_str[135], (Fnv64_t) 0x48fae3163854c23bULL },
	{ &fnv_test_str[136], (Fnv64_t) 0x07aaa640476e0b9aULL },
	{ &fnv_test_str[137], (Fnv64_t) 0x2f653656383a687dULL },
	{ &fnv_test_str[138], (Fnv64_t) 0xa1031f8e7599d79cULL },
	{ &fnv_test_str[139], (Fnv64_t) 0xa31908178ff92477ULL },
	{ &fnv_test_str[140], (Fnv64_t) 0x097edf3c14c3fb83ULL },
	{ &fnv_test_str[141], (Fnv64_t) 0xb51ca83feaa0971bULL },
	{ &fnv_test_str[142], (Fnv64_t) 0xdd3c0d96d784f2e9ULL },
	{ &fnv_test_str[143], (Fnv64_t) 0x86cd26a9ea767d78ULL },
	{ &fnv_test_str[144], (Fnv64_t) 0xe6b215ff54a30c18ULL },
	{ &fnv_test_str[145], (Fnv64_t) 0xec5b06a1c5531093ULL },
	{ &fnv_test_str[146], (Fnv64_t) 0x45665a929f9ec5e5ULL },
	{ &fnv_test_str[147], (Fnv64_t) 0x8c7609b4a9f10907ULL },
	{ &fnv_test_str[148], (Fnv64_t) 0x89aac3a491f0d729ULL },
	{ &fnv_test_str[149], (Fnv64_t) 0x32ce6b26e0f4a403ULL },
	{ &fnv_test_str[150], (Fnv64_t) 0x614ab44e02b53e01ULL },
	{ &fnv_test_str[151], (Fnv64_t) 0xfa6472eb6eef3290ULL },
	{ &fnv_test_str[152], (Fnv64_t) 0x9e5d75eb1948eb6aULL },
	{ &fnv_test_str[153], (Fnv64_t) 0xb6d12ad4a8671852ULL },
	{ &fnv_test_str[154], (Fnv64_t) 0x88826f56eba07af1ULL },
	{ &fnv_test_str[155], (Fnv64_t) 0x44535bf2645bc0fdULL },
	{ &fnv_test_str[156], (Fnv64_t) 0x169388ffc21e3728ULL },
	{ &fnv_test_str[157], (Fnv64_t) 0xf68aac9e396d8224ULL },
	{ &fnv_test_str[158], (Fnv64_t) 0x8e87d7e7472b3883ULL },
	{ &fnv_test_str[159], (Fnv64_t) 0x295c26caa8b423deULL },
	{ &fnv_test_str[160], (Fnv64_t) 0x322c814292e72176ULL },
	{ &fnv_test_str[161], (Fnv64_t) 0x8a06550eb8af7268ULL },
	{ &fnv_test_str[162], (Fnv64_t) 0xef86d60e661bcf71ULL },
	{ &fnv_test_str[163], (Fnv64_t) 0x9e5426c87f30ee54ULL },
	{ &fnv_test_str[164], (Fnv64_t) 0xf1ea8aa826fd047eULL },
	{ &fnv_test_str[165], (Fnv64_t) 0x0babaf9a642cb769ULL },
	{ &fnv_test_str[166], (Fnv64_t) 0x4b3341d4068d012eULL },
	{ &fnv_test_str[167], (Fnv64_t) 0xd15605cbc30a335cULL },
	{ &fnv_test_str[168], (Fnv64_t) 0x5b21060aed8412e5ULL },
	{ &fnv_test_str[169], (Fnv64_t) 0x45e2cda1ce6f4227ULL },
	{ &fnv_test_str[170], (Fnv64_t) 0x50ae3745033ad7d4ULL },
	{ &fnv_test_str[171], (Fnv64_t) 0xaa4588ced46bf414ULL },
	{ &fnv_test_str[172], (Fnv64_t) 0xc1b0056c4a95467eULL },
	{ &fnv_test_str[173], (Fnv64_t) 0x56576a71de8b4089ULL },
	{ &fnv_test_str[174], (Fnv64_t) 0xbf20965fa6dc927eULL },
	{ &fnv_test_str[175], (Fnv64_t) 0x569f8383c2040882ULL },
	{ &fnv_test_str[176], (Fnv64_t) 0xe1e772fba08feca0ULL },
	{ &fnv_test_str[177], (Fnv64_t) 0x4ced94af97138ac4ULL },
	{ &fnv_test_str[178], (Fnv64_t) 0xc4112ffb337a82fbULL },
	{ &fnv_test_str[179], (Fnv64_t) 0xd64a4fd41de38b7dULL },
	{ &fnv_test_str[180], (Fnv64_t) 0x4cfc32329edebcbbULL },
	{ &fnv_test_str[181], (Fnv64_t) 0x0803564445050395ULL },
	{ &fnv_test_str[182], (Fnv64_t) 0xaa1574ecf4642ffdULL },
	{ &fnv_test_str[183], (Fnv64_t) 0x694bc4e54cc315f9ULL },
	{ &fnv_test_str[184], (Fnv64_t) 0xa3d7cb273b011721ULL },
	{ &fnv_test_str[185], (Fnv64_t) 0x577c2f8b6115bfa5ULL },
	{ &fnv_test_str[186], (Fnv64_t) 0xb7ec8c1a769fb4c1ULL },
	{ &fnv_test_str[187], (Fnv64_t) 0x5d5cfce63359ab19ULL },
	{ &fnv_test_str[188], (Fnv64_t) 0x33b96c3cd65b5f71ULL },
	{ &fnv_test_str[189], (Fnv64_t) 0xd845097780602bb9ULL },
	{ &fnv_test_str[190], (Fnv64_t) 0x84d47645d02da3d5ULL },
	{ &fnv_test_str[191], (Fnv64_t) 0x83544f33b58773a5ULL },
	{ &fnv_test_str[192], (Fnv64_t) 0x9175cbb2160836c5ULL },
	{ &fnv_test_str[193], (Fnv64_t) 0xc71b3bc175e72bc5ULL },
	{ &fnv_test_str[194], (Fnv64_t) 0x636806ac222ec985ULL },
	{ &fnv_test_str[195], (Fnv64_t) 0xb6ef0e6950f52ed5ULL },
	{ &fnv_test_str[196], (Fnv64_t) 0xead3d8a0f3dfdaa5ULL },
	{ &fnv_test_str[197], (Fnv64_t) 0x922908fe9a861ba5ULL },
	{ &fnv_test_str[198], (Fnv64_t) 0x6d4821de275fd5c5ULL },
	{ &fnv_test_str[199], (Fnv64_t) 0x1fe3fce62bd816b5ULL },
	{ &fnv_test_str[200], (Fnv64_t) 0xc23e9fccd6f70591ULL },
	{ &fnv_test_str[201], (Fnv64_t) 0xc1af12bdfe16b5b5ULL },
	{ &fnv_test_str[202], (Fnv64_t) 0x39e9f18f2f85e221ULL },
	{ NULL, (Fnv64_t) 0 }
};

/* end of output generated by make vector.c */
/*
 * insert the contents of vector.c above
 */

static const char *program = "test_hash";	/* our name */

/*
 * test_fnv64 - test the FNV64 hash
 *
 * given:
 *	hash_type	type of FNV hash to test
 *	init_hval	initial hash value
 *	mask	  	lower bit mask
 *	v_flag	  	1 => print test failure info on stderr
 *	code	  0 ==> generate FNV test vectors
 *		  1 ==> validate against FNV test vectors
 *
 * returns:	0 ==> OK, else test vector failure number
 */
int
test_fnv64(Fnv64_t init_hval, Fnv64_t mask, int v_flag, int code)
{
	struct test_vector *t;	/* FNV test vector */
	Fnv64_t hval;		/* current hash value */
	int tstnum;		/* test vector that failed, starting at 1 */

	if (code != 1) {
		fprintf(stderr, "Unsupported code.\n");
		return (EXIT_FAILURE);
	}

	/*
	 * loop through all test vectors
	 */
	for (t = fnv_test_str, tstnum = 1; t->buf != NULL; ++t, ++tstnum) {

		/*
		 * compute the FNV hash
		 */
		hval = init_hval;
		hval = __wt_hash_fnv(t->buf, t->len);

		if ((hval&mask) != (fnv1a_64_vector[tstnum-1].fnv1a_64 &mask)) {
			if (v_flag) {
				fprintf(stderr,
				    "%s: failed fnv1a_64 test # %d\n",
				    program, tstnum);
				fprintf(stderr,
				    "%s: test # 1 is 1st test\n",
				    program);
				fprintf(stderr,
			    "%s: expected 0x%016llx != generated: 0x%016llx\n",
				    program, (hval&mask),
			    (fnv1a_64_vector[tstnum-1].fnv1a_64 & mask));
			}
			return (tstnum);
		}
	}

	/*
	 * no failures, return code 0 ==> all OK
	 */
	return (0);
}
