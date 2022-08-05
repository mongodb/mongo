/*************************************************
*      Perl-Compatible Regular Expressions       *
*************************************************/

/* PCRE is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language.

                       Written by Philip Hazel
     Original API code Copyright (c) 1997-2012 University of Cambridge
         New API code Copyright (c) 2016 University of Cambridge

-----------------------------------------------------------------------------
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the University of Cambridge nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------------
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 0
#include "pcre2.h"

/*
 Letter characters:
   \xe6\x92\xad = 0x64ad = 25773 (kanji)
 Non-letter characters:
   \xc2\xa1 = 0xa1 =  (Inverted Exclamation Mark)
   \xf3\xa9\xb7\x80 = 0xe9dc0 = 957888
   \xed\xa0\x80 = 55296 = 0xd800 (Invalid UTF character)
   \xed\xb0\x80 = 56320 = 0xdc00 (Invalid UTF character)
 Newlines:
   \xc2\x85 = 0x85 = 133 (NExt Line = NEL)
   \xe2\x80\xa8 = 0x2028 = 8232 (Line Separator)
 Othercase pairs:
   \xc3\xa9 = 0xe9 = 233 (e')
      \xc3\x89 = 0xc9 = 201 (E')
   \xc3\xa1 = 0xe1 = 225 (a')
      \xc3\x81 = 0xc1 = 193 (A')
   \x53 = 0x53 = S
     \x73 = 0x73 = s
     \xc5\xbf = 0x17f = 383 (long S)
   \xc8\xba = 0x23a = 570
      \xe2\xb1\xa5 = 0x2c65 = 11365
   \xe1\xbd\xb8 = 0x1f78 = 8056
      \xe1\xbf\xb8 = 0x1ff8 = 8184
   \xf0\x90\x90\x80 = 0x10400 = 66560
      \xf0\x90\x90\xa8 = 0x10428 = 66600
   \xc7\x84 = 0x1c4 = 452
     \xc7\x85 = 0x1c5 = 453
     \xc7\x86 = 0x1c6 = 454
 Caseless sets:
   ucp_Armenian - \x{531}-\x{556} -> \x{561}-\x{586}
   ucp_Coptic - \x{2c80}-\x{2ce3} -> caseless: XOR 0x1
   ucp_Latin - \x{ff21}-\x{ff3a} -> \x{ff41]-\x{ff5a}

 Mark property:
   \xcc\x8d = 0x30d = 781
 Special:
   \xc2\x80 = 0x80 = 128 (lowest 2 byte character)
   \xdf\xbf = 0x7ff = 2047 (highest 2 byte character)
   \xe0\xa0\x80 = 0x800 = 2048 (lowest 2 byte character)
   \xef\xbf\xbf = 0xffff = 65535 (highest 3 byte character)
   \xf0\x90\x80\x80 = 0x10000 = 65536 (lowest 4 byte character)
   \xf4\x8f\xbf\xbf = 0x10ffff = 1114111 (highest allowed utf character)
*/

static int regression_tests(void);
static int invalid_utf8_regression_tests(void);
static int invalid_utf16_regression_tests(void);
static int invalid_utf32_regression_tests(void);

int main(void)
{
	int jit = 0;
#if defined SUPPORT_PCRE2_8
	pcre2_config_8(PCRE2_CONFIG_JIT, &jit);
#elif defined SUPPORT_PCRE2_16
	pcre2_config_16(PCRE2_CONFIG_JIT, &jit);
#elif defined SUPPORT_PCRE2_32
	pcre2_config_32(PCRE2_CONFIG_JIT, &jit);
#endif
	if (!jit) {
		printf("JIT must be enabled to run pcre2_jit_test\n");
		return 1;
	}
	return regression_tests()
		| invalid_utf8_regression_tests()
		| invalid_utf16_regression_tests()
		| invalid_utf32_regression_tests();
}

/* --------------------------------------------------------------------------------------- */

#if !(defined SUPPORT_PCRE2_8) && !(defined SUPPORT_PCRE2_16) && !(defined SUPPORT_PCRE2_32)
#error SUPPORT_PCRE2_8 or SUPPORT_PCRE2_16 or SUPPORT_PCRE2_32 must be defined
#endif

#define MU	(PCRE2_MULTILINE | PCRE2_UTF)
#define MUP	(PCRE2_MULTILINE | PCRE2_UTF | PCRE2_UCP)
#define CMU	(PCRE2_CASELESS | PCRE2_MULTILINE | PCRE2_UTF)
#define CMUP	(PCRE2_CASELESS | PCRE2_MULTILINE | PCRE2_UTF | PCRE2_UCP)
#define M	(PCRE2_MULTILINE)
#define MP	(PCRE2_MULTILINE | PCRE2_UCP)
#define U	(PCRE2_UTF)
#define CM	(PCRE2_CASELESS | PCRE2_MULTILINE)

#define BSR(x)	((x) << 16)
#define A	PCRE2_NEWLINE_ANYCRLF

#define GET_NEWLINE(x)	((x) & 0xffff)
#define GET_BSR(x)	((x) >> 16)

#define OFFSET_MASK	0x00ffff
#define F_NO8		0x010000
#define F_NO16		0x020000
#define F_NO32		0x020000
#define F_NOMATCH	0x040000
#define F_DIFF		0x080000
#define F_FORCECONV	0x100000
#define F_PROPERTY	0x200000

struct regression_test_case {
	int compile_options;
	int newline;
	int match_options;
	int start_offset;
	const char *pattern;
	const char *input;
};

static struct regression_test_case regression_test_cases[] = {
	/* Constant strings. */
	{ MU, A, 0, 0, "AbC", "AbAbC" },
	{ MU, A, 0, 0, "ACCEPT", "AACACCACCEACCEPACCEPTACCEPTT" },
	{ CMU, A, 0, 0, "aA#\xc3\xa9\xc3\x81", "aA#Aa#\xc3\x89\xc3\xa1" },
	{ M, A, 0, 0, "[^a]", "aAbB" },
	{ CM, A, 0, 0, "[^m]", "mMnN" },
	{ M, A, 0, 0, "a[^b][^#]", "abacd" },
	{ CM, A, 0, 0, "A[^B][^E]", "abacd" },
	{ CMU, A, 0, 0, "[^x][^#]", "XxBll" },
	{ MU, A, 0, 0, "[^a]", "aaa\xc3\xa1#Ab" },
	{ CMU, A, 0, 0, "[^A]", "aA\xe6\x92\xad" },
	{ MU, A, 0, 0, "\\W(\\W)?\\w", "\r\n+bc" },
	{ MU, A, 0, 0, "\\W(\\W)?\\w", "\n\r+bc" },
	{ MU, A, 0, 0, "\\W(\\W)?\\w", "\r\r+bc" },
	{ MU, A, 0, 0, "\\W(\\W)?\\w", "\n\n+bc" },
	{ MU, A, 0, 0, "[axd]", "sAXd" },
	{ CMU, A, 0, 0, "[axd]", "sAXd" },
	{ CMU, A, 0, 0 | F_NOMATCH, "[^axd]", "DxA" },
	{ MU, A, 0, 0, "[a-dA-C]", "\xe6\x92\xad\xc3\xa9.B" },
	{ MU, A, 0, 0, "[^a-dA-C]", "\xe6\x92\xad\xc3\xa9" },
	{ CMU, A, 0, 0, "[^\xc3\xa9]", "\xc3\xa9\xc3\x89." },
	{ MU, A, 0, 0, "[^\xc3\xa9]", "\xc3\xa9\xc3\x89." },
	{ MU, A, 0, 0, "[^a]", "\xc2\x80[]" },
	{ CMU, A, 0, 0, "\xf0\x90\x90\xa7", "\xf0\x90\x91\x8f" },
	{ CM, A, 0, 0, "1a2b3c4", "1a2B3c51A2B3C4" },
	{ PCRE2_CASELESS, 0, 0, 0, "\xff#a", "\xff#\xff\xfe##\xff#A" },
	{ PCRE2_CASELESS, 0, 0, 0, "\xfe", "\xff\xfc#\xfe\xfe" },
	{ PCRE2_CASELESS, 0, 0, 0, "a1", "Aa1" },
#ifndef NEVER_BACKSLASH_C
	{ M, A, 0, 0, "\\Ca", "cda" },
	{ CM, A, 0, 0, "\\Ca", "CDA" },
	{ M, A, 0, 0 | F_NOMATCH, "\\Cx", "cda" },
	{ CM, A, 0, 0 | F_NOMATCH, "\\Cx", "CDA" },
#endif /* !NEVER_BACKSLASH_C */
	{ CMUP, A, 0, 0, "\xf0\x90\x90\x80\xf0\x90\x90\xa8", "\xf0\x90\x90\xa8\xf0\x90\x90\x80" },
	{ CMUP, A, 0, 0, "\xf0\x90\x90\x80{2}", "\xf0\x90\x90\x80#\xf0\x90\x90\xa8\xf0\x90\x90\x80" },
	{ CMUP, A, 0, 0, "\xf0\x90\x90\xa8{2}", "\xf0\x90\x90\x80#\xf0\x90\x90\xa8\xf0\x90\x90\x80" },
	{ CMUP, A, 0, 0, "\xe1\xbd\xb8\xe1\xbf\xb8", "\xe1\xbf\xb8\xe1\xbd\xb8" },
	{ M, A, 0, 0, "[3-57-9]", "5" },
	{ PCRE2_AUTO_CALLOUT, A, 0, 0, "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
		"12345678901234567890123456789012345678901234567890123456789012345678901234567890" },

	/* Assertions. */
	{ MU, A, 0, 0, "\\b[^A]", "A_B#" },
	{ M, A, 0, 0 | F_NOMATCH, "\\b\\W", "\n*" },
	{ MU, A, 0, 0, "\\B[^,]\\b[^s]\\b", "#X" },
	{ MP, A, 0, 0, "\\B", "_\xa1" },
	{ MP, A, 0, 0 | F_PROPERTY, "\\b_\\b[,A]\\B", "_," },
	{ MUP, A, 0, 0, "\\b", "\xe6\x92\xad!" },
	{ MUP, A, 0, 0, "\\B", "_\xc2\xa1\xc3\xa1\xc2\x85" },
	{ MUP, A, 0, 0, "\\b[^A]\\B[^c]\\b[^_]\\B", "_\xc3\xa1\xe2\x80\xa8" },
	{ MUP, A, 0, 0, "\\b\\w+\\B", "\xc3\x89\xc2\xa1\xe6\x92\xad\xc3\x81\xc3\xa1" },
	{ MU, A, 0, 0 | F_NOMATCH, "\\b.", "\xcd\xbe" },
	{ CMUP, A, 0, 0, "\\By", "\xf0\x90\x90\xa8y" },
	{ M, A, 0, 0 | F_NOMATCH, "\\R^", "\n" },
	{ M, A, 0, 1 | F_NOMATCH, "^", "\n" },
	{ 0, 0, 0, 0, "^ab", "ab" },
	{ 0, 0, 0, 0 | F_NOMATCH, "^ab", "aab" },
	{ M, PCRE2_NEWLINE_CRLF, 0, 0, "^a", "\r\raa\n\naa\r\naa" },
	{ MU, A, 0, 0, "^-", "\xe2\x80\xa8--\xc2\x85-\r\n-" },
	{ M, PCRE2_NEWLINE_ANY, 0, 0, "^-", "a--b--\x85--" },
	{ MU, PCRE2_NEWLINE_ANY, 0, 0, "^-", "a--\xe2\x80\xa8--" },
	{ MU, PCRE2_NEWLINE_ANY, 0, 0, "^-", "a--\xc2\x85--" },
	{ 0, 0, 0, 0, "ab$", "ab" },
	{ 0, 0, 0, 0 | F_NOMATCH, "ab$", "abab\n\n" },
	{ PCRE2_DOLLAR_ENDONLY, 0, 0, 0 | F_NOMATCH, "ab$", "abab\r\n" },
	{ M, PCRE2_NEWLINE_CRLF, 0, 0, "a$", "\r\raa\n\naa\r\naa" },
	{ M, PCRE2_NEWLINE_ANY, 0, 0, "a$", "aaa" },
	{ MU, PCRE2_NEWLINE_ANYCRLF, 0, 0, "#$", "#\xc2\x85###\r#" },
	{ MU, PCRE2_NEWLINE_ANY, 0, 0, "#$", "#\xe2\x80\xa9" },
	{ 0, PCRE2_NEWLINE_ANY, PCRE2_NOTBOL, 0 | F_NOMATCH, "^a", "aa\naa" },
	{ M, PCRE2_NEWLINE_ANY, PCRE2_NOTBOL, 0, "^a", "aa\naa" },
	{ 0, PCRE2_NEWLINE_ANY, PCRE2_NOTEOL, 0 | F_NOMATCH, "a$", "aa\naa" },
	{ 0, PCRE2_NEWLINE_ANY, PCRE2_NOTEOL, 0 | F_NOMATCH, "a$", "aa\r\n" },
	{ U | PCRE2_DOLLAR_ENDONLY, PCRE2_NEWLINE_ANY, 0, 0 | F_PROPERTY, "\\p{Any}{2,}$", "aa\r\n" },
	{ M, PCRE2_NEWLINE_ANY, PCRE2_NOTEOL, 0, "a$", "aa\naa" },
	{ 0, PCRE2_NEWLINE_CR, 0, 0, ".\\Z", "aaa" },
	{ U, PCRE2_NEWLINE_CR, 0, 0, "a\\Z", "aaa\r" },
	{ 0, PCRE2_NEWLINE_CR, 0, 0, ".\\Z", "aaa\n" },
	{ 0, PCRE2_NEWLINE_CRLF, 0, 0, ".\\Z", "aaa\r" },
	{ U, PCRE2_NEWLINE_CRLF, 0, 0, ".\\Z", "aaa\n" },
	{ 0, PCRE2_NEWLINE_CRLF, 0, 0, ".\\Z", "aaa\r\n" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\r" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\n" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\r\n" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\xe2\x80\xa8" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\r" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\n" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".\\Z", "aaa\r\n" },
	{ U, PCRE2_NEWLINE_ANY, 0, 0, ".\\Z", "aaa\xc2\x85" },
	{ U, PCRE2_NEWLINE_ANY, 0, 0, ".\\Z", "aaa\xe2\x80\xa8" },
	{ M, A, 0, 0, "\\Aa", "aaa" },
	{ M, A, 0, 1 | F_NOMATCH, "\\Aa", "aaa" },
	{ M, A, 0, 1, "\\Ga", "aaa" },
	{ M, A, 0, 1 | F_NOMATCH, "\\Ga", "aba" },
	{ M, A, 0, 0, "a\\z", "aaa" },
	{ M, A, 0, 0 | F_NOMATCH, "a\\z", "aab" },

	/* Brackets and alternatives. */
	{ MU, A, 0, 0, "(ab|bb|cd)", "bacde" },
	{ MU, A, 0, 0, "(?:ab|a)(bc|c)", "ababc" },
	{ MU, A, 0, 0, "((ab|(cc))|(bb)|(?:cd|efg))", "abac" },
	{ CMU, A, 0, 0, "((aB|(Cc))|(bB)|(?:cd|EFg))", "AcCe" },
	{ MU, A, 0, 0, "((ab|(cc))|(bb)|(?:cd|ebg))", "acebebg" },
	{ MU, A, 0, 0, "(?:(a)|(?:b))(cc|(?:d|e))(a|b)k", "accabdbbccbk" },
	{ MU, A, 0, 0, "\xc7\x82|\xc6\x82", "\xf1\x83\x82\x82\xc7\x82\xc7\x83" },
	{ MU, A, 0, 0, "=\xc7\x82|#\xc6\x82", "\xf1\x83\x82\x82=\xc7\x82\xc7\x83" },
	{ MU, A, 0, 0, "\xc7\x82\xc7\x83|\xc6\x82\xc6\x82", "\xf1\x83\x82\x82\xc7\x82\xc7\x83" },
	{ MU, A, 0, 0, "\xc6\x82\xc6\x82|\xc7\x83\xc7\x83|\xc8\x84\xc8\x84", "\xf1\x83\x82\x82\xc8\x84\xc8\x84" },
	{ U, A, 0, 0, "\xe1\x81\x80|\xe2\x82\x80|\xe4\x84\x80", "\xdf\xbf\xc2\x80\xe4\x84\x80" },
	{ U, A, 0, 0, "(?:\xe1\x81\x80|\xe2\x82\x80|\xe4\x84\x80)#", "\xdf\xbf\xc2\x80#\xe4\x84\x80#" },
	{ CM, A, 0, 0, "ab|cd", "CD" },
	{ CM, A, 0, 0, "a1277|a1377|bX487", "bx487" },
	{ CM, A, 0, 0, "a1277|a1377|bx487", "bX487" },

	/* Greedy and non-greedy ? operators. */
	{ MU, A, 0, 0, "(?:a)?a", "laab" },
	{ CMU, A, 0, 0, "(A)?A", "llaab" },
	{ MU, A, 0, 0, "(a)?\?a", "aab" }, /* ?? is the prefix of trygraphs in GCC. */
	{ MU, A, 0, 0, "(a)?a", "manm" },
	{ CMU, A, 0, 0, "(a|b)?\?d((?:e)?)", "ABABdx" },
	{ MU, A, 0, 0, "(a|b)?\?d((?:e)?)", "abcde" },
	{ MU, A, 0, 0, "((?:ab)?\?g|b(?:g(nn|d)?\?)?)?\?(?:n)?m", "abgnbgnnbgdnmm" },

	/* Greedy and non-greedy + operators */
	{ MU, A, 0, 0, "(aa)+aa", "aaaaaaa" },
	{ MU, A, 0, 0, "(aa)+?aa", "aaaaaaa" },
	{ MU, A, 0, 0, "(?:aba|ab|a)+l", "ababamababal" },
	{ MU, A, 0, 0, "(?:aba|ab|a)+?l", "ababamababal" },
	{ MU, A, 0, 0, "(a(?:bc|cb|b|c)+?|ss)+e", "accssabccbcacbccbbXaccssabccbcacbccbbe" },
	{ MU, A, 0, 0, "(a(?:bc|cb|b|c)+|ss)+?e", "accssabccbcacbccbbXaccssabccbcacbccbbe" },
	{ MU, A, 0, 0, "(?:(b(c)+?)+)?\?(?:(bc)+|(cb)+)+(?:m)+", "bccbcccbcbccbcbPbccbcccbcbccbcbmmn" },
	{ MU, A, 0, 0, "(aa|bb){8,1000}", "abaabbaabbaabbaab_aabbaabbaabbaabbaabbaabb_" },

	/* Greedy and non-greedy * operators */
	{ CMU, A, 0, 0, "(?:AA)*AB", "aaaaaaamaaaaaaab" },
	{ MU, A, 0, 0, "(?:aa)*?ab", "aaaaaaamaaaaaaab" },
	{ MU, A, 0, 0, "(aa|ab)*ab", "aaabaaab" },
	{ CMU, A, 0, 0, "(aa|Ab)*?aB", "aaabaaab" },
	{ MU, A, 0, 0, "(a|b)*(?:a)*(?:b)*m", "abbbaaababanabbbaaababamm" },
	{ MU, A, 0, 0, "(a|b)*?(?:a)*?(?:b)*?m", "abbbaaababanabbbaaababamm" },
	{ M, A, 0, 0, "a(a(\\1*)a|(b)b+){0}a", "aa" },
	{ M, A, 0, 0, "((?:a|)*){0}a", "a" },

	/* Combining ? + * operators */
	{ MU, A, 0, 0, "((bm)+)?\?(?:a)*(bm)+n|((am)+?)?(?:a)+(am)*n", "bmbmabmamaaamambmaman" },
	{ MU, A, 0, 0, "(((ab)?cd)*ef)+g", "abcdcdefcdefefmabcdcdefcdefefgg" },
	{ MU, A, 0, 0, "(((ab)?\?cd)*?ef)+?g", "abcdcdefcdefefmabcdcdefcdefefgg" },
	{ MU, A, 0, 0, "(?:(ab)?c|(?:ab)+?d)*g", "ababcdccababddg" },
	{ MU, A, 0, 0, "(?:(?:ab)?\?c|(ab)+d)*?g", "ababcdccababddg" },

	/* Single character iterators. */
	{ MU, A, 0, 0, "(a+aab)+aaaab", "aaaabcaaaabaabcaabcaaabaaaab" },
	{ MU, A, 0, 0, "(a*a*aab)+x", "aaaaabaabaaabmaabx" },
	{ MU, A, 0, 0, "(a*?(b|ab)a*?)+x", "aaaabcxbbaabaacbaaabaabax" },
	{ MU, A, 0, 0, "(a+(ab|ad)a+)+x", "aaabaaaadaabaaabaaaadaaax" },
	{ MU, A, 0, 0, "(a?(a)a?)+(aaa)", "abaaabaaaaaaaa" },
	{ MU, A, 0, 0, "(a?\?(a)a?\?)+(b)", "aaaacaaacaacacbaaab" },
	{ MU, A, 0, 0, "(a{0,4}(b))+d", "aaaaaabaabcaaaaabaaaaabd" },
	{ MU, A, 0, 0, "(a{0,4}?[^b])+d+(a{0,4}[^b])d+", "aaaaadaaaacaadddaaddd" },
	{ MU, A, 0, 0, "(ba{2})+c", "baabaaabacbaabaac" },
	{ MU, A, 0, 0, "(a*+bc++)+", "aaabbcaaabcccab" },
	{ MU, A, 0, 0, "(a?+[^b])+", "babaacacb" },
	{ MU, A, 0, 0, "(a{0,3}+b)(a{0,3}+b)(a{0,3}+)[^c]", "abaabaaacbaabaaaac" },
	{ CMU, A, 0, 0, "([a-c]+[d-f]+?)+?g", "aBdacdehAbDaFgA" },
	{ CMU, A, 0, 0, "[c-f]+k", "DemmFke" },
	{ MU, A, 0, 0, "([DGH]{0,4}M)+", "GGDGHDGMMHMDHHGHM" },
	{ MU, A, 0, 0, "([a-c]{4,}s)+", "abasabbasbbaabsbba" },
	{ CMU, A, 0, 0, "[ace]{3,7}", "AcbDAcEEcEd" },
	{ CMU, A, 0, 0, "[ace]{3,7}?", "AcbDAcEEcEd" },
	{ CMU, A, 0, 0, "[ace]{3,}", "AcbDAcEEcEd" },
	{ CMU, A, 0, 0, "[ace]{3,}?", "AcbDAcEEcEd" },
	{ MU, A, 0, 0, "[ckl]{2,}?g", "cdkkmlglglkcg" },
	{ CMU, A, 0, 0, "[ace]{5}?", "AcCebDAcEEcEd" },
	{ MU, A, 0, 0, "([AbC]{3,5}?d)+", "BACaAbbAEAACCbdCCbdCCAAbb" },
	{ MU, A, 0, 0, "([^ab]{0,}s){2}", "abaabcdsABamsDDs" },
	{ MU, A, 0, 0, "\\b\\w+\\B", "x,a_cd" },
	{ MUP, A, 0, 0, "\\b[^\xc2\xa1]+\\B", "\xc3\x89\xc2\xa1\xe6\x92\xad\xc3\x81\xc3\xa1" },
	{ CMU, A, 0, 0, "[^b]+(a*)([^c]?d{3})", "aaaaddd" },
	{ CMUP, A, 0, 0, "\xe1\xbd\xb8{2}", "\xe1\xbf\xb8#\xe1\xbf\xb8\xe1\xbd\xb8" },
	{ CMU, A, 0, 0, "[^\xf0\x90\x90\x80]{2,4}@", "\xf0\x90\x90\xa8\xf0\x90\x90\x80###\xf0\x90\x90\x80@@@" },
	{ CMU, A, 0, 0, "[^\xe1\xbd\xb8][^\xc3\xa9]", "\xe1\xbd\xb8\xe1\xbf\xb8\xc3\xa9\xc3\x89#" },
	{ MU, A, 0, 0, "[^\xe1\xbd\xb8][^\xc3\xa9]", "\xe1\xbd\xb8\xe1\xbf\xb8\xc3\xa9\xc3\x89#" },
	{ MU, A, 0, 0, "[^\xe1\xbd\xb8]{3,}?", "##\xe1\xbd\xb8#\xe1\xbd\xb8#\xc3\x89#\xe1\xbd\xb8" },
	{ MU, A, 0, 0, "\\d+123", "987654321,01234" },
	{ MU, A, 0, 0, "abcd*|\\w+xy", "aaaaa,abxyz" },
	{ MU, A, 0, 0, "(?:abc|((?:amc|\\b\\w*xy)))", "aaaaa,abxyz" },
	{ MU, A, 0, 0, "a(?R)|([a-z]++)#", ".abcd.abcd#."},
	{ MU, A, 0, 0, "a(?R)|([a-z]++)#", ".abcd.mbcd#."},
	{ MU, A, 0, 0, ".[ab]*.", "xx" },
	{ MU, A, 0, 0, ".[ab]*a", "xxa" },
	{ MU, A, 0, 0, ".[ab]?.", "xx" },
	{ MU, A, 0, 0, "_[ab]+_*a", "_aa" },
	{ MU, A, 0, 0, "#(A+)#\\d+", "#A#A#0" },
	{ MU, A, 0, 0, "(?P<size>\\d+)m|M", "4M" },

	/* Bracket repeats with limit. */
	{ MU, A, 0, 0, "(?:(ab){2}){5}M", "abababababababababababM" },
	{ MU, A, 0, 0, "(?:ab|abab){1,5}M", "abababababababababababM" },
	{ MU, A, 0, 0, "(?>ab|abab){1,5}M", "abababababababababababM" },
	{ MU, A, 0, 0, "(?:ab|abab){1,5}?M", "abababababababababababM" },
	{ MU, A, 0, 0, "(?>ab|abab){1,5}?M", "abababababababababababM" },
	{ MU, A, 0, 0, "(?:(ab){1,4}?){1,3}?M", "abababababababababababababM" },
	{ MU, A, 0, 0, "(?:(ab){1,4}){1,3}abababababababababababM", "ababababababababababababM" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?:(ab){1,4}){1,3}abababababababababababM", "abababababababababababM" },
	{ MU, A, 0, 0, "(ab){4,6}?M", "abababababababM" },

	/* Basic character sets. */
	{ MU, A, 0, 0, "(?:\\s)+(?:\\S)+", "ab \t\xc3\xa9\xe6\x92\xad " },
	{ MU, A, 0, 0, "(\\w)*(k)(\\W)?\?", "abcdef abck11" },
	{ MU, A, 0, 0, "\\((\\d)+\\)\\D", "a() (83 (8)2 (9)ab" },
	{ MU, A, 0, 0, "\\w(\\s|(?:\\d)*,)+\\w\\wb", "a 5, 4,, bb 5, 4,, aab" },
	{ MU, A, 0, 0, "(\\v+)(\\V+)", "\x0e\xc2\x85\xe2\x80\xa8\x0b\x09\xe2\x80\xa9" },
	{ MU, A, 0, 0, "(\\h+)(\\H+)", "\xe2\x80\xa8\xe2\x80\x80\x20\xe2\x80\x8a\xe2\x81\x9f\xe3\x80\x80\x09\x20\xc2\xa0\x0a" },
	{ MU, A, 0, 0, "x[bcef]+", "xaxdxecbfg" },
	{ MU, A, 0, 0, "x[bcdghij]+", "xaxexfxdgbjk" },
	{ MU, A, 0, 0, "x[^befg]+", "xbxexacdhg" },
	{ MU, A, 0, 0, "x[^bcdl]+", "xlxbxaekmd" },
	{ MU, A, 0, 0, "x[^bcdghi]+", "xbxdxgxaefji" },
	{ MU, A, 0, 0, "x[B-Fb-f]+", "xaxAxgxbfBFG" },
	{ CMU, A, 0, 0, "\\x{e9}+", "#\xf0\x90\x90\xa8\xc3\xa8\xc3\xa9\xc3\x89\xc3\x88" },
	{ CMU, A, 0, 0, "[^\\x{e9}]+", "\xc3\xa9#\xf0\x90\x90\xa8\xc3\xa8\xc3\x88\xc3\x89" },
	{ MU, A, 0, 0, "[\\x02\\x7e]+", "\xc3\x81\xe1\xbf\xb8\xf0\x90\x90\xa8\x01\x02\x7e\x7f" },
	{ MU, A, 0, 0, "[^\\x02\\x7e]+", "\x02\xc3\x81\xe1\xbf\xb8\xf0\x90\x90\xa8\x01\x7f\x7e" },
	{ MU, A, 0, 0, "[\\x{81}-\\x{7fe}]+", "#\xe1\xbf\xb8\xf0\x90\x90\xa8\xc2\x80\xc2\x81\xdf\xbe\xdf\xbf" },
	{ MU, A, 0, 0, "[^\\x{81}-\\x{7fe}]+", "\xc2\x81#\xe1\xbf\xb8\xf0\x90\x90\xa8\xc2\x80\xdf\xbf\xdf\xbe" },
	{ MU, A, 0, 0, "[\\x{801}-\\x{fffe}]+", "#\xc3\xa9\xf0\x90\x90\x80\xe0\xa0\x80\xe0\xa0\x81\xef\xbf\xbe\xef\xbf\xbf" },
	{ MU, A, 0, 0, "[^\\x{801}-\\x{fffe}]+", "\xe0\xa0\x81#\xc3\xa9\xf0\x90\x90\x80\xe0\xa0\x80\xef\xbf\xbf\xef\xbf\xbe" },
	{ MU, A, 0, 0, "[\\x{10001}-\\x{10fffe}]+", "#\xc3\xa9\xe2\xb1\xa5\xf0\x90\x80\x80\xf0\x90\x80\x81\xf4\x8f\xbf\xbe\xf4\x8f\xbf\xbf" },
	{ MU, A, 0, 0, "[^\\x{10001}-\\x{10fffe}]+", "\xf0\x90\x80\x81#\xc3\xa9\xe2\xb1\xa5\xf0\x90\x80\x80\xf4\x8f\xbf\xbf\xf4\x8f\xbf\xbe" },
	{ CMU, A, 0, 0 | F_NOMATCH, "^[\\x{0100}-\\x{017f}]", " " },

	/* Unicode properties. */
	{ MUP, A, 0, 0, "[1-5\xc3\xa9\\w]", "\xc3\xa1_" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[\xc3\x81\\p{Ll}]", "A_\xc3\x89\xc3\xa1" },
	{ MUP, A, 0, 0, "[\\Wd-h_x-z]+", "a\xc2\xa1#_yhzdxi" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[\\P{Any}]", "abc" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[^\\p{Any}]", "abc" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[\\P{Any}\xc3\xa1-\xc3\xa8]", "abc" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[^\\p{Any}\xc3\xa1-\xc3\xa8]", "abc" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[\xc3\xa1-\xc3\xa8\\P{Any}]", "abc" },
	{ MUP, A, 0, 0 | F_NOMATCH | F_PROPERTY, "[^\xc3\xa1-\xc3\xa8\\p{Any}]", "abc" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[\xc3\xa1-\xc3\xa8\\p{Any}]", "abc" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[^\xc3\xa1-\xc3\xa8\\P{Any}]", "abc" },
	{ MUP, A, 0, 0, "[b-\xc3\xa9\\s]", "a\xc\xe6\x92\xad" },
	{ CMUP, A, 0, 0, "[\xc2\x85-\xc2\x89\xc3\x89]", "\xc2\x84\xc3\xa9" },
	{ MUP, A, 0, 0, "[^b-d^&\\s]{3,}", "db^ !a\xe2\x80\xa8_ae" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[^\\S\\P{Any}][\\sN]{1,3}[\\P{N}]{4}", "\xe2\x80\xaa\xa N\x9\xc3\xa9_0" },
	{ MU, A, 0, 0 | F_PROPERTY, "[^\\P{L}\x9!D-F\xa]{2,3}", "\x9,.DF\xa.CG\xc3\x81" },
	{ CMUP, A, 0, 0, "[\xc3\xa1-\xc3\xa9_\xe2\x80\xa0-\xe2\x80\xaf]{1,5}[^\xe2\x80\xa0-\xe2\x80\xaf]", "\xc2\xa1\xc3\x89\xc3\x89\xe2\x80\xaf_\xe2\x80\xa0" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[\xc3\xa2-\xc3\xa6\xc3\x81-\xc3\x84\xe2\x80\xa8-\xe2\x80\xa9\xe6\x92\xad\\p{Zs}]{2,}", "\xe2\x80\xa7\xe2\x80\xa9\xe6\x92\xad \xe6\x92\xae" },
	{ MUP, A, 0, 0 | F_PROPERTY, "[\\P{L&}]{2}[^\xc2\x85-\xc2\x89\\p{Ll}\\p{Lu}]{2}", "\xc3\xa9\xe6\x92\xad.a\xe6\x92\xad|\xc2\x8a#" },
	{ PCRE2_UCP, 0, 0, 0 | F_PROPERTY, "[a-b\\s]{2,5}[^a]", "AB  baaa" },
	{ MUP, 0, 0, 0 | F_NOMATCH, "[^\\p{Hangul}\\p{Z}]", " " },
	{ MUP, 0, 0, 0, "[\\p{Lu}\\P{Latin}]+", "c\xEA\xA4\xAE,A,b" },
	{ MUP, 0, 0, 0, "[\\x{a92e}\\p{Lu}\\P{Latin}]+", "c\xEA\xA4\xAE,A,b" },
	{ CMUP, 0, 0, 0, "[^S]\\B", "\xe2\x80\x8a" },

	/* Possible empty brackets. */
	{ MU, A, 0, 0, "(?:|ab||bc|a)+d", "abcxabcabd" },
	{ MU, A, 0, 0, "(|ab||bc|a)+d", "abcxabcabd" },
	{ MU, A, 0, 0, "(?:|ab||bc|a)*d", "abcxabcabd" },
	{ MU, A, 0, 0, "(|ab||bc|a)*d", "abcxabcabd" },
	{ MU, A, 0, 0, "(?:|ab||bc|a)+?d", "abcxabcabd" },
	{ MU, A, 0, 0, "(|ab||bc|a)+?d", "abcxabcabd" },
	{ MU, A, 0, 0, "(?:|ab||bc|a)*?d", "abcxabcabd" },
	{ MU, A, 0, 0, "(|ab||bc|a)*?d", "abcxabcabd" },
	{ MU, A, 0, 0, "(((a)*?|(?:ba)+)+?|(?:|c|ca)*)*m", "abaacaccabacabalabaacaccabacabamm" },
	{ MU, A, 0, 0, "(?:((?:a)*|(ba)+?)+|(|c|ca)*?)*?m", "abaacaccabacabalabaacaccabacabamm" },

	/* Start offset. */
	{ MU, A, 0, 3, "(\\d|(?:\\w)*\\w)+", "0ac01Hb" },
	{ MU, A, 0, 4 | F_NOMATCH, "(\\w\\W\\w)+", "ab#d" },
	{ MU, A, 0, 2 | F_NOMATCH, "(\\w\\W\\w)+", "ab#d" },
	{ MU, A, 0, 1, "(\\w\\W\\w)+", "ab#d" },

	/* Newline. */
	{ M, PCRE2_NEWLINE_CRLF, 0, 0, "\\W{0,2}[^#]{3}", "\r\n#....." },
	{ M, PCRE2_NEWLINE_CR, 0, 0, "\\W{0,2}[^#]{3}", "\r\n#....." },
	{ M, PCRE2_NEWLINE_CRLF, 0, 0, "\\W{1,3}[^#]", "\r\n##...." },
	{ MU, A, PCRE2_NO_UTF_CHECK, 1, "^.a", "\n\x80\nxa" },
	{ MU, A, 0, 1, "^", "\r\n" },
	{ M, PCRE2_NEWLINE_CRLF, 0, 1 | F_NOMATCH, "^", "\r\n" },
	{ M, PCRE2_NEWLINE_CRLF, 0, 1, "^", "\r\na" },

	/* Any character except newline or any newline. */
	{ 0, PCRE2_NEWLINE_CRLF, 0, 0, ".", "\r" },
	{ U, PCRE2_NEWLINE_CRLF, 0, 0, ".(.).", "a\xc3\xa1\r\n\n\r\r" },
	{ 0, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".(.)", "a\rb\nc\r\n\xc2\x85\xe2\x80\xa8" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0, ".(.)", "a\rb\nc\r\n\xc2\x85\xe2\x80\xa8" },
	{ U, PCRE2_NEWLINE_ANY, 0, 0, "(.).", "a\rb\nc\r\n\xc2\x85\xe2\x80\xa9$de" },
	{ U, PCRE2_NEWLINE_ANYCRLF, 0, 0 | F_NOMATCH, ".(.).", "\xe2\x80\xa8\nb\r" },
	{ 0, PCRE2_NEWLINE_ANY, 0, 0, "(.)(.)", "#\x85#\r#\n#\r\n#\x84" },
	{ U, PCRE2_NEWLINE_ANY, 0, 0, "(.+)#", "#\rMn\xc2\x85#\n###" },
	{ 0, BSR(PCRE2_BSR_ANYCRLF), 0, 0, "\\R", "\r" },
	{ 0, BSR(PCRE2_BSR_ANYCRLF), 0, 0, "\\R", "\x85#\r\n#" },
	{ U, BSR(PCRE2_BSR_UNICODE), 0, 0, "\\R", "ab\xe2\x80\xa8#c" },
	{ U, BSR(PCRE2_BSR_UNICODE), 0, 0, "\\R", "ab\r\nc" },
	{ U, PCRE2_NEWLINE_CRLF | BSR(PCRE2_BSR_UNICODE), 0, 0, "(\\R.)+", "\xc2\x85\r\n#\xe2\x80\xa8\n\r\n\r" },
	{ MU, A, 0, 0 | F_NOMATCH, "\\R+", "ab" },
	{ MU, A, 0, 0, "\\R+", "ab\r\n\r" },
	{ MU, A, 0, 0, "\\R*", "ab\r\n\r" },
	{ MU, A, 0, 0, "\\R*", "\r\n\r" },
	{ MU, A, 0, 0, "\\R{2,4}", "\r\nab\r\r" },
	{ MU, A, 0, 0, "\\R{2,4}", "\r\nab\n\n\n\r\r\r" },
	{ MU, A, 0, 0, "\\R{2,}", "\r\nab\n\n\n\r\r\r" },
	{ MU, A, 0, 0, "\\R{0,3}", "\r\n\r\n\r\n\r\n\r\n" },
	{ MU, A, 0, 0 | F_NOMATCH, "\\R+\\R\\R", "\r\n\r\n" },
	{ MU, A, 0, 0, "\\R+\\R\\R", "\r\r\r" },
	{ MU, A, 0, 0, "\\R*\\R\\R", "\n\r" },
	{ MU, A, 0, 0 | F_NOMATCH, "\\R{2,4}\\R\\R", "\r\r\r" },
	{ MU, A, 0, 0, "\\R{2,4}\\R\\R", "\r\r\r\r" },

	/* Atomic groups (no fallback from "next" direction). */
	{ MU, A, 0, 0 | F_NOMATCH, "(?>ab)ab", "bab" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?>(ab))ab", "bab" },
	{ MU, A, 0, 0, "(?>ab)+abc(?>de)*def(?>gh)?ghe(?>ij)+?k(?>lm)*?n(?>op)?\?op",
			"bababcdedefgheijijklmlmnop" },
	{ MU, A, 0, 0, "(?>a(b)+a|(ab)?\?(b))an", "abban" },
	{ MU, A, 0, 0, "(?>ab+a|(?:ab)?\?b)an", "abban" },
	{ MU, A, 0, 0, "((?>ab|ad|)*?)(?>|c)*abad", "abababcababad" },
	{ MU, A, 0, 0, "(?>(aa|b|)*+(?>(##)|###)*d|(aa)(?>(baa)?)m)", "aabaa#####da" },
	{ MU, A, 0, 0, "((?>a|)+?)b", "aaacaaab" },
	{ MU, A, 0, 0, "(?>x|)*$", "aaa" },
	{ MU, A, 0, 0, "(?>(x)|)*$", "aaa" },
	{ MU, A, 0, 0, "(?>x|())*$", "aaa" },
	{ MU, A, 0, 0, "((?>[cxy]a|[a-d])*?)b", "aaa+ aaab" },
	{ MU, A, 0, 0, "((?>[cxy](a)|[a-d])*?)b", "aaa+ aaab" },
	{ MU, A, 0, 0, "(?>((?>(a+))))bab|(?>((?>(a+))))bb", "aaaabaaabaabab" },
	{ MU, A, 0, 0, "(?>(?>a+))bab|(?>(?>a+))bb", "aaaabaaabaabab" },
	{ MU, A, 0, 0, "(?>(a)c|(?>(c)|(a))a)b*?bab", "aaaabaaabaabab" },
	{ MU, A, 0, 0, "(?>ac|(?>c|a)a)b*?bab", "aaaabaaabaabab" },
	{ MU, A, 0, 0, "(?>(b)b|(a))*b(?>(c)|d)?x", "ababcaaabdbx" },
	{ MU, A, 0, 0, "(?>bb|a)*b(?>c|d)?x", "ababcaaabdbx" },
	{ MU, A, 0, 0, "(?>(bb)|a)*b(?>c|(d))?x", "ababcaaabdbx" },
	{ MU, A, 0, 0, "(?>(a))*?(?>(a))+?(?>(a))??x", "aaaaaacccaaaaabax" },
	{ MU, A, 0, 0, "(?>a)*?(?>a)+?(?>a)??x", "aaaaaacccaaaaabax" },
	{ MU, A, 0, 0, "(?>(a)|)*?(?>(a)|)+?(?>(a)|)??x", "aaaaaacccaaaaabax" },
	{ MU, A, 0, 0, "(?>a|)*?(?>a|)+?(?>a|)??x", "aaaaaacccaaaaabax" },
	{ MU, A, 0, 0, "(?>a(?>(a{0,2}))*?b|aac)+b", "aaaaaaacaaaabaaaaacaaaabaacaaabb" },
	{ CM, A, 0, 0, "(?>((?>a{32}|b+|(a*))?(?>c+|d*)?\?)+e)+?f", "aaccebbdde bbdaaaccebbdee bbdaaaccebbdeef" },
	{ MU, A, 0, 0, "(?>(?:(?>aa|a||x)+?b|(?>aa|a||(x))+?c)?(?>[ad]{0,2})*?d)+d", "aaacdbaabdcabdbaaacd aacaabdbdcdcaaaadaabcbaadd" },
	{ MU, A, 0, 0, "(?>(?:(?>aa|a||(x))+?b|(?>aa|a||x)+?c)?(?>[ad]{0,2})*?d)+d", "aaacdbaabdcabdbaaacd aacaabdbdcdcaaaadaabcbaadd" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X", "\xcc\x8d\xcc\x8d" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X", "\xcc\x8d\xcc\x8d#\xcc\x8d\xcc\x8d" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X+..", "\xcc\x8d#\xcc\x8d#\xcc\x8d\xcc\x8d" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X{2,4}", "abcdef" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X{2,4}?", "abcdef" },
	{ MU, A, 0, 0 | F_NOMATCH | F_PROPERTY, "\\X{2,4}..", "#\xcc\x8d##" },
	{ MU, A, 0, 0 | F_PROPERTY, "\\X{2,4}..", "#\xcc\x8d#\xcc\x8d##" },
	{ MU, A, 0, 0, "(c(ab)?+ab)+", "cabcababcab" },
	{ MU, A, 0, 0, "(?>(a+)b)+aabab", "aaaabaaabaabab" },

	/* Possessive quantifiers. */
	{ MU, A, 0, 0, "(?:a|b)++m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(?:a|b)*+m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(?:a|b)*+m", "ababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|b)++m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|b)*+m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|b)*+m", "ababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|b(*ACCEPT))++m", "maaxab" },
	{ MU, A, 0, 0, "(?:b*)++m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(?:b*)++m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "(?:b*)*+m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(?:b*)*+m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "(b*)++m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(b*)++m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "(b*)*+m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(b*)*+m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "(?:a|(b))++m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(?:(a)|b)*+m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "(?:(a)|(b))*+m", "ababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|(b))++m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "((a)|b)*+m", "mababbaaxababbaam" },
	{ MU, A, 0, 0, "((a)|(b))*+m", "ababbaaxababbaam" },
	{ MU, A, 0, 0, "(a|(b)(*ACCEPT))++m", "maaxab" },
	{ MU, A, 0, 0, "(?:(b*))++m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(?:(b*))++m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "(?:(b*))*+m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "(?:(b*))*+m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "((b*))++m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "((b*))++m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0, "((b*))*+m", "bxbbxbbbxm" },
	{ MU, A, 0, 0, "((b*))*+m", "bxbbxbbbxbbm" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?>(b{2,4}))(?:(?:(aa|c))++m|(?:(aa|c))+n)", "bbaacaaccaaaacxbbbmbn" },
	{ MU, A, 0, 0, "((?:b)++a)+(cd)*+m", "bbababbacdcdnbbababbacdcdm" },
	{ MU, A, 0, 0, "((?:(b))++a)+((c)d)*+m", "bbababbacdcdnbbababbacdcdm" },
	{ MU, A, 0, 0, "(?:(?:(?:ab)*+k)++(?:n(?:cd)++)*+)*+m", "ababkkXababkkabkncXababkkabkncdcdncdXababkkabkncdcdncdkkabkncdXababkkabkncdcdncdkkabkncdm" },
	{ MU, A, 0, 0, "(?:((ab)*+(k))++(n(?:c(d))++)*+)*+m", "ababkkXababkkabkncXababkkabkncdcdncdXababkkabkncdcdncdkkabkncdXababkkabkncdcdncdkkabkncdm" },

	/* Back references. */
	{ MU, A, 0, 0, "(aa|bb)(\\1*)(ll|)(\\3*)bbbbbbc", "aaaaaabbbbbbbbc" },
	{ CMU, A, 0, 0, "(aa|bb)(\\1+)(ll|)(\\3+)bbbbbbc", "bBbbBbCbBbbbBbbcbbBbbbBBbbC" },
	{ CM, A, 0, 0, "(a{2,4})\\1", "AaAaaAaA" },
	{ MU, A, 0, 0, "(aa|bb)(\\1?)aa(\\1?)(ll|)(\\4+)bbc", "aaaaaaaabbaabbbbaabbbbc" },
	{ MU, A, 0, 0, "(aa|bb)(\\1{0,5})(ll|)(\\3{0,5})cc", "bbxxbbbbxxaaaaaaaaaaaaaaaacc" },
	{ MU, A, 0, 0, "(aa|bb)(\\1{3,5})(ll|)(\\3{3,5})cc", "bbbbbbbbbbbbaaaaaaccbbbbbbbbbbbbbbcc" },
	{ MU, A, 0, 0, "(aa|bb)(\\1{3,})(ll|)(\\3{3,})cc", "bbbbbbbbbbbbaaaaaaccbbbbbbbbbbbbbbcc" },
	{ MU, A, 0, 0, "(\\w+)b(\\1+)c", "GabGaGaDbGaDGaDc" },
	{ MU, A, 0, 0, "(?:(aa)|b)\\1?b", "bb" },
	{ CMU, A, 0, 0, "(aa|bb)(\\1*?)aa(\\1+?)", "bBBbaaAAaaAAaa" },
	{ MU, A, 0, 0, "(aa|bb)(\\1*?)(dd|)cc(\\3+?)", "aaaaaccdd" },
	{ CMU, A, 0, 0, "(?:(aa|bb)(\\1?\?)cc){2}(\\1?\?)", "aAaABBbbAAaAcCaAcCaA" },
	{ MU, A, 0, 0, "(?:(aa|bb)(\\1{3,5}?)){2}(dd|)(\\3{3,5}?)", "aaaaaabbbbbbbbbbaaaaaaaaaaaaaa" },
	{ CM, A, 0, 0, "(?:(aa|bb)(\\1{3,}?)){2}(dd|)(\\3{3,}?)", "aaaaaabbbbbbbbbbaaaaaaaaaaaaaa" },
	{ MU, A, 0, 0, "(?:(aa|bb)(\\1{0,3}?)){2}(dd|)(\\3{0,3}?)b(\\1{0,3}?)(\\1{0,3})", "aaaaaaaaaaaaaaabaaaaa" },
	{ MU, A, 0, 0, "(a(?:\\1|)a){3}b", "aaaaaaaaaaab" },
	{ M, A, 0, 0, "(a?)b(\\1\\1*\\1+\\1?\\1*?\\1+?\\1??\\1*+\\1++\\1?+\\1{4}\\1{3,5}\\1{4,}\\1{0,5}\\1{3,5}?\\1{4,}?\\1{0,5}?\\1{3,5}+\\1{4,}+\\1{0,5}+#){2}d", "bb#b##d" },
	{ MUP, A, 0, 0 | F_PROPERTY, "(\\P{N})\\1{2,}", ".www." },
	{ MUP, A, 0, 0 | F_PROPERTY, "(\\P{N})\\1{0,2}", "wwwww." },
	{ MUP, A, 0, 0 | F_PROPERTY, "(\\P{N})\\1{1,2}ww", "wwww" },
	{ MUP, A, 0, 0 | F_PROPERTY, "(\\P{N})\\1{1,2}ww", "wwwww" },
	{ PCRE2_UCP, 0, 0, 0 | F_PROPERTY, "(\\P{N})\\1{2,}", ".www." },
	{ CMUP, A, 0, 0, "(\xf0\x90\x90\x80)\\1", "\xf0\x90\x90\xa8\xf0\x90\x90\xa8" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0 | F_NOMATCH, "\\k<A>{1,3}(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES | PCRE2_MATCH_UNSET_BACKREF, A, 0, 0, "\\k<A>{1,3}(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES | PCRE2_MATCH_UNSET_BACKREF, A, 0, 0, "\\k<A>*(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?<A>aa)(?<A>bb)\\k<A>{0,3}aaaaaa", "aabbaaaaaa" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?<A>aa)(?<A>bb)\\k<A>{2,5}bb", "aabbaaaabb" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>aa)|(?<A>bb))\\k<A>{0,3}m", "aaaaaaaabbbbaabbbbm" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0 | F_NOMATCH, "\\k<A>{1,3}?(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES | PCRE2_MATCH_UNSET_BACKREF, A, 0, 0, "\\k<A>{1,3}?(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "\\k<A>*?(?<A>aa)(?<A>bb)", "aabb" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>aa)|(?<A>bb))\\k<A>{0,3}?m", "aaaaaabbbbbbaabbbbbbbbbbm" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>aa)|(?<A>bb))\\k<A>*?m", "aaaaaabbbbbbaabbbbbbbbbbm" },
	{ MU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>aa)|(?<A>bb))\\k<A>{2,3}?", "aaaabbbbaaaabbbbbbbbbb" },
	{ CMU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>AA)|(?<A>BB))\\k<A>{0,3}M", "aaaaaaaabbbbaabbbbm" },
	{ CMU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>AA)|(?<A>BB))\\k<A>{1,3}M", "aaaaaaaabbbbaabbbbm" },
	{ CMU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>AA)|(?<A>BB))\\k<A>{0,3}?M", "aaaaaabbbbbbaabbbbbbbbbbm" },
	{ CMU | PCRE2_DUPNAMES, A, 0, 0, "(?:(?<A>AA)|(?<A>BB))\\k<A>{2,3}?", "aaaabbbbaaaabbbbbbbbbb" },

	/* Assertions. */
	{ MU, A, 0, 0, "(?=xx|yy|zz)\\w{4}", "abczzdefg" },
	{ MU, A, 0, 0, "(?=((\\w+)b){3}|ab)", "dbbbb ab" },
	{ MU, A, 0, 0, "(?!ab|bc|cd)[a-z]{2}", "Xabcdef" },
	{ MU, A, 0, 0, "(?<=aaa|aa|a)a", "aaa" },
	{ MU, A, 0, 2, "(?<=aaa|aa|a)a", "aaa" },
	{ M, A, 0, 0, "(?<=aaa|aa|a)a", "aaa" },
	{ M, A, 0, 2, "(?<=aaa|aa|a)a", "aaa" },
	{ MU, A, 0, 0, "(\\d{2})(?!\\w+c|(((\\w?)m){2}n)+|\\1)", "x5656" },
	{ MU, A, 0, 0, "((?=((\\d{2,6}\\w){2,}))\\w{5,20}K){2,}", "567v09708K12l00M00 567v09708K12l00M00K45K" },
	{ MU, A, 0, 0, "(?=(?:(?=\\S+a)\\w*(b)){3})\\w+\\d", "bba bbab nbbkba nbbkba0kl" },
	{ MU, A, 0, 0, "(?>a(?>(b+))a(?=(..)))*?k", "acabbcabbaabacabaabbakk" },
	{ MU, A, 0, 0, "((?(?=(a))a)+k)", "bbak" },
	{ MU, A, 0, 0, "((?(?=a)a)+k)", "bbak" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?=(?>(a))m)amk", "a k" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?!(?>(a))m)amk", "a k" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?>(?=(a))am)amk", "a k" },
	{ MU, A, 0, 0, "(?=(?>a|(?=(?>(b+))a|c)[a-c]+)*?m)[a-cm]+k", "aaam bbam baaambaam abbabba baaambaamk" },
	{ MU, A, 0, 0, "(?> ?\?\\b(?(?=\\w{1,4}(a))m)\\w{0,8}bc){2,}?", "bca ssbc mabd ssbc mabc" },
	{ MU, A, 0, 0, "(?:(?=ab)?[^n][^n])+m", "ababcdabcdcdabnababcdabcdcdabm" },
	{ MU, A, 0, 0, "(?:(?=a(b))?[^n][^n])+m", "ababcdabcdcdabnababcdabcdcdabm" },
	{ MU, A, 0, 0, "(?:(?=.(.))??\\1.)+m", "aabbbcbacccanaabbbcbacccam" },
	{ MU, A, 0, 0, "(?:(?=.)??[a-c])+m", "abacdcbacacdcaccam" },
	{ MU, A, 0, 0, "((?!a)?(?!([^a]))?)+$", "acbab" },
	{ MU, A, 0, 0, "((?!a)?\?(?!([^a]))?\?)+$", "acbab" },
	{ MU, A, 0, 0, "a(?=(?C)\\B(?C`x`))b", "ab" },
	{ MU, A, 0, 0, "a(?!(?C)\\B(?C`x`))bb|ab", "abb" },
	{ MU, A, 0, 0, "a(?=\\b|(?C)\\B(?C`x`))b", "ab" },
	{ MU, A, 0, 0, "a(?!\\b|(?C)\\B(?C`x`))bb|ab", "abb" },
	{ MU, A, 0, 0, "c(?(?=(?C)\\B(?C`x`))ab|a)", "cab" },
	{ MU, A, 0, 0, "c(?(?!(?C)\\B(?C`x`))ab|a)", "cab" },
	{ MU, A, 0, 0, "c(?(?=\\b|(?C)\\B(?C`x`))ab|a)", "cab" },
	{ MU, A, 0, 0, "c(?(?!\\b|(?C)\\B(?C`x`))ab|a)", "cab" },
	{ MU, A, 0, 0, "a(?=)b", "ab" },
	{ MU, A, 0, 0 | F_NOMATCH, "a(?!)b", "ab" },

	/* Not empty, ACCEPT, FAIL */
	{ MU, A, PCRE2_NOTEMPTY, 0 | F_NOMATCH, "a*", "bcx" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "a*", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "a*?", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY_ATSTART, 0, "a*", "bcaad" },
	{ MU, A, 0, 0, "a(*ACCEPT)b", "ab" },
	{ MU, A, PCRE2_NOTEMPTY, 0 | F_NOMATCH, "a*(*ACCEPT)b", "bcx" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "a*(*ACCEPT)b", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "a*?(*ACCEPT)b", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY, 0 | F_NOMATCH, "(?:z|a*(*ACCEPT)b)", "bcx" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "(?:z|a*(*ACCEPT)b)", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "(?:z|a*?(*ACCEPT)b)", "bcaad" },
	{ MU, A, PCRE2_NOTEMPTY_ATSTART, 0, "a*(*ACCEPT)b", "bcx" },
	{ MU, A, PCRE2_NOTEMPTY_ATSTART, 0 | F_NOMATCH, "a*(*ACCEPT)b", "" },
	{ MU, A, 0, 0, "((a(*ACCEPT)b))", "ab" },
	{ MU, A, 0, 0, "(a(*FAIL)a|a)", "aaa" },
	{ MU, A, 0, 0, "(?=ab(*ACCEPT)b)a", "ab" },
	{ MU, A, 0, 0, "(?=(?:x|ab(*ACCEPT)b))", "ab" },
	{ MU, A, 0, 0, "(?=(a(b(*ACCEPT)b)))a", "ab" },
	{ MU, A, PCRE2_NOTEMPTY, 0, "(?=a*(*ACCEPT))c", "c" },
	{ MU, A, PCRE2_NOTEMPTY, 0 | F_NOMATCH, "(?=A)", "AB" },

	/* Conditional blocks. */
	{ MU, A, 0, 0, "(?(?=(a))a|b)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?!(b))a|b)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?=a)a|b)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?!b)a|b)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?=(a))a*|b*)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?!(b))a*|b*)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?!(b))(?:aaaaaa|a)|(?:bbbbbb|b))+aaaak", "aaaaaaaaaaaaaa bbbbbbbbbbbbbbb aaaaaaak" },
	{ MU, A, 0, 0, "(?(?!b)(?:aaaaaa|a)|(?:bbbbbb|b))+aaaak", "aaaaaaaaaaaaaa bbbbbbbbbbbbbbb aaaaaaak" },
	{ MU, A, 0, 0 | F_DIFF, "(?(?!(b))(?:aaaaaa|a)|(?:bbbbbb|b))+bbbbk", "aaaaaaaaaaaaaa bbbbbbbbbbbbbbb bbbbbbbk" },
	{ MU, A, 0, 0, "(?(?!b)(?:aaaaaa|a)|(?:bbbbbb|b))+bbbbk", "aaaaaaaaaaaaaa bbbbbbbbbbbbbbb bbbbbbbk" },
	{ MU, A, 0, 0, "(?(?=a)a*|b*)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?!b)a*|b*)+k", "ababbalbbadabak" },
	{ MU, A, 0, 0, "(?(?=a)ab)", "a" },
	{ MU, A, 0, 0, "(?(?<!b)c)", "b" },
	{ MU, A, 0, 0, "(?(DEFINE)a(b))", "a" },
	{ MU, A, 0, 0, "a(?(DEFINE)(?:b|(?:c?)+)*)", "a" },
	{ MU, A, 0, 0, "(?(?=.[a-c])[k-l]|[A-D])", "kdB" },
	{ MU, A, 0, 0, "(?(?!.{0,4}[cd])(aa|bb)|(cc|dd))+", "aabbccddaa" },
	{ MU, A, 0, 0, "(?(?=[^#@]*@)(aaab|aa|aba)|(aba|aab)){3,}", "aaabaaaba#aaabaaaba#aaabaaaba@" },
	{ MU, A, 0, 0, "((?=\\w{5})\\w(?(?=\\w*k)\\d|[a-f_])*\\w\\s)+", "mol m10kk m088k _f_a_ mbkkl" },
	{ MU, A, 0, 0, "(c)?\?(?(1)a|b)", "cdcaa" },
	{ MU, A, 0, 0, "(c)?\?(?(1)a|b)", "cbb" },
	{ MU, A, 0, 0 | F_DIFF, "(?(?=(a))(aaaa|a?))+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?=a)(aaaa|a?))+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?!(b))(aaaa|a?))+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?!b)(aaaa|a?))+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0 | F_DIFF, "(?(?=(a))a*)+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?=a)a*)+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?!(b))a*)+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?!b)a*)+aak", "aaaaab aaaaak" },
	{ MU, A, 0, 0, "(?(?=(?=(?!(x))a)aa)aaa|(?(?=(?!y)bb)bbb))*k", "abaabbaaabbbaaabbb abaabbaaabbbaaabbbk" },
	{ MU, A, 0, 0, "(?P<Name>a)?(?P<Name2>b)?(?(Name)c|d)*l", "bc ddd abccabccl" },
	{ MU, A, 0, 0, "(?P<Name>a)?(?P<Name2>b)?(?(Name)c|d)+?dd", "bcabcacdb bdddd" },
	{ MU, A, 0, 0, "(?P<Name>a)?(?P<Name2>b)?(?(Name)c|d)+l", "ababccddabdbccd abcccl" },
	{ MU, A, 0, 0, "((?:a|aa)(?(1)aaa))x", "aax" },
	{ MU, A, 0, 0, "(?(?!)a|b)", "ab" },
	{ MU, A, 0, 0, "(?(?!)a)", "ab" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?(?!)a|b)", "ac" },

	/* Set start of match. */
	{ MU, A, 0, 0, "(?:\\Ka)*aaaab", "aaaaaaaa aaaaaaabb" },
	{ MU, A, 0, 0, "(?>\\Ka\\Ka)*aaaab", "aaaaaaaa aaaaaaaaaabb" },
	{ MU, A, 0, 0, "a+\\K(?<=\\Gaa)a", "aaaaaa" },
	{ MU, A, PCRE2_NOTEMPTY, 0 | F_NOMATCH, "a\\K(*ACCEPT)b", "aa" },
	{ MU, A, PCRE2_NOTEMPTY_ATSTART, 0, "a\\K(*ACCEPT)b", "aa" },

	/* First line. */
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_PROPERTY, "\\p{Any}a", "bb\naaa" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH | F_PROPERTY, "\\p{Any}a", "bb\r\naaa" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0, "(?<=a)", "a" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH, "[^a][^b]", "ab" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH, "a", "\na" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH, "[abc]", "\na" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH, "^a", "\na" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0 | F_NOMATCH, "^(?<=\n)", "\na" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0, "\xf0\x90\x90\x80", "\xf0\x90\x90\x80" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_ANY, 0, 0 | F_NOMATCH, "#", "\xc2\x85#" },
	{ M | PCRE2_FIRSTLINE, PCRE2_NEWLINE_ANY, 0, 0 | F_NOMATCH, "#", "\x85#" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_ANY, 0, 0 | F_NOMATCH, "^#", "\xe2\x80\xa8#" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 0 | F_PROPERTY, "\\p{Any}", "\r\na" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 0, ".", "\r" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 0, "a", "\ra" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 0 | F_NOMATCH, "ba", "bbb\r\nba" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 0 | F_NOMATCH | F_PROPERTY, "\\p{Any}{4}|a", "\r\na" },
	{ MU | PCRE2_FIRSTLINE, PCRE2_NEWLINE_CRLF, 0, 1, ".", "\r\n" },
	{ PCRE2_FIRSTLINE | PCRE2_DOTALL, PCRE2_NEWLINE_LF, 0, 0 | F_NOMATCH, "ab.", "ab" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 1 | F_NOMATCH, "^[a-d0-9]", "\nxx\nd" },
	{ PCRE2_FIRSTLINE | PCRE2_DOTALL, PCRE2_NEWLINE_ANY, 0, 0, "....a", "012\n0a" },
	{ MU | PCRE2_FIRSTLINE, A, 0, 0, "[aC]", "a" },

	/* Recurse. */
	{ MU, A, 0, 0, "(a)(?1)", "aa" },
	{ MU, A, 0, 0, "((a))(?1)", "aa" },
	{ MU, A, 0, 0, "(b|a)(?1)", "aa" },
	{ MU, A, 0, 0, "(b|(a))(?1)", "aa" },
	{ MU, A, 0, 0 | F_NOMATCH, "((a)(b)(?:a*))(?1)", "aba" },
	{ MU, A, 0, 0, "((a)(b)(?:a*))(?1)", "abab" },
	{ MU, A, 0, 0, "((a+)c(?2))b(?1)", "aacaabaca" },
	{ MU, A, 0, 0, "((?2)b|(a)){2}(?1)", "aabab" },
	{ MU, A, 0, 0, "(?1)(a)*+(?2)(b(?1))", "aababa" },
	{ MU, A, 0, 0, "(?1)(((a(*ACCEPT)))b)", "axaa" },
	{ MU, A, 0, 0, "(?1)(?(DEFINE) (((ac(*ACCEPT)))b) )", "akaac" },
	{ MU, A, 0, 0, "(a+)b(?1)b\\1", "abaaabaaaaa" },
	{ MU, A, 0, 0, "(?(DEFINE)(aa|a))(?1)ab", "aab" },
	{ MU, A, 0, 0, "(?(DEFINE)(a\\Kb))(?1)+ababc", "abababxabababc" },
	{ MU, A, 0, 0, "(a\\Kb)(?1)+ababc", "abababxababababc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(a\\Kb)(?1)+ababc", "abababxababababxc" },
	{ MU, A, 0, 0, "b|<(?R)*>", "<<b>" },
	{ MU, A, 0, 0, "(a\\K){0}(?:(?1)b|ac)", "ac" },
	{ MU, A, 0, 0, "(?(DEFINE)(a(?2)|b)(b(?1)|(a)))(?:(?1)|(?2))m", "ababababnababababaam" },
	{ MU, A, 0, 0, "(a)((?(R)a|b))(?2)", "aabbabaa" },
	{ MU, A, 0, 0, "(a)((?(R2)a|b))(?2)", "aabbabaa" },
	{ MU, A, 0, 0, "(a)((?(R1)a|b))(?2)", "ababba" },
	{ MU, A, 0, 0, "(?(R0)aa|bb(?R))", "abba aabb bbaa" },
	{ MU, A, 0, 0, "((?(R)(?:aaaa|a)|(?:(aaaa)|(a)))+)(?1)$", "aaaaaaaaaa aaaa" },
	{ MU, A, 0, 0, "(?P<Name>a(?(R&Name)a|b))(?1)", "aab abb abaa" },
	{ MU, A, 0, 0, "((?(R)a|(?1)){3})", "XaaaaaaaaaX" },
	{ MU, A, 0, 0, "((?:(?(R)a|(?1))){3})", "XaaaaaaaaaX" },
	{ MU, A, 0, 0, "((?(R)a|(?1)){1,3})aaaaaa", "aaaaaaaaXaaaaaaaaa" },
	{ MU, A, 0, 0, "((?(R)a|(?1)){1,3}?)M", "aaaM" },
	{ MU, A, 0, 0, "((.)(?:.|\\2(?1))){0}#(?1)#", "#aabbccdde# #aabbccddee#" },
	{ MU, A, 0, 0, "((.)(?:\\2|\\2{4}b)){0}#(?:(?1))+#", "#aaaab# #aaaaab#" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?1)$((.|\\2xx){1,2})", "abc" },

	/* 16 bit specific tests. */
	{ CM, A, 0, 0 | F_FORCECONV, "\xc3\xa1", "\xc3\x81\xc3\xa1" },
	{ CM, A, 0, 0 | F_FORCECONV, "\xe1\xbd\xb8", "\xe1\xbf\xb8\xe1\xbd\xb8" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xc3\xa1]", "\xc3\x81\xc3\xa1" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xe1\xbd\xb8]", "\xe1\xbf\xb8\xe1\xbd\xb8" },
	{ CM, A, 0, 0 | F_FORCECONV, "[a-\xed\xb0\x80]", "A" },
	{ CM, A, 0, 0 | F_NO8 | F_FORCECONV, "[a-\\x{dc00}]", "B" },
	{ CM, A, 0, 0 | F_NO8 | F_NOMATCH | F_FORCECONV, "[b-\\x{dc00}]", "a" },
	{ CM, A, 0, 0 | F_NO8 | F_FORCECONV, "\xed\xa0\x80\\x{d800}\xed\xb0\x80\\x{dc00}", "\xed\xa0\x80\xed\xa0\x80\xed\xb0\x80\xed\xb0\x80" },
	{ CM, A, 0, 0 | F_NO8 | F_FORCECONV, "[\xed\xa0\x80\\x{d800}]{1,2}?[\xed\xb0\x80\\x{dc00}]{1,2}?#", "\xed\xa0\x80\xed\xa0\x80\xed\xb0\x80\xed\xb0\x80#" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xed\xa0\x80\xed\xb0\x80#]{0,3}(?<=\xed\xb0\x80.)", "\xed\xa0\x80#\xed\xa0\x80##\xed\xb0\x80\xed\xa0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xed\xa0\x80-\xed\xb3\xbf]", "\xed\x9f\xbf\xed\xa0\x83" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xed\xa0\x80-\xed\xb3\xbf]", "\xed\xb4\x80\xed\xb3\xb0" },
	{ CM, A, 0, 0 | F_NO8 | F_FORCECONV, "[\\x{d800}-\\x{dcff}]", "\xed\x9f\xbf\xed\xa0\x83" },
	{ CM, A, 0, 0 | F_NO8 | F_FORCECONV, "[\\x{d800}-\\x{dcff}]", "\xed\xb4\x80\xed\xb3\xb0" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xed\xa0\x80-\xef\xbf\xbf]+[\x1-\xed\xb0\x80]+#", "\xed\xa0\x85\xc3\x81\xed\xa0\x85\xef\xbf\xb0\xc2\x85\xed\xa9\x89#" },
	{ CM, A, 0, 0 | F_FORCECONV, "[\xed\xa0\x80][\xed\xb0\x80]{2,}", "\xed\xa0\x80\xed\xb0\x80\xed\xa0\x80\xed\xb0\x80\xed\xb0\x80\xed\xb0\x80" },
	{ M, A, 0, 0 | F_FORCECONV, "[^\xed\xb0\x80]{3,}?", "##\xed\xb0\x80#\xed\xb0\x80#\xc3\x89#\xed\xb0\x80" },
	{ M, A, 0, 0 | F_NO8 | F_FORCECONV, "[^\\x{dc00}]{3,}?", "##\xed\xb0\x80#\xed\xb0\x80#\xc3\x89#\xed\xb0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV, ".\\B.", "\xed\xa0\x80\xed\xb0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV, "\\D+(?:\\d+|.)\\S+(?:\\s+|.)\\W+(?:\\w+|.)\xed\xa0\x80\xed\xa0\x80", "\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV, "\\d*\\s*\\w*\xed\xa0\x80\xed\xa0\x80", "\xed\xa0\x80\xed\xa0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV | F_NOMATCH, "\\d*?\\D*?\\s*?\\S*?\\w*?\\W*?##", "\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80#" },
	{ CM | PCRE2_EXTENDED, A, 0, 0 | F_FORCECONV, "\xed\xa0\x80 \xed\xb0\x80 !", "\xed\xa0\x80\xed\xb0\x80!" },
	{ CM, A, 0, 0 | F_FORCECONV, "\xed\xa0\x80+#[^#]+\xed\xa0\x80", "\xed\xa0\x80#a\xed\xa0\x80" },
	{ CM, A, 0, 0 | F_FORCECONV, "(\xed\xa0\x80+)#\\1", "\xed\xa0\x80\xed\xa0\x80#\xed\xa0\x80\xed\xa0\x80" },
	{ M, PCRE2_NEWLINE_ANY, 0, 0 | F_NO8 | F_FORCECONV, "^-", "a--\xe2\x80\xa8--" },
	{ 0, BSR(PCRE2_BSR_UNICODE), 0, 0 | F_NO8 | F_FORCECONV, "\\R", "ab\xe2\x80\xa8" },
	{ 0, 0, 0, 0 | F_NO8 | F_FORCECONV, "\\v", "ab\xe2\x80\xa9" },
	{ 0, 0, 0, 0 | F_NO8 | F_FORCECONV, "\\h", "ab\xe1\xa0\x8e" },
	{ 0, 0, 0, 0 | F_NO8 | F_FORCECONV, "\\v+?\\V+?#", "\xe2\x80\xa9\xe2\x80\xa9\xef\xbf\xbf\xef\xbf\xbf#" },
	{ 0, 0, 0, 0 | F_NO8 | F_FORCECONV, "\\h+?\\H+?#", "\xe1\xa0\x8e\xe1\xa0\x8e\xef\xbf\xbf\xef\xbf\xbf#" },

	/* Partial matching. */
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "ab", "a" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "ab|a", "a" },
	{ MU, A, PCRE2_PARTIAL_HARD, 0, "ab|a", "a" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "\\b#", "a" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "(?<=a)b", "a" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "abc|(?<=xxa)bc", "xxab" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "a\\B", "a" },
	{ MU, A, PCRE2_PARTIAL_HARD, 0, "a\\b", "a" },

	/* (*MARK) verb. */
	{ MU, A, 0, 0, "a(*MARK:aa)a", "ababaa" },
	{ MU, A, 0, 0 | F_NOMATCH, "a(*:aa)a", "abab" },
	{ MU, A, 0, 0, "a(*:aa)(b(*:bb)b|bc)", "abc" },
	{ MU, A, 0, 0 | F_NOMATCH, "a(*:1)x|b(*:2)y", "abc" },
	{ MU, A, 0, 0, "(?>a(*:aa))b|ac", "ac" },
	{ MU, A, 0, 0, "(?(DEFINE)(a(*:aa)))(?1)", "a" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?(DEFINE)((a)(*:aa)))(?1)b", "aa" },
	{ MU, A, 0, 0, "(?(DEFINE)(a(*:aa)))a(?1)b|aac", "aac" },
	{ MU, A, 0, 0, "(a(*:aa)){0}(?:b(?1)b|c)+c", "babbab cc" },
	{ MU, A, 0, 0, "(a(*:aa)){0}(?:b(?1)b)+", "babba" },
	{ MU, A, 0, 0 | F_NOMATCH, "(a(*:aa)){0}(?:b(?1)b)+", "ba" },
	{ MU, A, 0, 0, "(a\\K(*:aa)){0}(?:b(?1)b|c)+c", "babbab cc" },
	{ MU, A, 0, 0, "(a\\K(*:aa)){0}(?:b(?1)b)+", "babba" },
	{ MU, A, 0, 0 | F_NOMATCH, "(a\\K(*:aa)){0}(?:b(?1)b)+", "ba" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*:mark)m", "a" },

	/* (*COMMIT) verb. */
	{ MU, A, 0, 0 | F_NOMATCH, "a(*COMMIT)b", "ac" },
	{ MU, A, 0, 0, "aa(*COMMIT)b", "xaxaab" },
	{ MU, A, 0, 0 | F_NOMATCH, "a(*COMMIT)(*:msg)b|ac", "ac" },
	{ MU, A, 0, 0 | F_NOMATCH, "(a(*COMMIT)b)++", "abac" },
	{ MU, A, 0, 0 | F_NOMATCH, "((a)(*COMMIT)b)++", "abac" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?=a(*COMMIT)b)ab|ad", "ad" },

	/* (*PRUNE) verb. */
	{ MU, A, 0, 0, "aa\\K(*PRUNE)b", "aaab" },
	{ MU, A, 0, 0, "aa(*PRUNE:bb)b|a", "aa" },
	{ MU, A, 0, 0, "(a)(a)(*PRUNE)b|(a)", "aa" },
	{ MU, A, 0, 0, "(a)(a)(a)(a)(a)(a)(a)(a)(*PRUNE)b|(a)", "aaaaaaaa" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "a(*PRUNE)a|", "a" },
	{ MU, A, PCRE2_PARTIAL_SOFT, 0, "a(*PRUNE)a|m", "a" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?=a(*PRUNE)b)ab|ad", "ad" },
	{ MU, A, 0, 0, "a(*COMMIT)(*PRUNE)d|bc", "abc" },
	{ MU, A, 0, 0, "(?=a(*COMMIT)b)a(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?=a(*COMMIT)b)a(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0, "(?=(a)(*COMMIT)b)a(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?=(a)(*COMMIT)b)a(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0, "(a(*COMMIT)b){0}a(?1)(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(a(*COMMIT)b){0}a(*COMMIT)(?1)(*PRUNE)c|bc", "abc" },
	{ MU, A, 0, 0, "(a(*COMMIT)b)++(*PRUNE)d|c", "ababc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(a(*COMMIT)b)++(*PRUNE)d|c", "ababc" },
	{ MU, A, 0, 0, "((a)(*COMMIT)b)++(*PRUNE)d|c", "ababc" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)((a)(*COMMIT)b)++(*PRUNE)d|c", "ababc" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)*abab(*PRUNE)d|ba", "ababab" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)*abab(*PRUNE)d|ba", "ababab" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)+abab(*PRUNE)d|ba", "ababab" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)+abab(*PRUNE)d|ba", "ababab" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)?ab(*PRUNE)d|ba", "aba" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)?ab(*PRUNE)d|ba", "aba" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)*?n(*PRUNE)d|ba", "abababn" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)*?n(*PRUNE)d|ba", "abababn" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)+?n(*PRUNE)d|ba", "abababn" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)+?n(*PRUNE)d|ba", "abababn" },
	{ MU, A, 0, 0, "(?>a(*COMMIT)b)??n(*PRUNE)d|bn", "abn" },
	{ MU, A, 0, 0 | F_NOMATCH, "(*COMMIT)(?>a(*COMMIT)b)??n(*PRUNE)d|bn", "abn" },

	/* (*SKIP) verb. */
	{ MU, A, 0, 0 | F_NOMATCH, "(?=a(*SKIP)b)ab|ad", "ad" },
	{ MU, A, 0, 0, "(\\w+(*SKIP)#)", "abcd,xyz#," },
	{ MU, A, 0, 0, "\\w+(*SKIP)#|mm", "abcd,xyz#," },
	{ MU, A, 0, 0 | F_NOMATCH, "b+(?<=(*SKIP)#c)|b+", "#bbb" },

	/* (*THEN) verb. */
	{ MU, A, 0, 0, "((?:a(*THEN)|aab)(*THEN)c|a+)+m", "aabcaabcaabcaabcnacm" },
	{ MU, A, 0, 0 | F_NOMATCH, "((?:a(*THEN)|aab)(*THEN)c|a+)+m", "aabcm" },
	{ MU, A, 0, 0, "((?:a(*THEN)|aab)c|a+)+m", "aabcaabcnmaabcaabcm" },
	{ MU, A, 0, 0, "((?:a|aab)(*THEN)c|a+)+m", "aam" },
	{ MU, A, 0, 0, "((?:a(*COMMIT)|aab)(*THEN)c|a+)+m", "aam" },
	{ MU, A, 0, 0, "(?(?=a(*THEN)b)ab|ad)", "ad" },
	{ MU, A, 0, 0, "(?(?!a(*THEN)b)ad|add)", "add" },
	{ MU, A, 0, 0 | F_NOMATCH, "(?(?=a)a(*THEN)b|ad)", "ad" },
	{ MU, A, 0, 0, "(?!(?(?=a)ab|b(*THEN)d))bn|bnn", "bnn" },
	{ MU, A, 0, 0, "(?=(*THEN: ))* ", " " },
	{ MU, A, 0, 0, "a(*THEN)(?R) |", "a" },

	/* Recurse and control verbs. */
	{ MU, A, 0, 0, "(a(*ACCEPT)b){0}a(?1)b", "aacaabb" },
	{ MU, A, 0, 0, "((a)\\2(*ACCEPT)b){0}a(?1)b", "aaacaaabb" },
	{ MU, A, 0, 0, "((ab|a(*ACCEPT)x)+|ababababax){0}_(?1)_", "_ababababax_ _ababababa_" },
	{ MU, A, 0, 0, "((.)(?:A(*ACCEPT)|(?1)\\2)){0}_(?1)_", "_bcdaAdcb_bcdaAdcb_" },
	{ MU, A, 0, 0, "((*MARK:m)(?:a|a(*COMMIT)b|aa)){0}_(?1)_", "_ab_" },
	{ MU, A, 0, 0, "((*MARK:m)(?:a|a(*COMMIT)b|aa)){0}_(?1)_|(_aa_)", "_aa_" },
	{ MU, A, 0, 0, "(a(*COMMIT)(?:b|bb)|c(*ACCEPT)d|dd){0}_(?1)+_", "_ax_ _cd_ _abbb_ _abcd_ _abbcdd_" },
	{ MU, A, 0, 0, "((.)(?:.|(*COMMIT)\\2{3}(*ACCEPT).*|.*)){0}_(?1){0,4}_", "_aaaabbbbccccddd_ _aaaabbbbccccdddd_" },

#ifdef SUPPORT_UNICODE
	/* Script runs and iterations. */
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)*#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)+#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)*?#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)+?#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)*+#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)++#", "!abcdefghijklmno!abcdefghijklmno!abcdef#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)?#", "!ab!abc!ab!ab#" },
	{ MU, A, 0, 0, "!(*sr:\\w\\w|\\w\\w\\w)??#", "!ab!abc!ab!ab#" },
#endif

	/* Deep recursion. */
	{ MU, A, 0, 0, "((((?:(?:(?:\\w)+)?)*|(?>\\w)+?)+|(?>\\w)?\?)*)?\\s", "aaaaa+ " },
	{ MU, A, 0, 0, "(?:((?:(?:(?:\\w*?)+)??|(?>\\w)?|\\w*+)*)+)+?\\s", "aa+ " },
	{ MU, A, 0, 0, "((a?)+)+b", "aaaaaaaaaaaa b" },

	/* Deep recursion: Stack limit reached. */
	{ M, A, 0, 0 | F_NOMATCH, "a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?aaaaaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaaaaaa" },
	{ M, A, 0, 0 | F_NOMATCH, "(?:a+)+b", "aaaaaaaaaaaaaaaaaaaaaaaa b" },
	{ M, A, 0, 0 | F_NOMATCH, "(?:a+?)+?b", "aaaaaaaaaaaaaaaaaaaaaaaa b" },
	{ M, A, 0, 0 | F_NOMATCH, "(?:a*)*b", "aaaaaaaaaaaaaaaaaaaaaaaa b" },
	{ M, A, 0, 0 | F_NOMATCH, "(?:a*?)*?b", "aaaaaaaaaaaaaaaaaaaaaaaa b" },

	{ 0, 0, 0, 0, NULL, NULL }
};

#ifdef SUPPORT_PCRE2_8
static pcre2_jit_stack_8* callback8(void *arg)
{
	return (pcre2_jit_stack_8 *)arg;
}
#endif

#ifdef SUPPORT_PCRE2_16
static pcre2_jit_stack_16* callback16(void *arg)
{
	return (pcre2_jit_stack_16 *)arg;
}
#endif

#ifdef SUPPORT_PCRE2_32
static pcre2_jit_stack_32* callback32(void *arg)
{
	return (pcre2_jit_stack_32 *)arg;
}
#endif

#ifdef SUPPORT_PCRE2_8
static pcre2_jit_stack_8 *stack8;

static pcre2_jit_stack_8 *getstack8(void)
{
	if (!stack8)
		stack8 = pcre2_jit_stack_create_8(1, 1024 * 1024, NULL);
	return stack8;
}

static void setstack8(pcre2_match_context_8 *mcontext)
{
	if (!mcontext) {
		if (stack8)
			pcre2_jit_stack_free_8(stack8);
		stack8 = NULL;
		return;
	}

	pcre2_jit_stack_assign_8(mcontext, callback8, getstack8());
}
#endif /* SUPPORT_PCRE2_8 */

#ifdef SUPPORT_PCRE2_16
static pcre2_jit_stack_16 *stack16;

static pcre2_jit_stack_16 *getstack16(void)
{
	if (!stack16)
		stack16 = pcre2_jit_stack_create_16(1, 1024 * 1024, NULL);
	return stack16;
}

static void setstack16(pcre2_match_context_16 *mcontext)
{
	if (!mcontext) {
		if (stack16)
			pcre2_jit_stack_free_16(stack16);
		stack16 = NULL;
		return;
	}

	pcre2_jit_stack_assign_16(mcontext, callback16, getstack16());
}
#endif /* SUPPORT_PCRE2_16 */

#ifdef SUPPORT_PCRE2_32
static pcre2_jit_stack_32 *stack32;

static pcre2_jit_stack_32 *getstack32(void)
{
	if (!stack32)
		stack32 = pcre2_jit_stack_create_32(1, 1024 * 1024, NULL);
	return stack32;
}

static void setstack32(pcre2_match_context_32 *mcontext)
{
	if (!mcontext) {
		if (stack32)
			pcre2_jit_stack_free_32(stack32);
		stack32 = NULL;
		return;
	}

	pcre2_jit_stack_assign_32(mcontext, callback32, getstack32());
}
#endif /* SUPPORT_PCRE2_32 */

#ifdef SUPPORT_PCRE2_16

static int convert_utf8_to_utf16(PCRE2_SPTR8 input, PCRE2_UCHAR16 *output, int *offsetmap, int max_length)
{
	PCRE2_SPTR8 iptr = input;
	PCRE2_UCHAR16 *optr = output;
	unsigned int c;

	if (max_length == 0)
		return 0;

	while (*iptr && max_length > 1) {
		c = 0;
		if (offsetmap)
			*offsetmap++ = (int)(iptr - (unsigned char*)input);

		if (*iptr < 0xc0)
			c = *iptr++;
		else if (!(*iptr & 0x20)) {
			c = ((iptr[0] & 0x1f) << 6) | (iptr[1] & 0x3f);
			iptr += 2;
		} else if (!(*iptr & 0x10)) {
			c = ((iptr[0] & 0x0f) << 12) | ((iptr[1] & 0x3f) << 6) | (iptr[2] & 0x3f);
			iptr += 3;
		} else if (!(*iptr & 0x08)) {
			c = ((iptr[0] & 0x07) << 18) | ((iptr[1] & 0x3f) << 12) | ((iptr[2] & 0x3f) << 6) | (iptr[3] & 0x3f);
			iptr += 4;
		}

		if (c < 65536) {
			*optr++ = c;
			max_length--;
		} else if (max_length <= 2) {
			*optr = '\0';
			return (int)(optr - output);
		} else {
			c -= 0x10000;
			*optr++ = 0xd800 | ((c >> 10) & 0x3ff);
			*optr++ = 0xdc00 | (c & 0x3ff);
			max_length -= 2;
			if (offsetmap)
				offsetmap++;
		}
	}
	if (offsetmap)
		*offsetmap = (int)(iptr - (unsigned char*)input);
	*optr = '\0';
	return (int)(optr - output);
}

static int copy_char8_to_char16(PCRE2_SPTR8 input, PCRE2_UCHAR16 *output, int max_length)
{
	PCRE2_SPTR8 iptr = input;
	PCRE2_UCHAR16 *optr = output;

	if (max_length == 0)
		return 0;

	while (*iptr && max_length > 1) {
		*optr++ = *iptr++;
		max_length--;
	}
	*optr = '\0';
	return (int)(optr - output);
}

#define REGTEST_MAX_LENGTH16 4096
static PCRE2_UCHAR16 regtest_buf16[REGTEST_MAX_LENGTH16];
static int regtest_offsetmap16[REGTEST_MAX_LENGTH16];

#endif /* SUPPORT_PCRE2_16 */

#ifdef SUPPORT_PCRE2_32

static int convert_utf8_to_utf32(PCRE2_SPTR8 input, PCRE2_UCHAR32 *output, int *offsetmap, int max_length)
{
	PCRE2_SPTR8 iptr = input;
	PCRE2_UCHAR32 *optr = output;
	unsigned int c;

	if (max_length == 0)
		return 0;

	while (*iptr && max_length > 1) {
		c = 0;
		if (offsetmap)
			*offsetmap++ = (int)(iptr - (unsigned char*)input);

		if (*iptr < 0xc0)
			c = *iptr++;
		else if (!(*iptr & 0x20)) {
			c = ((iptr[0] & 0x1f) << 6) | (iptr[1] & 0x3f);
			iptr += 2;
		} else if (!(*iptr & 0x10)) {
			c = ((iptr[0] & 0x0f) << 12) | ((iptr[1] & 0x3f) << 6) | (iptr[2] & 0x3f);
			iptr += 3;
		} else if (!(*iptr & 0x08)) {
			c = ((iptr[0] & 0x07) << 18) | ((iptr[1] & 0x3f) << 12) | ((iptr[2] & 0x3f) << 6) | (iptr[3] & 0x3f);
			iptr += 4;
		}

		*optr++ = c;
		max_length--;
	}
	if (offsetmap)
		*offsetmap = (int)(iptr - (unsigned char*)input);
	*optr = 0;
	return (int)(optr - output);
}

static int copy_char8_to_char32(PCRE2_SPTR8 input, PCRE2_UCHAR32 *output, int max_length)
{
	PCRE2_SPTR8 iptr = input;
	PCRE2_UCHAR32 *optr = output;

	if (max_length == 0)
		return 0;

	while (*iptr && max_length > 1) {
		*optr++ = *iptr++;
		max_length--;
	}
	*optr = '\0';
	return (int)(optr - output);
}

#define REGTEST_MAX_LENGTH32 4096
static PCRE2_UCHAR32 regtest_buf32[REGTEST_MAX_LENGTH32];
static int regtest_offsetmap32[REGTEST_MAX_LENGTH32];

#endif /* SUPPORT_PCRE2_32 */

static int check_ascii(const char *input)
{
	const unsigned char *ptr = (unsigned char *)input;
	while (*ptr) {
		if (*ptr > 127)
			return 0;
		ptr++;
	}
	return 1;
}

#define OVECTOR_SIZE 15

static int regression_tests(void)
{
	struct regression_test_case *current = regression_test_cases;
	int error;
	PCRE2_SIZE err_offs;
	int is_successful;
	int is_ascii;
	int total = 0;
	int successful = 0;
	int successful_row = 0;
	int counter = 0;
	int jit_compile_mode;
	int utf = 0;
	int disabled_options = 0;
	int i;
#ifdef SUPPORT_PCRE2_8
	pcre2_code_8 *re8;
	pcre2_compile_context_8 *ccontext8;
	pcre2_match_data_8 *mdata8_1;
	pcre2_match_data_8 *mdata8_2;
	pcre2_match_context_8 *mcontext8;
	PCRE2_SIZE *ovector8_1 = NULL;
	PCRE2_SIZE *ovector8_2 = NULL;
	int return_value8[2];
#endif
#ifdef SUPPORT_PCRE2_16
	pcre2_code_16 *re16;
	pcre2_compile_context_16 *ccontext16;
	pcre2_match_data_16 *mdata16_1;
	pcre2_match_data_16 *mdata16_2;
	pcre2_match_context_16 *mcontext16;
	PCRE2_SIZE *ovector16_1 = NULL;
	PCRE2_SIZE *ovector16_2 = NULL;
	int return_value16[2];
	int length16;
#endif
#ifdef SUPPORT_PCRE2_32
	pcre2_code_32 *re32;
	pcre2_compile_context_32 *ccontext32;
	pcre2_match_data_32 *mdata32_1;
	pcre2_match_data_32 *mdata32_2;
	pcre2_match_context_32 *mcontext32;
	PCRE2_SIZE *ovector32_1 = NULL;
	PCRE2_SIZE *ovector32_2 = NULL;
	int return_value32[2];
	int length32;
#endif

#if defined SUPPORT_PCRE2_8
	PCRE2_UCHAR8 cpu_info[128];
#elif defined SUPPORT_PCRE2_16
	PCRE2_UCHAR16 cpu_info[128];
#elif defined SUPPORT_PCRE2_32
	PCRE2_UCHAR32 cpu_info[128];
#endif
#if defined SUPPORT_UNICODE && ((defined(SUPPORT_PCRE2_8) + defined(SUPPORT_PCRE2_16) + defined(SUPPORT_PCRE2_32)) >= 2)
	int return_value;
#endif

	/* This test compares the behaviour of interpreter and JIT. Although disabling
	utf or ucp may make tests fail, if the pcre2_match result is the SAME, it is
	still considered successful from pcre2_jit_test point of view. */

#if defined SUPPORT_PCRE2_8
	pcre2_config_8(PCRE2_CONFIG_JITTARGET, &cpu_info);
#elif defined SUPPORT_PCRE2_16
	pcre2_config_16(PCRE2_CONFIG_JITTARGET, &cpu_info);
#elif defined SUPPORT_PCRE2_32
	pcre2_config_32(PCRE2_CONFIG_JITTARGET, &cpu_info);
#endif

	printf("Running JIT regression tests\n");
	printf("  target CPU of SLJIT compiler: ");
	for (i = 0; cpu_info[i]; i++)
		printf("%c", (char)(cpu_info[i]));
	printf("\n");

#if defined SUPPORT_PCRE2_8
	pcre2_config_8(PCRE2_CONFIG_UNICODE, &utf);
#elif defined SUPPORT_PCRE2_16
	pcre2_config_16(PCRE2_CONFIG_UNICODE, &utf);
#elif defined SUPPORT_PCRE2_32
	pcre2_config_32(PCRE2_CONFIG_UNICODE, &utf);
#endif

	if (!utf)
		disabled_options |= PCRE2_UTF;
#ifdef SUPPORT_PCRE2_8
	printf("  in  8 bit mode with UTF-8  %s:\n", utf ? "enabled" : "disabled");
#endif
#ifdef SUPPORT_PCRE2_16
	printf("  in 16 bit mode with UTF-16 %s:\n", utf ? "enabled" : "disabled");
#endif
#ifdef SUPPORT_PCRE2_32
	printf("  in 32 bit mode with UTF-32 %s:\n", utf ? "enabled" : "disabled");
#endif

	while (current->pattern) {
		/* printf("\nPattern: %s :\n", current->pattern); */
		total++;
		is_ascii = 0;
		if (!(current->start_offset & F_PROPERTY))
			is_ascii = check_ascii(current->pattern) && check_ascii(current->input);

		if (current->match_options & PCRE2_PARTIAL_SOFT)
			jit_compile_mode = PCRE2_JIT_PARTIAL_SOFT;
		else if (current->match_options & PCRE2_PARTIAL_HARD)
			jit_compile_mode = PCRE2_JIT_PARTIAL_HARD;
		else
			jit_compile_mode = PCRE2_JIT_COMPLETE;
		error = 0;
#ifdef SUPPORT_PCRE2_8
		re8 = NULL;
		ccontext8 = pcre2_compile_context_create_8(NULL);
		if (ccontext8) {
			if (GET_NEWLINE(current->newline))
				pcre2_set_newline_8(ccontext8, GET_NEWLINE(current->newline));
			if (GET_BSR(current->newline))
				pcre2_set_bsr_8(ccontext8, GET_BSR(current->newline));

			if (!(current->start_offset & F_NO8)) {
				re8 = pcre2_compile_8((PCRE2_SPTR8)current->pattern, PCRE2_ZERO_TERMINATED,
					current->compile_options & ~disabled_options,
					&error, &err_offs, ccontext8);

				if (!re8 && (utf || is_ascii))
					printf("\n8 bit: Cannot compile pattern \"%s\": %d\n", current->pattern, error);
			}
			pcre2_compile_context_free_8(ccontext8);
		}
		else
			printf("\n8 bit: Cannot allocate compile context\n");
#endif
#ifdef SUPPORT_PCRE2_16
		if ((current->compile_options & PCRE2_UTF) || (current->start_offset & F_FORCECONV))
			convert_utf8_to_utf16((PCRE2_SPTR8)current->pattern, regtest_buf16, NULL, REGTEST_MAX_LENGTH16);
		else
			copy_char8_to_char16((PCRE2_SPTR8)current->pattern, regtest_buf16, REGTEST_MAX_LENGTH16);

		re16 = NULL;
		ccontext16 = pcre2_compile_context_create_16(NULL);
		if (ccontext16) {
			if (GET_NEWLINE(current->newline))
				pcre2_set_newline_16(ccontext16, GET_NEWLINE(current->newline));
			if (GET_BSR(current->newline))
				pcre2_set_bsr_16(ccontext16, GET_BSR(current->newline));

			if (!(current->start_offset & F_NO16)) {
				re16 = pcre2_compile_16(regtest_buf16, PCRE2_ZERO_TERMINATED,
					current->compile_options & ~disabled_options,
					&error, &err_offs, ccontext16);

				if (!re16 && (utf || is_ascii))
					printf("\n16 bit: Cannot compile pattern \"%s\": %d\n", current->pattern, error);
			}
			pcre2_compile_context_free_16(ccontext16);
		}
		else
			printf("\n16 bit: Cannot allocate compile context\n");
#endif
#ifdef SUPPORT_PCRE2_32
		if ((current->compile_options & PCRE2_UTF) || (current->start_offset & F_FORCECONV))
			convert_utf8_to_utf32((PCRE2_SPTR8)current->pattern, regtest_buf32, NULL, REGTEST_MAX_LENGTH32);
		else
			copy_char8_to_char32((PCRE2_SPTR8)current->pattern, regtest_buf32, REGTEST_MAX_LENGTH32);

		re32 = NULL;
		ccontext32 = pcre2_compile_context_create_32(NULL);
		if (ccontext32) {
			if (GET_NEWLINE(current->newline))
				pcre2_set_newline_32(ccontext32, GET_NEWLINE(current->newline));
			if (GET_BSR(current->newline))
				pcre2_set_bsr_32(ccontext32, GET_BSR(current->newline));

			if (!(current->start_offset & F_NO32)) {
				re32 = pcre2_compile_32(regtest_buf32, PCRE2_ZERO_TERMINATED,
					current->compile_options & ~disabled_options,
					&error, &err_offs, ccontext32);

				if (!re32 && (utf || is_ascii))
					printf("\n32 bit: Cannot compile pattern \"%s\": %d\n", current->pattern, error);
			}
			pcre2_compile_context_free_32(ccontext32);
		}
		else
			printf("\n32 bit: Cannot allocate compile context\n");
#endif

		counter++;
		if ((counter & 0x3) != 0) {
#ifdef SUPPORT_PCRE2_8
			setstack8(NULL);
#endif
#ifdef SUPPORT_PCRE2_16
			setstack16(NULL);
#endif
#ifdef SUPPORT_PCRE2_32
			setstack32(NULL);
#endif
		}

#ifdef SUPPORT_PCRE2_8
		return_value8[0] = -1000;
		return_value8[1] = -1000;
		mdata8_1 = pcre2_match_data_create_8(OVECTOR_SIZE, NULL);
		mdata8_2 = pcre2_match_data_create_8(OVECTOR_SIZE, NULL);
		mcontext8 = pcre2_match_context_create_8(NULL);
		if (!mdata8_1 || !mdata8_2 || !mcontext8) {
			printf("\n8 bit: Cannot allocate match data\n");
			pcre2_match_data_free_8(mdata8_1);
			pcre2_match_data_free_8(mdata8_2);
			pcre2_match_context_free_8(mcontext8);
			pcre2_code_free_8(re8);
			re8 = NULL;
		} else {
			ovector8_1 = pcre2_get_ovector_pointer_8(mdata8_1);
			ovector8_2 = pcre2_get_ovector_pointer_8(mdata8_2);
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector8_1[i] = -2;
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector8_2[i] = -2;
			pcre2_set_match_limit_8(mcontext8, 10000000);
		}
		if (re8) {
			return_value8[1] = pcre2_match_8(re8, (PCRE2_SPTR8)current->input, strlen(current->input),
				current->start_offset & OFFSET_MASK, current->match_options, mdata8_2, mcontext8);

			if (pcre2_jit_compile_8(re8, jit_compile_mode)) {
				printf("\n8 bit: JIT compiler does not support \"%s\"\n", current->pattern);
			} else if ((counter & 0x1) != 0) {
				setstack8(mcontext8);
				return_value8[0] = pcre2_match_8(re8, (PCRE2_SPTR8)current->input, strlen(current->input),
					current->start_offset & OFFSET_MASK, current->match_options, mdata8_1, mcontext8);
			} else {
				pcre2_jit_stack_assign_8(mcontext8, NULL, getstack8());
				return_value8[0] = pcre2_jit_match_8(re8, (PCRE2_SPTR8)current->input, strlen(current->input),
					current->start_offset & OFFSET_MASK, current->match_options, mdata8_1, mcontext8);
			}
		}
#endif

#ifdef SUPPORT_PCRE2_16
		return_value16[0] = -1000;
		return_value16[1] = -1000;
		mdata16_1 = pcre2_match_data_create_16(OVECTOR_SIZE, NULL);
		mdata16_2 = pcre2_match_data_create_16(OVECTOR_SIZE, NULL);
		mcontext16 = pcre2_match_context_create_16(NULL);
		if (!mdata16_1 || !mdata16_2 || !mcontext16) {
			printf("\n16 bit: Cannot allocate match data\n");
			pcre2_match_data_free_16(mdata16_1);
			pcre2_match_data_free_16(mdata16_2);
			pcre2_match_context_free_16(mcontext16);
			pcre2_code_free_16(re16);
			re16 = NULL;
		} else {
			ovector16_1 = pcre2_get_ovector_pointer_16(mdata16_1);
			ovector16_2 = pcre2_get_ovector_pointer_16(mdata16_2);
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector16_1[i] = -2;
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector16_2[i] = -2;
			pcre2_set_match_limit_16(mcontext16, 10000000);
		}
		if (re16) {
			if ((current->compile_options & PCRE2_UTF) || (current->start_offset & F_FORCECONV))
				length16 = convert_utf8_to_utf16((PCRE2_SPTR8)current->input, regtest_buf16, regtest_offsetmap16, REGTEST_MAX_LENGTH16);
			else
				length16 = copy_char8_to_char16((PCRE2_SPTR8)current->input, regtest_buf16, REGTEST_MAX_LENGTH16);

			return_value16[1] = pcre2_match_16(re16, regtest_buf16, length16,
				current->start_offset & OFFSET_MASK, current->match_options, mdata16_2, mcontext16);

			if (pcre2_jit_compile_16(re16, jit_compile_mode)) {
				printf("\n16 bit: JIT compiler does not support \"%s\"\n", current->pattern);
			} else if ((counter & 0x1) != 0) {
				setstack16(mcontext16);
				return_value16[0] = pcre2_match_16(re16, regtest_buf16, length16,
					current->start_offset & OFFSET_MASK, current->match_options, mdata16_1, mcontext16);
			} else {
				pcre2_jit_stack_assign_16(mcontext16, NULL, getstack16());
				return_value16[0] = pcre2_jit_match_16(re16, regtest_buf16, length16,
					current->start_offset & OFFSET_MASK, current->match_options, mdata16_1, mcontext16);
			}
		}
#endif

#ifdef SUPPORT_PCRE2_32
		return_value32[0] = -1000;
		return_value32[1] = -1000;
		mdata32_1 = pcre2_match_data_create_32(OVECTOR_SIZE, NULL);
		mdata32_2 = pcre2_match_data_create_32(OVECTOR_SIZE, NULL);
		mcontext32 = pcre2_match_context_create_32(NULL);
		if (!mdata32_1 || !mdata32_2 || !mcontext32) {
			printf("\n32 bit: Cannot allocate match data\n");
			pcre2_match_data_free_32(mdata32_1);
			pcre2_match_data_free_32(mdata32_2);
			pcre2_match_context_free_32(mcontext32);
			pcre2_code_free_32(re32);
			re32 = NULL;
		} else {
			ovector32_1 = pcre2_get_ovector_pointer_32(mdata32_1);
			ovector32_2 = pcre2_get_ovector_pointer_32(mdata32_2);
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector32_1[i] = -2;
			for (i = 0; i < OVECTOR_SIZE * 2; ++i)
				ovector32_2[i] = -2;
			pcre2_set_match_limit_32(mcontext32, 10000000);
		}
		if (re32) {
			if ((current->compile_options & PCRE2_UTF) || (current->start_offset & F_FORCECONV))
				length32 = convert_utf8_to_utf32((PCRE2_SPTR8)current->input, regtest_buf32, regtest_offsetmap32, REGTEST_MAX_LENGTH32);
			else
				length32 = copy_char8_to_char32((PCRE2_SPTR8)current->input, regtest_buf32, REGTEST_MAX_LENGTH32);

			return_value32[1] = pcre2_match_32(re32, regtest_buf32, length32,
				current->start_offset & OFFSET_MASK, current->match_options, mdata32_2, mcontext32);

			if (pcre2_jit_compile_32(re32, jit_compile_mode)) {
				printf("\n32 bit: JIT compiler does not support \"%s\"\n", current->pattern);
			} else if ((counter & 0x1) != 0) {
				setstack32(mcontext32);
				return_value32[0] = pcre2_match_32(re32, regtest_buf32, length32,
					current->start_offset & OFFSET_MASK, current->match_options, mdata32_1, mcontext32);
			} else {
				pcre2_jit_stack_assign_32(mcontext32, NULL, getstack32());
				return_value32[0] = pcre2_jit_match_32(re32, regtest_buf32, length32,
					current->start_offset & OFFSET_MASK, current->match_options, mdata32_1, mcontext32);
			}
		}
#endif

		/* printf("[%d-%d-%d|%d-%d|%d-%d|%d-%d]%s",
			return_value8[0], return_value16[0], return_value32[0],
			(int)ovector8_1[0], (int)ovector8_1[1],
			(int)ovector16_1[0], (int)ovector16_1[1],
			(int)ovector32_1[0], (int)ovector32_1[1],
			(current->compile_options & PCRE2_CASELESS) ? "C" : ""); */

		/* If F_DIFF is set, just run the test, but do not compare the results.
		Segfaults can still be captured. */

		is_successful = 1;
		if (!(current->start_offset & F_DIFF)) {
#if defined SUPPORT_UNICODE && ((defined(SUPPORT_PCRE2_8) + defined(SUPPORT_PCRE2_16) + defined(SUPPORT_PCRE2_32)) >= 2)
			if (!(current->start_offset & F_FORCECONV)) {

				/* All results must be the same. */
#ifdef SUPPORT_PCRE2_8
				if ((return_value = return_value8[0]) != return_value8[1]) {
					printf("\n8 bit: Return value differs(J8:%d,I8:%d): [%d] '%s' @ '%s'\n",
						return_value8[0], return_value8[1], total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
#ifdef SUPPORT_PCRE2_16
				if ((return_value = return_value16[0]) != return_value16[1]) {
					printf("\n16 bit: Return value differs(J16:%d,I16:%d): [%d] '%s' @ '%s'\n",
						return_value16[0], return_value16[1], total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
#ifdef SUPPORT_PCRE2_32
				if ((return_value = return_value32[0]) != return_value32[1]) {
					printf("\n32 bit: Return value differs(J32:%d,I32:%d): [%d] '%s' @ '%s'\n",
						return_value32[0], return_value32[1], total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
#if defined SUPPORT_PCRE2_8 && defined SUPPORT_PCRE2_16
				if (return_value8[0] != return_value16[0]) {
					printf("\n8 and 16 bit: Return value differs(J8:%d,J16:%d): [%d] '%s' @ '%s'\n",
						return_value8[0], return_value16[0],
						total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
#if defined SUPPORT_PCRE2_8 && defined SUPPORT_PCRE2_32
				if (return_value8[0] != return_value32[0]) {
					printf("\n8 and 32 bit: Return value differs(J8:%d,J32:%d): [%d] '%s' @ '%s'\n",
						return_value8[0], return_value32[0],
						total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
#if defined SUPPORT_PCRE2_16 && defined SUPPORT_PCRE2_32
				if (return_value16[0] != return_value32[0]) {
					printf("\n16 and 32 bit: Return value differs(J16:%d,J32:%d): [%d] '%s' @ '%s'\n",
						return_value16[0], return_value32[0],
						total, current->pattern, current->input);
					is_successful = 0;
				} else
#endif
				if (return_value >= 0 || return_value == PCRE2_ERROR_PARTIAL) {
					if (return_value == PCRE2_ERROR_PARTIAL) {
						return_value = 2;
					} else {
						return_value *= 2;
					}
#ifdef SUPPORT_PCRE2_8
					return_value8[0] = return_value;
#endif
#ifdef SUPPORT_PCRE2_16
					return_value16[0] = return_value;
#endif
#ifdef SUPPORT_PCRE2_32
					return_value32[0] = return_value;
#endif
					/* Transform back the results. */
					if (current->compile_options & PCRE2_UTF) {
#ifdef SUPPORT_PCRE2_16
						for (i = 0; i < return_value; ++i) {
							if (ovector16_1[i] != PCRE2_UNSET)
								ovector16_1[i] = regtest_offsetmap16[ovector16_1[i]];
							if (ovector16_2[i] != PCRE2_UNSET)
								ovector16_2[i] = regtest_offsetmap16[ovector16_2[i]];
						}
#endif
#ifdef SUPPORT_PCRE2_32
						for (i = 0; i < return_value; ++i) {
							if (ovector32_1[i] != PCRE2_UNSET)
								ovector32_1[i] = regtest_offsetmap32[ovector32_1[i]];
							if (ovector32_2[i] != PCRE2_UNSET)
								ovector32_2[i] = regtest_offsetmap32[ovector32_2[i]];
						}
#endif
					}

					for (i = 0; i < return_value; ++i) {
#if defined SUPPORT_PCRE2_8 && defined SUPPORT_PCRE2_16
						if (ovector8_1[i] != ovector8_2[i] || ovector8_1[i] != ovector16_1[i] || ovector8_1[i] != ovector16_2[i]) {
							printf("\n8 and 16 bit: Ovector[%d] value differs(J8:%d,I8:%d,J16:%d,I16:%d): [%d] '%s' @ '%s' \n",
								i, (int)ovector8_1[i], (int)ovector8_2[i], (int)ovector16_1[i], (int)ovector16_2[i],
								total, current->pattern, current->input);
							is_successful = 0;
						}
#endif
#if defined SUPPORT_PCRE2_8 && defined SUPPORT_PCRE2_32
						if (ovector8_1[i] != ovector8_2[i] || ovector8_1[i] != ovector32_1[i] || ovector8_1[i] != ovector32_2[i]) {
							printf("\n8 and 32 bit: Ovector[%d] value differs(J8:%d,I8:%d,J32:%d,I32:%d): [%d] '%s' @ '%s' \n",
								i, (int)ovector8_1[i], (int)ovector8_2[i], (int)ovector32_1[i], (int)ovector32_2[i],
								total, current->pattern, current->input);
							is_successful = 0;
						}
#endif
#if defined SUPPORT_PCRE2_16 && defined SUPPORT_PCRE2_32
						if (ovector16_1[i] != ovector16_2[i] || ovector16_1[i] != ovector32_1[i] || ovector16_1[i] != ovector32_2[i]) {
							printf("\n16 and 32 bit: Ovector[%d] value differs(J16:%d,I16:%d,J32:%d,I32:%d): [%d] '%s' @ '%s' \n",
								i, (int)ovector16_1[i], (int)ovector16_2[i], (int)ovector32_1[i], (int)ovector32_2[i],
								total, current->pattern, current->input);
							is_successful = 0;
						}
#endif
					}
				}
			} else
#endif /* more than one of SUPPORT_PCRE2_8, SUPPORT_PCRE2_16 and SUPPORT_PCRE2_32 */
			{
#ifdef SUPPORT_PCRE2_8
				if (return_value8[0] != return_value8[1]) {
					printf("\n8 bit: Return value differs(%d:%d): [%d] '%s' @ '%s'\n",
						return_value8[0], return_value8[1], total, current->pattern, current->input);
					is_successful = 0;
				} else if (return_value8[0] >= 0 || return_value8[0] == PCRE2_ERROR_PARTIAL) {
					if (return_value8[0] == PCRE2_ERROR_PARTIAL)
						return_value8[0] = 2;
					else
						return_value8[0] *= 2;

					for (i = 0; i < return_value8[0]; ++i)
						if (ovector8_1[i] != ovector8_2[i]) {
							printf("\n8 bit: Ovector[%d] value differs(%d:%d): [%d] '%s' @ '%s'\n",
								i, (int)ovector8_1[i], (int)ovector8_2[i], total, current->pattern, current->input);
							is_successful = 0;
						}
				}
#endif

#ifdef SUPPORT_PCRE2_16
				if (return_value16[0] != return_value16[1]) {
					printf("\n16 bit: Return value differs(%d:%d): [%d] '%s' @ '%s'\n",
						return_value16[0], return_value16[1], total, current->pattern, current->input);
					is_successful = 0;
				} else if (return_value16[0] >= 0 || return_value16[0] == PCRE2_ERROR_PARTIAL) {
					if (return_value16[0] == PCRE2_ERROR_PARTIAL)
						return_value16[0] = 2;
					else
						return_value16[0] *= 2;

					for (i = 0; i < return_value16[0]; ++i)
						if (ovector16_1[i] != ovector16_2[i]) {
							printf("\n16 bit: Ovector[%d] value differs(%d:%d): [%d] '%s' @ '%s'\n",
								i, (int)ovector16_1[i], (int)ovector16_2[i], total, current->pattern, current->input);
							is_successful = 0;
						}
				}
#endif

#ifdef SUPPORT_PCRE2_32
				if (return_value32[0] != return_value32[1]) {
					printf("\n32 bit: Return value differs(%d:%d): [%d] '%s' @ '%s'\n",
						return_value32[0], return_value32[1], total, current->pattern, current->input);
					is_successful = 0;
				} else if (return_value32[0] >= 0 || return_value32[0] == PCRE2_ERROR_PARTIAL) {
					if (return_value32[0] == PCRE2_ERROR_PARTIAL)
						return_value32[0] = 2;
					else
						return_value32[0] *= 2;

					for (i = 0; i < return_value32[0]; ++i)
						if (ovector32_1[i] != ovector32_2[i]) {
							printf("\n32 bit: Ovector[%d] value differs(%d:%d): [%d] '%s' @ '%s'\n",
								i, (int)ovector32_1[i], (int)ovector32_2[i], total, current->pattern, current->input);
							is_successful = 0;
						}
				}
#endif
			}
		}

		if (is_successful) {
#ifdef SUPPORT_PCRE2_8
			if (!(current->start_offset & F_NO8) && (utf || is_ascii)) {
				if (return_value8[0] < 0 && !(current->start_offset & F_NOMATCH)) {
					printf("8 bit: Test should match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}

				if (return_value8[0] >= 0 && (current->start_offset & F_NOMATCH)) {
					printf("8 bit: Test should not match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}
			}
#endif
#ifdef SUPPORT_PCRE2_16
			if (!(current->start_offset & F_NO16) && (utf || is_ascii)) {
				if (return_value16[0] < 0 && !(current->start_offset & F_NOMATCH)) {
					printf("16 bit: Test should match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}

				if (return_value16[0] >= 0 && (current->start_offset & F_NOMATCH)) {
					printf("16 bit: Test should not match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}
			}
#endif
#ifdef SUPPORT_PCRE2_32
			if (!(current->start_offset & F_NO32) && (utf || is_ascii)) {
				if (return_value32[0] < 0 && !(current->start_offset & F_NOMATCH)) {
					printf("32 bit: Test should match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}

				if (return_value32[0] >= 0 && (current->start_offset & F_NOMATCH)) {
					printf("32 bit: Test should not match: [%d] '%s' @ '%s'\n",
						total, current->pattern, current->input);
					is_successful = 0;
				}
			}
#endif
		}

		if (is_successful) {
#ifdef SUPPORT_PCRE2_8
			if (re8 && !(current->start_offset & F_NO8) && pcre2_get_mark_8(mdata8_1) != pcre2_get_mark_8(mdata8_2)) {
				printf("8 bit: Mark value mismatch: [%d] '%s' @ '%s'\n",
					total, current->pattern, current->input);
				is_successful = 0;
			}
#endif
#ifdef SUPPORT_PCRE2_16
			if (re16 && !(current->start_offset & F_NO16) && pcre2_get_mark_16(mdata16_1) != pcre2_get_mark_16(mdata16_2)) {
				printf("16 bit: Mark value mismatch: [%d] '%s' @ '%s'\n",
					total, current->pattern, current->input);
				is_successful = 0;
			}
#endif
#ifdef SUPPORT_PCRE2_32
			if (re32 && !(current->start_offset & F_NO32) && pcre2_get_mark_32(mdata32_1) != pcre2_get_mark_32(mdata32_2)) {
				printf("32 bit: Mark value mismatch: [%d] '%s' @ '%s'\n",
					total, current->pattern, current->input);
				is_successful = 0;
			}
#endif
		}

#ifdef SUPPORT_PCRE2_8
		pcre2_code_free_8(re8);
		pcre2_match_data_free_8(mdata8_1);
		pcre2_match_data_free_8(mdata8_2);
		pcre2_match_context_free_8(mcontext8);
#endif
#ifdef SUPPORT_PCRE2_16
		pcre2_code_free_16(re16);
		pcre2_match_data_free_16(mdata16_1);
		pcre2_match_data_free_16(mdata16_2);
		pcre2_match_context_free_16(mcontext16);
#endif
#ifdef SUPPORT_PCRE2_32
		pcre2_code_free_32(re32);
		pcre2_match_data_free_32(mdata32_1);
		pcre2_match_data_free_32(mdata32_2);
		pcre2_match_context_free_32(mcontext32);
#endif

		if (is_successful) {
			successful++;
			successful_row++;
			printf(".");
			if (successful_row >= 60) {
				successful_row = 0;
				printf("\n");
			}
		} else
			successful_row = 0;

		fflush(stdout);
		current++;
	}
#ifdef SUPPORT_PCRE2_8
	setstack8(NULL);
#endif
#ifdef SUPPORT_PCRE2_16
	setstack16(NULL);
#endif
#ifdef SUPPORT_PCRE2_32
	setstack32(NULL);
#endif

	if (total == successful) {
		printf("\nAll JIT regression tests are successfully passed.\n");
		return 0;
	} else {
		printf("\nSuccessful test ratio: %d%% (%d failed)\n", successful * 100 / total, total - successful);
		return 1;
	}
}

#if defined SUPPORT_UNICODE

static int check_invalid_utf_result(int pattern_index, const char *type, int result,
	int match_start, int match_end, PCRE2_SIZE *ovector)
{
	if (match_start < 0) {
		if (result != -1) {
			printf("Pattern[%d] %s result is not -1.\n", pattern_index, type);
			return 1;
		}
		return 0;
	}

	if (result <= 0) {
		printf("Pattern[%d] %s result (%d) is not greater than 0.\n", pattern_index, type, result);
		return 1;
	}

	if (ovector[0] != (PCRE2_SIZE)match_start) {
		printf("Pattern[%d] %s ovector[0] is unexpected (%d instead of %d)\n",
			pattern_index, type, (int)ovector[0], match_start);
		return 1;
	}

	if (ovector[1] != (PCRE2_SIZE)match_end) {
		printf("Pattern[%d] %s ovector[1] is unexpected (%d instead of %d)\n",
			pattern_index, type, (int)ovector[1], match_end);
		return 1;
	}

	return 0;
}

#endif /* SUPPORT_UNICODE */

#if defined SUPPORT_UNICODE && defined SUPPORT_PCRE2_8

#define UDA (PCRE2_UTF | PCRE2_DOTALL | PCRE2_ANCHORED)
#define CI (PCRE2_JIT_COMPLETE | PCRE2_JIT_INVALID_UTF)
#define CPI (PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT | PCRE2_JIT_INVALID_UTF)

struct invalid_utf8_regression_test_case {
	int compile_options;
	int jit_compile_options;
	int start_offset;
	int skip_left;
	int skip_right;
	int match_start;
	int match_end;
	const char *pattern[2];
	const char *input;
};

static const char invalid_utf8_newline_cr;

static const struct invalid_utf8_regression_test_case invalid_utf8_regression_test_cases[] = {
	{ UDA, CI, 0, 0, 0, 0, 4, { ".", NULL }, "\xf4\x8f\xbf\xbf" },
	{ UDA, CI, 0, 0, 0, 0, 4, { ".", NULL }, "\xf0\x90\x80\x80" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf4\x90\x80\x80" },
	{ UDA, CI, 0, 0, 1, -1, -1, { ".", NULL }, "\xf4\x8f\xbf\xbf" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf0\x90\x80\x7f" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf0\x90\x80\xc0" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf0\x8f\xbf\xbf" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xef\xbf\xbf#" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xef\xbf\xbf" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xe0\xa0\x80#" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xe0\xa0\x80" },
	{ UDA, CI, 0, 0, 2, -1, -1, { ".", NULL }, "\xef\xbf\xbf#" },
	{ UDA, CI, 0, 0, 1, -1, -1, { ".", NULL }, "\xef\xbf\xbf" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xef\xbf\x7f#" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xef\xbf\xc0" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xe0\x9f\xbf#" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xe0\x9f\xbf" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xed\x9f\xbf#" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xed\xa0\x80#" },
	{ UDA, CI, 0, 0, 0, 0, 3, { ".", NULL }, "\xee\x80\x80#" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xed\xbf\xbf#" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xdf\xbf##" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xdf\xbf#" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xdf\xbf" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xc2\x80##" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xc2\x80#" },
	{ UDA, CI, 0, 0, 0, 0, 2, { ".", NULL }, "\xc2\x80" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xe0\x80##" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xdf\xc0##" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xe0\x80" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xdf\xc0" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xc1\xbf##" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xc1\xbf" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\x80###" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\x80" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf8###" },
	{ UDA, CI, 0, 0, 0, -1, -1, { ".", NULL }, "\xf8" },
	{ UDA, CI, 0, 0, 0, 0, 1, { ".", NULL }, "\x7f" },

	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "\xf4\x8f\xbf\xbf#" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf4\xa0\x80\x80\xf4\xa0\x80\x80" },
	{ UDA, CPI, 4, 1, 1, -1, -1, { "\\B", "\\b" }, "\xf4\x8f\xbf\xbf\xf4\x8f\xbf\xbf" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "#\xef\xbf\xbf#" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "#\xe0\xa0\x80#" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "\xf0\x90\x80\x80#" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "\xf3\xbf\xbf\xbf#" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf0\x8f\xbf\xbf\xf0\x8f\xbf\xbf" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf5\x80\x80\x80\xf5\x80\x80\x80" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf4\x90\x80\x80\xf4\x90\x80\x80" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf4\x8f\xbf\xff\xf4\x8f\xbf\xff" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\xf4\x8f\xff\xbf\xf4\x8f\xff\xbf" },
	{ UDA, CPI, 4, 0, 1, -1, -1, { "\\B", "\\b" }, "\xef\x80\x80\x80\xef\x80\x80" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "\x80\x80\x80\x80\x80\x80\x80\x80" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "#\xe0\x9f\xbf\xe0\x9f\xbf#" },
	{ UDA, CPI, 4, 2, 2, -1, -1, { "\\B", "\\b" }, "#\xe0\xa0\x80\xe0\xa0\x80#" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "#\xf0\x80\x80\xf0\x80\x80#" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "#\xed\xa0\x80\xed\xa0\x80#" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "##\xdf\xbf#" },
	{ UDA, CPI, 4, 2, 0, 2, 2, { "\\B", NULL }, "##\xdf\xbf#" },
	{ UDA, CPI, 4, 0, 0, 4, 4, { "\\B", NULL }, "##\xc2\x80#" },
	{ UDA, CPI, 4, 2, 0, 2, 2, { "\\B", NULL }, "##\xc2\x80#" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "##\xc1\xbf\xc1\xbf##" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "##\xdf\xc0\xdf\xc0##" },
	{ UDA, CPI, 4, 0, 0, -1, -1, { "\\B", "\\b" }, "##\xe0\x80\xe0\x80##" },

	{ UDA, CPI, 3, 0, 0, 3, 3, { "\\B", NULL }, "\xef\xbf\xbf#" },
	{ UDA, CPI, 3, 0, 0, 3, 3, { "\\B", NULL }, "\xe0\xa0\x80#" },
	{ UDA, CPI, 3, 0, 0, -1, -1, { "\\B", "\\b" }, "\xe0\x9f\xbf\xe0\x9f\xbf" },
	{ UDA, CPI, 3, 1, 1, -1, -1, { "\\B", "\\b" }, "\xef\xbf\xbf\xef\xbf\xbf" },
	{ UDA, CPI, 3, 0, 1, -1, -1, { "\\B", "\\b" }, "\xdf\x80\x80\xdf\x80" },
	{ UDA, CPI, 3, 0, 0, -1, -1, { "\\B", "\\b" }, "\xef\xbf\xff\xef\xbf\xff" },
	{ UDA, CPI, 3, 0, 0, -1, -1, { "\\B", "\\b" }, "\xef\xff\xbf\xef\xff\xbf" },
	{ UDA, CPI, 3, 0, 0, -1, -1, { "\\B", "\\b" }, "\xed\xbf\xbf\xed\xbf\xbf" },

	{ UDA, CPI, 2, 0, 0, 2, 2, { "\\B", NULL }, "\xdf\xbf#" },
	{ UDA, CPI, 2, 0, 0, 2, 2, { "\\B", NULL }, "\xc2\x80#" },
	{ UDA, CPI, 2, 1, 1, -1, -1, { "\\B", "\\b" }, "\xdf\xbf\xdf\xbf" },
	{ UDA, CPI, 2, 0, 0, -1, -1, { "\\B", "\\b" }, "\xc1\xbf\xc1\xbf" },
	{ UDA, CPI, 2, 0, 0, -1, -1, { "\\B", "\\b" }, "\xe0\x80\xe0\x80" },
	{ UDA, CPI, 2, 0, 0, -1, -1, { "\\B", "\\b" }, "\xdf\xff\xdf\xff" },
	{ UDA, CPI, 2, 0, 0, -1, -1, { "\\B", "\\b" }, "\xff\xbf\xff\xbf" },

	{ UDA, CPI, 1, 0, 0, 1, 1, { "\\B", NULL }, "\x7f#" },
	{ UDA, CPI, 1, 0, 0, 1, 1, { "\\B", NULL }, "\x01#" },
	{ UDA, CPI, 1, 0, 0, -1, -1, { "\\B", "\\b" }, "\x80\x80" },
	{ UDA, CPI, 1, 0, 0, -1, -1, { "\\B", "\\b" }, "\xb0\xb0" },

	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 2, { "(.)\\1", NULL }, "aA" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, -1, -1, { "(.)\\1", NULL }, "a\xff" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 4, { "(.)\\1", NULL }, "\xc3\xa1\xc3\x81" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 1, -1, -1, { "(.)\\1", NULL }, "\xc3\xa1\xc3\x81" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, -1, -1, { "(.)\\1", NULL }, "\xc2\x80\x80" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 6, { "(.)\\1", NULL }, "\xe1\xbd\xb8\xe1\xbf\xb8" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 1, -1, -1, { "(.)\\1", NULL }, "\xe1\xbd\xb8\xe1\xbf\xb8" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 8, { "(.)\\1", NULL }, "\xf0\x90\x90\x80\xf0\x90\x90\xa8" },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 1, -1, -1, { "(.)\\1", NULL }, "\xf0\x90\x90\x80\xf0\x90\x90\xa8" },

	{ UDA, CPI, 0, 0, 0, 0, 1, { "\\X", NULL }, "A" },
	{ UDA, CPI, 0, 0, 0, -1, -1, { "\\X", NULL }, "\xff" },
	{ UDA, CPI, 0, 0, 0, 0, 2, { "\\X", NULL }, "\xc3\xa1" },
	{ UDA, CPI, 0, 0, 1, -1, -1, { "\\X", NULL }, "\xc3\xa1" },
	{ UDA, CPI, 0, 0, 0, -1, -1, { "\\X", NULL }, "\xc3\x7f" },
	{ UDA, CPI, 0, 0, 0, 0, 3, { "\\X", NULL }, "\xe1\xbd\xb8" },
	{ UDA, CPI, 0, 0, 1, -1, -1, { "\\X", NULL }, "\xe1\xbd\xb8" },
	{ UDA, CPI, 0, 0, 0, 0, 4, { "\\X", NULL }, "\xf0\x90\x90\x80" },
	{ UDA, CPI, 0, 0, 1, -1, -1, { "\\X", NULL }, "\xf0\x90\x90\x80" },

	{ UDA, CPI, 0, 0, 0, -1, -1, { "[^#]", NULL }, "#" },
	{ UDA, CPI, 0, 0, 0, 0, 4, { "[^#]", NULL }, "\xf4\x8f\xbf\xbf" },
	{ UDA, CPI, 0, 0, 0, -1, -1, { "[^#]", NULL }, "\xf4\x90\x80\x80" },
	{ UDA, CPI, 0, 0, 0, -1, -1, { "[^#]", NULL }, "\xc1\x80" },

	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 2, 3, { "^\\W", NULL }, " \x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 14, 15, { "^\\W", NULL }, " \xc0\x8a#\xe0\x80\x8a#\xf0\x80\x80\x8a#\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 3, 4, { "^\\W", NULL }, " \xf8\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 3, 4, { "^\\W", NULL }, " \xc3\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 3, 4, { "^\\W", NULL }, " \xf1\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 4, 5, { "^\\W", NULL }, " \xf2\xbf\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 5, 6, { "^\\W", NULL }, " \xf2\xbf\xbf\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 3, 4, { "^\\W", NULL }, " \xef\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 4, 5, { "^\\W", NULL }, " \xef\xbf\x0a#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 5, 6, { "^\\W", NULL }, " \x85#\xc2\x85#"},
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 7, 8, { "^\\W", NULL }, " \xe2\x80\xf8\xe2\x80\xa8#"},

	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, -1, -1, { "#", NULL }, "\xe2\x80\xf8\xe2\x80\xa8#"},
	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, 3, 4, { "#", NULL }, "\xe2\x80\xf8#\xe2\x80\xa8#"},
	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, -1, -1, { "#", NULL }, "abcd\xc2\x85#"},
	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, 1, 2, { "#", NULL }, "\x85#\xc2\x85#"},
	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, 5, 6, { "#", NULL }, "\xef,\x80,\xf8#\x0a"},
	{ PCRE2_UTF | PCRE2_FIRSTLINE, CI, 0, 0, 0, -1, -1, { "#", NULL }, "\xef,\x80,\xf8\x0a#"},

	{ PCRE2_UTF | PCRE2_NO_START_OPTIMIZE, CI, 0, 0, 0, 4, 8, { "#\xc7\x85#", NULL }, "\x80\x80#\xc7#\xc7\x85#" },
	{ PCRE2_UTF | PCRE2_NO_START_OPTIMIZE, CI, 0, 0, 0, 7, 11, { "#\xc7\x85#", NULL }, "\x80\x80#\xc7\x80\x80\x80#\xc7\x85#" },
	{ PCRE2_UTF, CI, 0, 0, 0, 4, 8, { "#\xc7\x85#", NULL }, "\x80\x80#\xc7#\xc7\x85#" },
	{ PCRE2_UTF, CI, 0, 0, 0, 7, 11, { "#\xc7\x85#", NULL }, "\x80\x80#\xc7\x80\x80\x80#\xc7\x85#" },

	{ PCRE2_UTF | PCRE2_UCP, CI, 0, 0, 0, -1, -1, { "[\\s]", NULL }, "\xed\xa0\x80" },

	/* These two are not invalid UTF tests, but this infrastructure fits better for them. */
	{ 0, PCRE2_JIT_COMPLETE, 0, 0, 1, -1, -1, { "\\X{2}", NULL }, "\r\n\n" },
	{ 0, PCRE2_JIT_COMPLETE, 0, 0, 1, -1, -1, { "\\R{2}", NULL }, "\r\n\n" },

	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 0, 0, 0, -1, -1, { "^.a", &invalid_utf8_newline_cr }, "\xc3\xa7#a" },

	{ 0, 0, 0, 0, 0, 0, 0, { NULL, NULL }, NULL }
};

#undef UDA
#undef CI
#undef CPI

static int run_invalid_utf8_test(const struct invalid_utf8_regression_test_case *current,
	int pattern_index, int i, pcre2_compile_context_8 *ccontext, pcre2_match_data_8 *mdata)
{
	pcre2_code_8 *code;
	int result, errorcode;
	PCRE2_SIZE length, erroroffset;
	PCRE2_SIZE *ovector = pcre2_get_ovector_pointer_8(mdata);

	if (current->pattern[i] == NULL)
		return 1;

	code = pcre2_compile_8((PCRE2_UCHAR8*)current->pattern[i], PCRE2_ZERO_TERMINATED,
		current->compile_options, &errorcode, &erroroffset, ccontext);

	if (!code) {
		printf("Pattern[%d:0] cannot be compiled. Error offset: %d\n", pattern_index, (int)erroroffset);
		return 0;
	}

	if (pcre2_jit_compile_8(code, current->jit_compile_options) != 0) {
		printf("Pattern[%d:0] cannot be compiled by the JIT compiler.\n", pattern_index);
		pcre2_code_free_8(code);
		return 0;
	}

	length = (PCRE2_SIZE)(strlen(current->input) - current->skip_left - current->skip_right);

	if (current->jit_compile_options & PCRE2_JIT_COMPLETE) {
		result = pcre2_jit_match_8(code, (PCRE2_UCHAR8*)(current->input + current->skip_left),
			length, current->start_offset - current->skip_left, 0, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_8(code);
			return 0;
		}
	}

	if (current->jit_compile_options & PCRE2_JIT_PARTIAL_SOFT) {
		result = pcre2_jit_match_8(code, (PCRE2_UCHAR8*)(current->input + current->skip_left),
			length, current->start_offset - current->skip_left, PCRE2_PARTIAL_SOFT, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "partial match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_8(code);
			return 0;
		}
	}

	pcre2_code_free_8(code);
	return 1;
}

static int invalid_utf8_regression_tests(void)
{
	const struct invalid_utf8_regression_test_case *current;
	pcre2_compile_context_8 *ccontext;
	pcre2_match_data_8 *mdata;
	int total = 0, successful = 0;
	int result;

	printf("\nRunning invalid-utf8 JIT regression tests\n");

	ccontext = pcre2_compile_context_create_8(NULL);
	pcre2_set_newline_8(ccontext, PCRE2_NEWLINE_ANY);
	mdata = pcre2_match_data_create_8(4, NULL);

	for (current = invalid_utf8_regression_test_cases; current->pattern[0]; current++) {
		/* printf("\nPattern: %s :\n", current->pattern); */
		total++;

		result = 1;
		if (current->pattern[1] != &invalid_utf8_newline_cr)
		{
			if (!run_invalid_utf8_test(current, total - 1, 0, ccontext, mdata))
				result = 0;
			if (!run_invalid_utf8_test(current, total - 1, 1, ccontext, mdata))
				result = 0;
		} else {
			pcre2_set_newline_8(ccontext, PCRE2_NEWLINE_CR);
			if (!run_invalid_utf8_test(current, total - 1, 0, ccontext, mdata))
				result = 0;
			pcre2_set_newline_8(ccontext, PCRE2_NEWLINE_ANY);
		}

		if (result) {
			successful++;
		}

		printf(".");
		if ((total % 60) == 0)
			printf("\n");
	}

	if ((total % 60) != 0)
		printf("\n");

	pcre2_match_data_free_8(mdata);
	pcre2_compile_context_free_8(ccontext);

	if (total == successful) {
		printf("\nAll invalid UTF8 JIT regression tests are successfully passed.\n");
		return 0;
	} else {
		printf("\nInvalid UTF8 successful test ratio: %d%% (%d failed)\n", successful * 100 / total, total - successful);
		return 1;
	}
}

#else /* !SUPPORT_UNICODE || !SUPPORT_PCRE2_8 */

static int invalid_utf8_regression_tests(void)
{
	return 0;
}

#endif /* SUPPORT_UNICODE && SUPPORT_PCRE2_8 */

#if defined SUPPORT_UNICODE && defined SUPPORT_PCRE2_16

#define UDA (PCRE2_UTF | PCRE2_DOTALL | PCRE2_ANCHORED)
#define CI (PCRE2_JIT_COMPLETE | PCRE2_JIT_INVALID_UTF)
#define CPI (PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT | PCRE2_JIT_INVALID_UTF)

struct invalid_utf16_regression_test_case {
	int compile_options;
	int jit_compile_options;
	int start_offset;
	int skip_left;
	int skip_right;
	int match_start;
	int match_end;
	const PCRE2_UCHAR16 *pattern[2];
	const PCRE2_UCHAR16 *input;
};

static PCRE2_UCHAR16 allany16[] = { '.', 0 };
static PCRE2_UCHAR16 non_word_boundary16[] = { '\\', 'B', 0 };
static PCRE2_UCHAR16 word_boundary16[] = { '\\', 'b', 0 };
static PCRE2_UCHAR16 backreference16[] = { '(', '.', ')', '\\', '1', 0 };
static PCRE2_UCHAR16 grapheme16[] = { '\\', 'X', 0 };
static PCRE2_UCHAR16 nothashmark16[] = { '[', '^', '#', ']', 0 };
static PCRE2_UCHAR16 afternl16[] = { '^', '\\', 'W', 0 };
static PCRE2_UCHAR16 generic16[] = { '#', 0xd800, 0xdc00, '#', 0 };
static PCRE2_UCHAR16 test16_1[] = { 0xd7ff, 0xe000, 0xffff, 0x01, '#', 0 };
static PCRE2_UCHAR16 test16_2[] = { 0xd800, 0xdc00, 0xd800, 0xdc00, 0 };
static PCRE2_UCHAR16 test16_3[] = { 0xdbff, 0xdfff, 0xdbff, 0xdfff, 0 };
static PCRE2_UCHAR16 test16_4[] = { 0xd800, 0xdbff, 0xd800, 0xdbff, 0 };
static PCRE2_UCHAR16 test16_5[] = { '#', 0xd800, 0xdc00, '#', 0 };
static PCRE2_UCHAR16 test16_6[] = { 'a', 'A', 0xdc28, 0 };
static PCRE2_UCHAR16 test16_7[] = { 0xd801, 0xdc00, 0xd801, 0xdc28, 0 };
static PCRE2_UCHAR16 test16_8[] = { '#', 0xd800, 0xdc00, 0 };
static PCRE2_UCHAR16 test16_9[] = { ' ', 0x2028, '#', 0 };
static PCRE2_UCHAR16 test16_10[] = { ' ', 0xdc00, 0xd800, 0x2028, '#', 0 };
static PCRE2_UCHAR16 test16_11[] = { 0xdc00, 0xdc00, 0xd800, 0xdc00, 0xdc00, '#', 0xd800, 0xdc00, '#', 0 };
static PCRE2_UCHAR16 test16_12[] = { '#', 0xd800, 0xdc00, 0xd800, '#', 0xd800, 0xdc00, 0xdc00, 0xdc00, '#', 0xd800, 0xdc00, '#', 0 };

static const struct invalid_utf16_regression_test_case invalid_utf16_regression_test_cases[] = {
	{ UDA, CI, 0, 0, 0, 0, 1, { allany16, NULL }, test16_1 },
	{ UDA, CI, 1, 0, 0, 1, 2, { allany16, NULL }, test16_1 },
	{ UDA, CI, 2, 0, 0, 2, 3, { allany16, NULL }, test16_1 },
	{ UDA, CI, 3, 0, 0, 3, 4, { allany16, NULL }, test16_1 },
	{ UDA, CI, 0, 0, 0, 0, 2, { allany16, NULL }, test16_2 },
	{ UDA, CI, 0, 0, 3, -1, -1, { allany16, NULL }, test16_2 },
	{ UDA, CI, 1, 0, 0, -1, -1, { allany16, NULL }, test16_2 },
	{ UDA, CI, 0, 0, 0, 0, 2, { allany16, NULL }, test16_3 },
	{ UDA, CI, 0, 0, 3, -1, -1, { allany16, NULL }, test16_3 },
	{ UDA, CI, 1, 0, 0, -1, -1, { allany16, NULL }, test16_3 },

	{ UDA, CPI, 1, 0, 0, 1, 1, { non_word_boundary16, NULL }, test16_1 },
	{ UDA, CPI, 2, 0, 0, 2, 2, { non_word_boundary16, NULL }, test16_1 },
	{ UDA, CPI, 3, 0, 0, 3, 3, { non_word_boundary16, NULL }, test16_1 },
	{ UDA, CPI, 4, 0, 0, 4, 4, { non_word_boundary16, NULL }, test16_1 },
	{ UDA, CPI, 2, 0, 0, 2, 2, { non_word_boundary16, NULL }, test16_2 },
	{ UDA, CPI, 2, 0, 0, 2, 2, { non_word_boundary16, NULL }, test16_3 },
	{ UDA, CPI, 2, 1, 1, -1, -1, { non_word_boundary16, word_boundary16 }, test16_2 },
	{ UDA, CPI, 2, 1, 1, -1, -1, { non_word_boundary16, word_boundary16 }, test16_3 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { non_word_boundary16, word_boundary16 }, test16_4 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { non_word_boundary16, word_boundary16 }, test16_5 },

	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 2, { backreference16, NULL }, test16_6 },
	{ UDA | PCRE2_CASELESS, CPI, 1, 0, 0, -1, -1, { backreference16, NULL }, test16_6 },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 4, { backreference16, NULL }, test16_7 },
	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 1, -1, -1, { backreference16, NULL }, test16_7 },

	{ UDA, CPI, 0, 0, 0, 0, 1, { grapheme16, NULL }, test16_6 },
	{ UDA, CPI, 1, 0, 0, 1, 2, { grapheme16, NULL }, test16_6 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { grapheme16, NULL }, test16_6 },
	{ UDA, CPI, 0, 0, 0, 0, 2, { grapheme16, NULL }, test16_7 },
	{ UDA, CPI, 2, 0, 0, 2, 4, { grapheme16, NULL }, test16_7 },
	{ UDA, CPI, 1, 0, 0, -1, -1, { grapheme16, NULL }, test16_7 },

	{ UDA, CPI, 0, 0, 0, -1, -1, { nothashmark16, NULL }, test16_8 },
	{ UDA, CPI, 1, 0, 0, 1, 3, { nothashmark16, NULL }, test16_8 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { nothashmark16, NULL }, test16_8 },

	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 2, 3, { afternl16, NULL }, test16_9 },
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 4, 5, { afternl16, NULL }, test16_10 },

	{ PCRE2_UTF | PCRE2_NO_START_OPTIMIZE, CI, 0, 0, 0, 5, 9, { generic16, NULL }, test16_11 },
	{ PCRE2_UTF | PCRE2_NO_START_OPTIMIZE, CI, 0, 0, 0, 9, 13, { generic16, NULL }, test16_12 },
	{ PCRE2_UTF, CI, 0, 0, 0, 5, 9, { generic16, NULL }, test16_11 },
	{ PCRE2_UTF, CI, 0, 0, 0, 9, 13, { generic16, NULL }, test16_12 },

	{ 0, 0, 0, 0, 0, 0, 0, { NULL, NULL }, NULL }
};

#undef UDA
#undef CI
#undef CPI

static int run_invalid_utf16_test(const struct invalid_utf16_regression_test_case *current,
	int pattern_index, int i, pcre2_compile_context_16 *ccontext, pcre2_match_data_16 *mdata)
{
	pcre2_code_16 *code;
	int result, errorcode;
	PCRE2_SIZE length, erroroffset;
	const PCRE2_UCHAR16 *input;
	PCRE2_SIZE *ovector = pcre2_get_ovector_pointer_16(mdata);

	if (current->pattern[i] == NULL)
		return 1;

	code = pcre2_compile_16(current->pattern[i], PCRE2_ZERO_TERMINATED,
		current->compile_options, &errorcode, &erroroffset, ccontext);

	if (!code) {
		printf("Pattern[%d:0] cannot be compiled. Error offset: %d\n", pattern_index, (int)erroroffset);
		return 0;
	}

	if (pcre2_jit_compile_16(code, current->jit_compile_options) != 0) {
		printf("Pattern[%d:0] cannot be compiled by the JIT compiler.\n", pattern_index);
		pcre2_code_free_16(code);
		return 0;
	}

	input = current->input;
	length = 0;

	while (*input++ != 0)
		length++;

	length -= current->skip_left + current->skip_right;

	if (current->jit_compile_options & PCRE2_JIT_COMPLETE) {
		result = pcre2_jit_match_16(code, (current->input + current->skip_left),
			length, current->start_offset - current->skip_left, 0, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_16(code);
			return 0;
		}
	}

	if (current->jit_compile_options & PCRE2_JIT_PARTIAL_SOFT) {
		result = pcre2_jit_match_16(code, (current->input + current->skip_left),
			length, current->start_offset - current->skip_left, PCRE2_PARTIAL_SOFT, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "partial match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_16(code);
			return 0;
		}
	}

	pcre2_code_free_16(code);
	return 1;
}

static int invalid_utf16_regression_tests(void)
{
	const struct invalid_utf16_regression_test_case *current;
	pcre2_compile_context_16 *ccontext;
	pcre2_match_data_16 *mdata;
	int total = 0, successful = 0;
	int result;

	printf("\nRunning invalid-utf16 JIT regression tests\n");

	ccontext = pcre2_compile_context_create_16(NULL);
	pcre2_set_newline_16(ccontext, PCRE2_NEWLINE_ANY);
	mdata = pcre2_match_data_create_16(4, NULL);

	for (current = invalid_utf16_regression_test_cases; current->pattern[0]; current++) {
		/* printf("\nPattern: %s :\n", current->pattern); */
		total++;

		result = 1;
		if (!run_invalid_utf16_test(current, total - 1, 0, ccontext, mdata))
			result = 0;
		if (!run_invalid_utf16_test(current, total - 1, 1, ccontext, mdata))
			result = 0;

		if (result) {
			successful++;
		}

		printf(".");
		if ((total % 60) == 0)
			printf("\n");
	}

	if ((total % 60) != 0)
		printf("\n");

	pcre2_match_data_free_16(mdata);
	pcre2_compile_context_free_16(ccontext);

	if (total == successful) {
		printf("\nAll invalid UTF16 JIT regression tests are successfully passed.\n");
		return 0;
	} else {
		printf("\nInvalid UTF16 successful test ratio: %d%% (%d failed)\n", successful * 100 / total, total - successful);
		return 1;
	}
}

#else /* !SUPPORT_UNICODE || !SUPPORT_PCRE2_16 */

static int invalid_utf16_regression_tests(void)
{
	return 0;
}

#endif /* SUPPORT_UNICODE && SUPPORT_PCRE2_16 */

#if defined SUPPORT_UNICODE && defined SUPPORT_PCRE2_32

#define UDA (PCRE2_UTF | PCRE2_DOTALL | PCRE2_ANCHORED)
#define CI (PCRE2_JIT_COMPLETE | PCRE2_JIT_INVALID_UTF)
#define CPI (PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT | PCRE2_JIT_INVALID_UTF)

struct invalid_utf32_regression_test_case {
	int compile_options;
	int jit_compile_options;
	int start_offset;
	int skip_left;
	int skip_right;
	int match_start;
	int match_end;
	const PCRE2_UCHAR32 *pattern[2];
	const PCRE2_UCHAR32 *input;
};

static PCRE2_UCHAR32 allany32[] = { '.', 0 };
static PCRE2_UCHAR32 non_word_boundary32[] = { '\\', 'B', 0 };
static PCRE2_UCHAR32 word_boundary32[] = { '\\', 'b', 0 };
static PCRE2_UCHAR32 backreference32[] = { '(', '.', ')', '\\', '1', 0 };
static PCRE2_UCHAR32 grapheme32[] = { '\\', 'X', 0 };
static PCRE2_UCHAR32 nothashmark32[] = { '[', '^', '#', ']', 0 };
static PCRE2_UCHAR32 afternl32[] = { '^', '\\', 'W', 0 };
static PCRE2_UCHAR32 test32_1[] = { 0x10ffff, 0x10ffff, 0x110000, 0x110000, 0x10ffff, 0 };
static PCRE2_UCHAR32 test32_2[] = { 0xd7ff, 0xe000, 0xd800, 0xdfff, 0xe000, 0xdfff, 0xd800, 0 };
static PCRE2_UCHAR32 test32_3[] = { 'a', 'A', 0x110000, 0 };
static PCRE2_UCHAR32 test32_4[] = { '#', 0x10ffff, 0x110000, 0 };
static PCRE2_UCHAR32 test32_5[] = { ' ', 0x2028, '#', 0 };
static PCRE2_UCHAR32 test32_6[] = { ' ', 0x110000, 0x2028, '#', 0 };

static const struct invalid_utf32_regression_test_case invalid_utf32_regression_test_cases[] = {
	{ UDA, CI, 0, 0, 0, 0, 1, { allany32, NULL }, test32_1 },
	{ UDA, CI, 2, 0, 0, -1, -1, { allany32, NULL }, test32_1 },
	{ UDA, CI, 0, 0, 0, 0, 1, { allany32, NULL }, test32_2 },
	{ UDA, CI, 1, 0, 0, 1, 2, { allany32, NULL }, test32_2 },
	{ UDA, CI, 2, 0, 0, -1, -1, { allany32, NULL }, test32_2 },
	{ UDA, CI, 3, 0, 0, -1, -1, { allany32, NULL }, test32_2 },

	{ UDA, CPI, 1, 0, 0, 1, 1, { non_word_boundary32, NULL }, test32_1 },
	{ UDA, CPI, 3, 0, 0, -1, -1, { non_word_boundary32, word_boundary32 }, test32_1 },
	{ UDA, CPI, 1, 0, 0, 1, 1, { non_word_boundary32, NULL }, test32_2 },
	{ UDA, CPI, 3, 0, 0, -1, -1, { non_word_boundary32, word_boundary32 }, test32_2 },
	{ UDA, CPI, 6, 0, 0, -1, -1, { non_word_boundary32, word_boundary32 }, test32_2 },

	{ UDA | PCRE2_CASELESS, CPI, 0, 0, 0, 0, 2, { backreference32, NULL }, test32_3 },
	{ UDA | PCRE2_CASELESS, CPI, 1, 0, 0, -1, -1, { backreference32, NULL }, test32_3 },

	{ UDA, CPI, 0, 0, 0, 0, 1, { grapheme32, NULL }, test32_1 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { grapheme32, NULL }, test32_1 },
	{ UDA, CPI, 1, 0, 0, 1, 2, { grapheme32, NULL }, test32_2 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { grapheme32, NULL }, test32_2 },
	{ UDA, CPI, 3, 0, 0, -1, -1, { grapheme32, NULL }, test32_2 },
	{ UDA, CPI, 4, 0, 0, 4, 5, { grapheme32, NULL }, test32_2 },

	{ UDA, CPI, 0, 0, 0, -1, -1, { nothashmark32, NULL }, test32_4 },
	{ UDA, CPI, 1, 0, 0, 1, 2, { nothashmark32, NULL }, test32_4 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { nothashmark32, NULL }, test32_4 },
	{ UDA, CPI, 1, 0, 0, 1, 2, { nothashmark32, NULL }, test32_2 },
	{ UDA, CPI, 2, 0, 0, -1, -1, { nothashmark32, NULL }, test32_2 },

	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 2, 3, { afternl32, NULL }, test32_5 },
	{ PCRE2_UTF | PCRE2_MULTILINE, CI, 1, 0, 0, 3, 4, { afternl32, NULL }, test32_6 },

	{ 0, 0, 0, 0, 0, 0, 0, { NULL, NULL }, NULL }
};

#undef UDA
#undef CI
#undef CPI

static int run_invalid_utf32_test(const struct invalid_utf32_regression_test_case *current,
	int pattern_index, int i, pcre2_compile_context_32 *ccontext, pcre2_match_data_32 *mdata)
{
	pcre2_code_32 *code;
	int result, errorcode;
	PCRE2_SIZE length, erroroffset;
	const PCRE2_UCHAR32 *input;
	PCRE2_SIZE *ovector = pcre2_get_ovector_pointer_32(mdata);

	if (current->pattern[i] == NULL)
		return 1;

	code = pcre2_compile_32(current->pattern[i], PCRE2_ZERO_TERMINATED,
		current->compile_options, &errorcode, &erroroffset, ccontext);

	if (!code) {
		printf("Pattern[%d:0] cannot be compiled. Error offset: %d\n", pattern_index, (int)erroroffset);
		return 0;
	}

	if (pcre2_jit_compile_32(code, current->jit_compile_options) != 0) {
		printf("Pattern[%d:0] cannot be compiled by the JIT compiler.\n", pattern_index);
		pcre2_code_free_32(code);
		return 0;
	}

	input = current->input;
	length = 0;

	while (*input++ != 0)
		length++;

	length -= current->skip_left + current->skip_right;

	if (current->jit_compile_options & PCRE2_JIT_COMPLETE) {
		result = pcre2_jit_match_32(code, (current->input + current->skip_left),
			length, current->start_offset - current->skip_left, 0, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_32(code);
			return 0;
		}
	}

	if (current->jit_compile_options & PCRE2_JIT_PARTIAL_SOFT) {
		result = pcre2_jit_match_32(code, (current->input + current->skip_left),
			length, current->start_offset - current->skip_left, PCRE2_PARTIAL_SOFT, mdata, NULL);

		if (check_invalid_utf_result(pattern_index, "partial match", result, current->match_start, current->match_end, ovector)) {
			pcre2_code_free_32(code);
			return 0;
		}
	}

	pcre2_code_free_32(code);
	return 1;
}

static int invalid_utf32_regression_tests(void)
{
	const struct invalid_utf32_regression_test_case *current;
	pcre2_compile_context_32 *ccontext;
	pcre2_match_data_32 *mdata;
	int total = 0, successful = 0;
	int result;

	printf("\nRunning invalid-utf32 JIT regression tests\n");

	ccontext = pcre2_compile_context_create_32(NULL);
	pcre2_set_newline_32(ccontext, PCRE2_NEWLINE_ANY);
	mdata = pcre2_match_data_create_32(4, NULL);

	for (current = invalid_utf32_regression_test_cases; current->pattern[0]; current++) {
		/* printf("\nPattern: %s :\n", current->pattern); */
		total++;

		result = 1;
		if (!run_invalid_utf32_test(current, total - 1, 0, ccontext, mdata))
			result = 0;
		if (!run_invalid_utf32_test(current, total - 1, 1, ccontext, mdata))
			result = 0;

		if (result) {
			successful++;
		}

		printf(".");
		if ((total % 60) == 0)
			printf("\n");
	}

	if ((total % 60) != 0)
		printf("\n");

	pcre2_match_data_free_32(mdata);
	pcre2_compile_context_free_32(ccontext);

	if (total == successful) {
		printf("\nAll invalid UTF32 JIT regression tests are successfully passed.\n");
		return 0;
	} else {
		printf("\nInvalid UTF32 successful test ratio: %d%% (%d failed)\n", successful * 100 / total, total - successful);
		return 1;
	}
}

#else /* !SUPPORT_UNICODE || !SUPPORT_PCRE2_32 */

static int invalid_utf32_regression_tests(void)
{
	return 0;
}

#endif /* SUPPORT_UNICODE && SUPPORT_PCRE2_32 */

/* End of pcre2_jit_test.c */
