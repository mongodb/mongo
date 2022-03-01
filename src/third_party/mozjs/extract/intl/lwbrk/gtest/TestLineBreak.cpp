/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdio.h>
#include "nsXPCOM.h"
#include "nsISupports.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "gtest/gtest.h"

#include "mozilla/intl/LineBreaker.h"
#include "mozilla/intl/WordBreaker.h"

static char teng1[] =
    //          1         2         3         4         5         6         7
    // 01234567890123456789012345678901234567890123456789012345678901234567890123456789
    "This is a test to test(reasonable) line    break. This 0.01123 = 45 x 48.";

static uint32_t lexp1[] = {4,  7,  9,  14, 17, 34, 39, 40, 41,
                           42, 49, 54, 62, 64, 67, 69, 73};

static uint32_t wexp1[] = {4,  5,  7,  8,  9,  10, 14, 15, 17, 18, 22,
                           23, 33, 34, 35, 39, 43, 48, 49, 50, 54, 55,
                           56, 57, 62, 63, 64, 65, 67, 68, 69, 70, 72};

static char teng2[] =
    //          1         2         3         4         5         6         7
    // 01234567890123456789012345678901234567890123456789012345678901234567890123456789
    "()((reasonab(l)e) line  break. .01123=45x48.";

static uint32_t lexp2[] = {17, 22, 23, 30, 44};

static uint32_t wexp2[] = {4,  12, 13, 14, 15, 16, 17, 18, 22,
                           24, 29, 30, 31, 32, 37, 38, 43};

static char teng3[] =
    //          1         2         3         4         5         6         7
    // 01234567890123456789012345678901234567890123456789012345678901234567890123456789
    "It's a test to test(ronae ) line break....";

static uint32_t lexp3[] = {4, 6, 11, 14, 25, 27, 32, 42};

static uint32_t wexp3[] = {2,  3,  4,  5,  6,  7,  11, 12, 14, 15,
                           19, 20, 25, 26, 27, 28, 32, 33, 38};

static char ruler1[] =
    "          1         2         3         4         5         6         7  ";
static char ruler2[] =
    "0123456789012345678901234567890123456789012345678901234567890123456789012";

bool Check(const char* in, const uint32_t* out, uint32_t outlen, uint32_t i,
           uint32_t res[256]) {
  bool ok = true;

  if (i != outlen) {
    ok = false;
    printf("WARNING!!! return size wrong, expect %d but got %d \n", outlen, i);
  }

  for (uint32_t j = 0; j < i; j++) {
    if (j < outlen) {
      if (res[j] != out[j]) {
        ok = false;
        printf("[%d] expect %d but got %d\n", j, out[j], res[j]);
      }
    } else {
      ok = false;
      printf("[%d] additional %d\n", j, res[j]);
    }
  }

  if (!ok) {
    printf("string  = \n%s\n", in);
    printf("%s\n", ruler1);
    printf("%s\n", ruler2);

    printf("Expect = \n");
    for (uint32_t j = 0; j < outlen; j++) {
      printf("%d,", out[j]);
    }

    printf("\nResult = \n");
    for (uint32_t j = 0; j < i; j++) {
      printf("%d,", res[j]);
    }
    printf("\n");
  }

  return ok;
}

bool TestASCIILB(mozilla::intl::LineBreaker* lb, const char* in,
                 const uint32_t* out, uint32_t outlen) {
  NS_ConvertASCIItoUTF16 eng1(in);
  uint32_t i;
  uint32_t res[256];
  int32_t curr;

  for (i = 0, curr = 0; curr != NS_LINEBREAKER_NEED_MORE_TEXT && i < 256; i++) {
    curr = lb->Next(eng1.get(), eng1.Length(), curr);
    res[i] = curr != NS_LINEBREAKER_NEED_MORE_TEXT ? curr : eng1.Length();
  }

  return Check(in, out, outlen, i, res);
}

bool TestASCIIWB(mozilla::intl::WordBreaker* lb, const char* in,
                 const uint32_t* out, uint32_t outlen) {
  NS_ConvertASCIItoUTF16 eng1(in);

  uint32_t i;
  uint32_t res[256];
  int32_t curr = 0;

  for (i = 0, curr = lb->NextWord(eng1.get(), eng1.Length(), curr);
       curr != NS_WORDBREAKER_NEED_MORE_TEXT && i < 256;
       curr = lb->NextWord(eng1.get(), eng1.Length(), curr), i++) {
    res[i] = curr != NS_WORDBREAKER_NEED_MORE_TEXT ? curr : eng1.Length();
  }

  return Check(in, out, outlen, i, res);
}

TEST(LineBreak, LineBreaker)
{
  RefPtr<mozilla::intl::LineBreaker> t = mozilla::intl::LineBreaker::Create();

  ASSERT_TRUE(t);

  ASSERT_TRUE(TestASCIILB(t, teng1, lexp1, sizeof(lexp1) / sizeof(uint32_t)));
  ASSERT_TRUE(TestASCIILB(t, teng2, lexp2, sizeof(lexp2) / sizeof(uint32_t)));
  ASSERT_TRUE(TestASCIILB(t, teng3, lexp3, sizeof(lexp3) / sizeof(uint32_t)));
}

TEST(LineBreak, WordBreaker)
{
  RefPtr<mozilla::intl::WordBreaker> t = mozilla::intl::WordBreaker::Create();
  ASSERT_TRUE(t);

  ASSERT_TRUE(TestASCIIWB(t, teng1, wexp1, sizeof(wexp1) / sizeof(uint32_t)));
  ASSERT_TRUE(TestASCIIWB(t, teng2, wexp2, sizeof(wexp2) / sizeof(uint32_t)));
  ASSERT_TRUE(TestASCIIWB(t, teng3, wexp3, sizeof(wexp3) / sizeof(uint32_t)));
}

//                         012345678901234
static const char wb0[] = "T";
static const char wb1[] = "h";
static const char wb2[] = "is   is a int";
static const char wb3[] = "ernationali";
static const char wb4[] = "zation work.";

static const char* wb[] = {wb0, wb1, wb2, wb3, wb4};

void TestPrintWordWithBreak() {
  uint32_t numOfFragment = sizeof(wb) / sizeof(char*);
  RefPtr<mozilla::intl::WordBreaker> wbk = mozilla::intl::WordBreaker::Create();

  nsAutoString result;

  for (uint32_t i = 0; i < numOfFragment; i++) {
    NS_ConvertASCIItoUTF16 fragText(wb[i]);

    int32_t cur = 0;
    cur = wbk->NextWord(fragText.get(), fragText.Length(), cur);
    uint32_t start = 0;
    for (uint32_t j = 0; cur != NS_WORDBREAKER_NEED_MORE_TEXT; j++) {
      result.Append(Substring(fragText, start, cur - start));
      result.Append('^');
      start = (cur >= 0 ? cur : cur - start);
      cur = wbk->NextWord(fragText.get(), fragText.Length(), cur);
    }

    result.Append(Substring(fragText, fragText.Length() - start));

    if (i != numOfFragment - 1) {
      NS_ConvertASCIItoUTF16 nextFragText(wb[i + 1]);

      bool canBreak = true;
      canBreak = wbk->BreakInBetween(fragText.get(), fragText.Length(),
                                     nextFragText.get(), nextFragText.Length());
      if (canBreak) {
        result.Append('^');
      }
      fragText.Assign(nextFragText);
    }
  }
  ASSERT_STREQ("is^   ^is^ ^a^ ^  is a intzation^ ^work^ation work.",
               NS_ConvertUTF16toUTF8(result).get());
}

void TestFindWordBreakFromPosition(uint32_t fragN, uint32_t offset,
                                   const char* expected) {
  uint32_t numOfFragment = sizeof(wb) / sizeof(char*);
  RefPtr<mozilla::intl::WordBreaker> wbk = mozilla::intl::WordBreaker::Create();

  NS_ConvertASCIItoUTF16 fragText(wb[fragN]);

  mozilla::intl::WordRange res =
      wbk->FindWord(fragText.get(), fragText.Length(), offset);

  bool canBreak;
  nsAutoString result(Substring(fragText, res.mBegin, res.mEnd - res.mBegin));

  if ((uint32_t)fragText.Length() == res.mEnd) {
    // if we hit the end of the fragment
    nsAutoString curFragText = fragText;
    for (uint32_t p = fragN + 1; p < numOfFragment; p++) {
      NS_ConvertASCIItoUTF16 nextFragText(wb[p]);
      canBreak = wbk->BreakInBetween(curFragText.get(), curFragText.Length(),
                                     nextFragText.get(), nextFragText.Length());
      if (canBreak) {
        break;
      }
      mozilla::intl::WordRange r =
          wbk->FindWord(nextFragText.get(), nextFragText.Length(), 0);

      result.Append(Substring(nextFragText, r.mBegin, r.mEnd - r.mBegin));

      if ((uint32_t)nextFragText.Length() != r.mEnd) {
        break;
      }
      nextFragText.Assign(curFragText);
    }
  }

  if (0 == res.mBegin) {
    // if we hit the beginning of the fragment
    nsAutoString curFragText = fragText;
    for (uint32_t p = fragN; p > 0; p--) {
      NS_ConvertASCIItoUTF16 prevFragText(wb[p - 1]);
      canBreak = wbk->BreakInBetween(prevFragText.get(), prevFragText.Length(),
                                     curFragText.get(), curFragText.Length());
      if (canBreak) {
        break;
      }
      mozilla::intl::WordRange r = wbk->FindWord(
          prevFragText.get(), prevFragText.Length(), prevFragText.Length());

      result.Insert(Substring(prevFragText, r.mBegin, r.mEnd - r.mBegin), 0);

      if (0 != r.mBegin) {
        break;
      }
      prevFragText.Assign(curFragText);
    }
  }

  ASSERT_STREQ(expected, NS_ConvertUTF16toUTF8(result).get())
      << "FindWordBreakFromPosition(" << fragN << ", " << offset << ")";
}

void TestNextWordBreakWithComplexLanguage() {
  RefPtr<mozilla::intl::WordBreaker> wbk = mozilla::intl::WordBreaker::Create();
  nsString fragText(u"\u0e40\u0e1b\u0e47\u0e19\u0e19\u0e31\u0e01");

  int32_t offset = 0;
  while (offset != NS_WORDBREAKER_NEED_MORE_TEXT) {
    int32_t newOffset =
        wbk->NextWord(fragText.get(), fragText.Length(), offset);
    ASSERT_NE(offset, newOffset);
    offset = newOffset;
  }
  ASSERT_TRUE(true);
}

TEST(LineBreak, WordBreakUsage)
{
  TestPrintWordWithBreak();
  TestFindWordBreakFromPosition(0, 0, "This");
  TestFindWordBreakFromPosition(1, 0, "his");
  TestFindWordBreakFromPosition(2, 0, "is");
  TestFindWordBreakFromPosition(2, 1, "is");
  TestFindWordBreakFromPosition(2, 9, " ");
  TestFindWordBreakFromPosition(2, 10, "internationalization");
  TestFindWordBreakFromPosition(3, 4, "ernationalization");
  TestFindWordBreakFromPosition(3, 8, "ernationalization");
  TestFindWordBreakFromPosition(4, 6, " ");
  TestFindWordBreakFromPosition(4, 7, "work");
  TestNextWordBreakWithComplexLanguage();
}
