/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/JSONWriter.h"
#include "mozilla/UniquePtr.h"
#include <stdio.h>
#include <string.h>

using mozilla::JSONWriteFunc;
using mozilla::JSONWriter;
using mozilla::MakeUnique;

// This writes all the output into a big buffer.
struct StringWriteFunc : public JSONWriteFunc
{
  const static size_t kLen = 100000;
  char mBuf[kLen];
  char* mPtr;

  StringWriteFunc() : mPtr(mBuf) {}

  void Write(const char* aStr)
  {
    char* last = mPtr + strlen(aStr);    // where the nul will be added

    // If you change this test and this assertion fails, just make kLen bigger.
    MOZ_RELEASE_ASSERT(last < mBuf + kLen);
    sprintf(mPtr, "%s", aStr);
    mPtr = last;
  }
};

void Check(JSONWriteFunc* aFunc, const char* aExpected)
{
  const char* actual = static_cast<StringWriteFunc*>(aFunc)->mBuf;
  if (strcmp(aExpected, actual) != 0) {
    fprintf(stderr,
            "---- EXPECTED ----\n<<<%s>>>\n"
            "---- ACTUAL ----\n<<<%s>>>\n",
            aExpected, actual);
    MOZ_RELEASE_ASSERT(false, "expected and actual output don't match");
  }
}

// Note: to convert actual output into |expected| strings that C++ can handle,
// apply the following substitutions, in order, to each line.
// - s/\\/\\\\/g    # escapes backslashes
// - s/"/\\"/g      # escapes quotes
// - s/$/\\n\\/     # adds a newline and string continuation char to each line

void TestBasicProperties()
{
  const char* expected = "\
{\n\
 \"null\": null,\n\
 \"bool1\": true,\n\
 \"bool2\": false,\n\
 \"int1\": 123,\n\
 \"int2\": -123,\n\
 \"int3\": -123456789000,\n\
 \"double1\": 1.2345,\n\
 \"double2\": -3,\n\
 \"double3\": 1e-7,\n\
 \"double4\": 1.1111111111111111e+21,\n\
 \"string1\": \"\",\n\
 \"string2\": \"1234\",\n\
 \"string3\": \"hello\",\n\
 \"string4\": \"\\\" \\\\ \\u0007 \\b \\t \\n \\u000b \\f \\r\",\n\
 \"len 0 array, multi-line\": [\n\
 ],\n\
 \"len 0 array, single-line\": [],\n\
 \"len 1 array\": [\n\
  1\n\
 ],\n\
 \"len 5 array, multi-line\": [\n\
  1,\n\
  2,\n\
  3,\n\
  4,\n\
  5\n\
 ],\n\
 \"len 3 array, single-line\": [1, [{}, 2, []], 3],\n\
 \"len 0 object, multi-line\": {\n\
 },\n\
 \"len 0 object, single-line\": {},\n\
 \"len 1 object\": {\n\
  \"one\": 1\n\
 },\n\
 \"len 5 object\": {\n\
  \"one\": 1,\n\
  \"two\": 2,\n\
  \"three\": 3,\n\
  \"four\": 4,\n\
  \"five\": 5\n\
 },\n\
 \"len 3 object, single-line\": {\"a\": 1, \"b\": [{}, 2, []], \"c\": 3}\n\
}\n\
";

  JSONWriter w(MakeUnique<StringWriteFunc>());

  w.Start();
  {
    w.NullProperty("null");

    w.BoolProperty("bool1", true);
    w.BoolProperty("bool2", false);

    w.IntProperty("int1", 123);
    w.IntProperty("int2", -0x7b);
    w.IntProperty("int3", -123456789000ll);

    w.DoubleProperty("double1", 1.2345);
    w.DoubleProperty("double2", -3);
    w.DoubleProperty("double3", 1e-7);
    w.DoubleProperty("double4", 1.1111111111111111e+21);

    w.StringProperty("string1", "");
    w.StringProperty("string2", "1234");
    w.StringProperty("string3", "hello");
    w.StringProperty("string4", "\" \\ \a \b \t \n \v \f \r");

    w.StartArrayProperty("len 0 array, multi-line", w.MultiLineStyle);
    w.EndArray();

    w.StartArrayProperty("len 0 array, single-line", w.SingleLineStyle);
    w.EndArray();

    w.StartArrayProperty("len 1 array");
    {
      w.IntElement(1);
    }
    w.EndArray();

    w.StartArrayProperty("len 5 array, multi-line", w.MultiLineStyle);
    {
      w.IntElement(1);
      w.IntElement(2);
      w.IntElement(3);
      w.IntElement(4);
      w.IntElement(5);
    }
    w.EndArray();

    w.StartArrayProperty("len 3 array, single-line", w.SingleLineStyle);
    {
      w.IntElement(1);
      w.StartArrayElement();
      {
        w.StartObjectElement(w.SingleLineStyle);
        w.EndObject();

        w.IntElement(2);

        w.StartArrayElement(w.MultiLineStyle);  // style overridden from above
        w.EndArray();
      }
      w.EndArray();
      w.IntElement(3);
    }
    w.EndArray();

    w.StartObjectProperty("len 0 object, multi-line");
    w.EndObject();

    w.StartObjectProperty("len 0 object, single-line", w.SingleLineStyle);
    w.EndObject();

    w.StartObjectProperty("len 1 object");
    {
      w.IntProperty("one", 1);
    }
    w.EndObject();

    w.StartObjectProperty("len 5 object");
    {
      w.IntProperty("one", 1);
      w.IntProperty("two", 2);
      w.IntProperty("three", 3);
      w.IntProperty("four", 4);
      w.IntProperty("five", 5);
    }
    w.EndObject();

    w.StartObjectProperty("len 3 object, single-line", w.SingleLineStyle);
    {
      w.IntProperty("a", 1);
      w.StartArrayProperty("b");
      {
        w.StartObjectElement();
        w.EndObject();

        w.IntElement(2);

        w.StartArrayElement(w.SingleLineStyle);
        w.EndArray();
      }
      w.EndArray();
      w.IntProperty("c", 3);
    }
    w.EndObject();
  }
  w.End();

  Check(w.WriteFunc(), expected);
}

void TestBasicElements()
{
  const char* expected = "\
{\n\
 \"array\": [\n\
  null,\n\
  true,\n\
  false,\n\
  123,\n\
  -123,\n\
  -123456789000,\n\
  1.2345,\n\
  -3,\n\
  1e-7,\n\
  1.1111111111111111e+21,\n\
  \"\",\n\
  \"1234\",\n\
  \"hello\",\n\
  \"\\\" \\\\ \\u0007 \\b \\t \\n \\u000b \\f \\r\",\n\
  [\n\
  ],\n\
  [],\n\
  [\n\
   1\n\
  ],\n\
  [\n\
   1,\n\
   2,\n\
   3,\n\
   4,\n\
   5\n\
  ],\n\
  [1, [{}, 2, []], 3],\n\
  {\n\
  },\n\
  {},\n\
  {\n\
   \"one\": 1\n\
  },\n\
  {\n\
   \"one\": 1,\n\
   \"two\": 2,\n\
   \"three\": 3,\n\
   \"four\": 4,\n\
   \"five\": 5\n\
  },\n\
  {\"a\": 1, \"b\": [{}, 2, []], \"c\": 3}\n\
 ]\n\
}\n\
";

  JSONWriter w(MakeUnique<StringWriteFunc>());

  w.Start();
  w.StartArrayProperty("array");
  {
    w.NullElement();

    w.BoolElement(true);
    w.BoolElement(false);

    w.IntElement(123);
    w.IntElement(-0x7b);
    w.IntElement(-123456789000ll);

    w.DoubleElement(1.2345);
    w.DoubleElement(-3);
    w.DoubleElement(1e-7);
    w.DoubleElement(1.1111111111111111e+21);

    w.StringElement("");
    w.StringElement("1234");
    w.StringElement("hello");
    w.StringElement("\" \\ \a \b \t \n \v \f \r");

    w.StartArrayElement();
    w.EndArray();

    w.StartArrayElement(w.SingleLineStyle);
    w.EndArray();

    w.StartArrayElement();
    {
      w.IntElement(1);
    }
    w.EndArray();

    w.StartArrayElement();
    {
      w.IntElement(1);
      w.IntElement(2);
      w.IntElement(3);
      w.IntElement(4);
      w.IntElement(5);
    }
    w.EndArray();

    w.StartArrayElement(w.SingleLineStyle);
    {
      w.IntElement(1);
      w.StartArrayElement();
      {
        w.StartObjectElement(w.SingleLineStyle);
        w.EndObject();

        w.IntElement(2);

        w.StartArrayElement(w.MultiLineStyle);  // style overridden from above
        w.EndArray();
      }
      w.EndArray();
      w.IntElement(3);
    }
    w.EndArray();

    w.StartObjectElement();
    w.EndObject();

    w.StartObjectElement(w.SingleLineStyle);
    w.EndObject();

    w.StartObjectElement();
    {
      w.IntProperty("one", 1);
    }
    w.EndObject();

    w.StartObjectElement();
    {
      w.IntProperty("one", 1);
      w.IntProperty("two", 2);
      w.IntProperty("three", 3);
      w.IntProperty("four", 4);
      w.IntProperty("five", 5);
    }
    w.EndObject();

    w.StartObjectElement(w.SingleLineStyle);
    {
      w.IntProperty("a", 1);
      w.StartArrayProperty("b");
      {
        w.StartObjectElement();
        w.EndObject();

        w.IntElement(2);

        w.StartArrayElement(w.SingleLineStyle);
        w.EndArray();
      }
      w.EndArray();
      w.IntProperty("c", 3);
    }
    w.EndObject();
  }
  w.EndArray();
  w.End();

  Check(w.WriteFunc(), expected);
}

void TestOneLineObject()
{
  const char* expected = "\
{\"i\": 1, \"array\": [null, [{}], {\"o\": {}}, \"s\"], \"d\": 3.33}\n\
";

  JSONWriter w(MakeUnique<StringWriteFunc>());

  w.Start(w.SingleLineStyle);

  w.IntProperty("i", 1);

  w.StartArrayProperty("array");
  {
    w.NullElement();

    w.StartArrayElement(w.MultiLineStyle);  // style overridden from above
    {
      w.StartObjectElement();
      w.EndObject();
    }
    w.EndArray();

    w.StartObjectElement();
    {
      w.StartObjectProperty("o");
      w.EndObject();
    }
    w.EndObject();

    w.StringElement("s");
  }
  w.EndArray();

  w.DoubleProperty("d", 3.33);

  w.End();

  Check(w.WriteFunc(), expected);
}

void TestStringEscaping()
{
  // This test uses hexadecimal character escapes because UTF8 literals cause
  // problems for some compilers (see bug 1069726).
  const char* expected = "\
{\n\
 \"ascii\": \"\x7F~}|{zyxwvutsrqponmlkjihgfedcba`_^]\\\\[ZYXWVUTSRQPONMLKJIHGFEDCBA@?>=<;:9876543210/.-,+*)('&%$#\\\"! \\u001f\\u001e\\u001d\\u001c\\u001b\\u001a\\u0019\\u0018\\u0017\\u0016\\u0015\\u0014\\u0013\\u0012\\u0011\\u0010\\u000f\\u000e\\r\\f\\u000b\\n\\t\\b\\u0007\\u0006\\u0005\\u0004\\u0003\\u0002\\u0001\",\n\
 \"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\x87\xD9\x86\xD8\xA7\xD9\x83\": true,\n\
 \"\xD5\xA2\xD5\xA1\xD6\x80\xD5\xA5\xD6\x82 \xD5\xB9\xD5\xAF\xD5\xA1\": -123,\n\
 \"\xE4\xBD\xA0\xE5\xA5\xBD\": 1.234,\n\
 \"\xCE\xB3\xCE\xB5\xCE\xB9\xCE\xB1 \xCE\xB5\xCE\xBA\xCE\xB5\xCE\xAF\": \"\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85\",\n\
 \"hall\xC3\xB3 \xC3\xBE" "arna\": 4660,\n\
 \"\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\": {\n\
  \"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\": [\n\
  ]\n\
 }\n\
}\n\
";

  JSONWriter w(MakeUnique<StringWriteFunc>());

  // Test the string escaping behaviour.
  w.Start();
  {
    // Test all 127 ascii values. Do it in reverse order so that the 0
    // at the end serves as the null char.
    char buf[128];
    for (int i = 0; i < 128; i++) {
      buf[i] = 127 - i;
    }
    w.StringProperty("ascii", buf);

    // Test lots of unicode stuff. Note that this file is encoded as UTF-8.
    w.BoolProperty("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\x87\xD9\x86\xD8\xA7\xD9\x83", true);
    w.IntProperty("\xD5\xA2\xD5\xA1\xD6\x80\xD5\xA5\xD6\x82 \xD5\xB9\xD5\xAF\xD5\xA1", -123);
    w.DoubleProperty("\xE4\xBD\xA0\xE5\xA5\xBD", 1.234);
    w.StringProperty("\xCE\xB3\xCE\xB5\xCE\xB9\xCE\xB1 \xCE\xB5\xCE\xBA\xCE\xB5\xCE\xAF", "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85");
    w.IntProperty("hall\xC3\xB3 \xC3\xBE" "arna", 0x1234);
    w.StartObjectProperty("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
    {
      w.StartArrayProperty("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
      w.EndArray();
    }
    w.EndObject();
  }
  w.End();

  Check(w.WriteFunc(), expected);
}

void TestDeepNesting()
{
  const char* expected = "\
{\n\
 \"a\": [\n\
  {\n\
   \"a\": [\n\
    {\n\
     \"a\": [\n\
      {\n\
       \"a\": [\n\
        {\n\
         \"a\": [\n\
          {\n\
           \"a\": [\n\
            {\n\
             \"a\": [\n\
              {\n\
               \"a\": [\n\
                {\n\
                 \"a\": [\n\
                  {\n\
                   \"a\": [\n\
                    {\n\
                    }\n\
                   ]\n\
                  }\n\
                 ]\n\
                }\n\
               ]\n\
              }\n\
             ]\n\
            }\n\
           ]\n\
          }\n\
         ]\n\
        }\n\
       ]\n\
      }\n\
     ]\n\
    }\n\
   ]\n\
  }\n\
 ]\n\
}\n\
";

  JSONWriter w(MakeUnique<StringWriteFunc>());

  w.Start();
  {
    static const int n = 10;
    for (int i = 0; i < n; i++) {
      w.StartArrayProperty("a");
      w.StartObjectElement();
    }
    for (int i = 0; i < n; i++) {
      w.EndObject();
      w.EndArray();
    }
  }
  w.End();

  Check(w.WriteFunc(), expected);
}

int main(void)
{
  TestBasicProperties();
  TestBasicElements();
  TestOneLineObject();
  TestStringEscaping();
  TestDeepNesting();

  return 0;
}
