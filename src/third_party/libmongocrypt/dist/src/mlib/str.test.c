#include "./str.h"

#define CHECK(Expr)                                   \
   ((Expr) ? 0                                        \
           : ((fprintf (stderr,                       \
                        "%s:%d: Check '%s' failed\n", \
                        __FILE__,                     \
                        __LINE__,                     \
                        #Expr),                       \
               abort ()),                             \
              0))

#define test_predicate(Bool, Left, Pred, Right) \
   MSTR_ASSERT (Bool, mstrv_lit (Left), Pred, mstrv_lit (Right))

int
main ()
{
   // Test the null-initializers:
   mstr str = MSTR_NULL;
   mstr_view null_view = MSTRV_NULL;
   (void) null_view;

   str = mstr_copy_cstr ("foo");
   CHECK (str.len == 3);
   MSTR_ASSERT_EQ (str.view, mstrv_lit ("foo"));
   CHECK (strncmp (str.data, "foo", 3) == 0);

   mstr_inplace_append (&str, mstrv_lit ("bar"));
   MSTR_ASSERT_EQ (str.view, mstrv_lit ("foobar"));

   mstr_free (str);

   str = mstr_copy_cstr ("foobar");
   mstr_inplace_trunc (&str, 3);
   MSTR_ASSERT_EQ (str.view, mstrv_lit ("foo"));
   mstr_free (str);

   int pos = mstr_find (mstrv_lit ("foo"), mstrv_lit ("bar"));
   CHECK (pos == -1);

   pos = mstr_find (mstrv_lit ("foo"), mstrv_lit ("barbaz"));
   CHECK (pos == -1);

   pos = mstr_find (mstrv_lit ("foobar"), mstrv_lit ("bar"));
   CHECK (pos == 3);

   // Simple replacement:
   str = mstr_copy_cstr ("foo bar baz");
   mstr str2 = mstr_replace (str.view, mstrv_lit ("bar"), mstrv_lit ("foo"));
   MSTR_ASSERT_EQ (str2.view, mstrv_lit ("foo foo baz"));
   mstr_free (str);

   // Replace multiple instances:
   mstr_inplace_replace (&str2, mstrv_lit ("foo"), mstrv_lit ("baz"));
   MSTR_ASSERT_EQ (str2.view, mstrv_lit ("baz baz baz"));

   // Replace with a string containing the needle:
   mstr_inplace_replace (&str2, mstrv_lit ("baz"), mstrv_lit ("foo bar baz"));
   MSTR_ASSERT_EQ (str2.view,
                   mstrv_lit ("foo bar baz foo bar baz foo bar baz"));

   // Replace with empty string:
   mstr_inplace_replace (&str2, mstrv_lit ("bar "), mstrv_lit (""));
   MSTR_ASSERT_EQ (str2.view, mstrv_lit ("foo baz foo baz foo baz"));

   // Replacing a string that isn't there:
   mstr_inplace_replace (&str2, mstrv_lit ("quux"), mstrv_lit ("nope"));
   MSTR_ASSERT_EQ (str2.view, mstrv_lit ("foo baz foo baz foo baz"));

   // Replacing an empty string is just a duplication:
   mstr_inplace_replace (&str2, mstrv_lit (""), mstrv_lit ("never"));
   MSTR_ASSERT_EQ (str2.view, mstrv_lit ("foo baz foo baz foo baz"));

   mstr_free (str2);

   CHECK (mstrv_view_cstr ("foo\000bar").len == 3);
   CHECK (mstrv_lit ("foo\000bar").len == 7);

   str = mstr_new (0).mstr;
   MSTR_ITER_SPLIT (part, mstrv_lit ("foo bar baz"), mstrv_lit (" "))
   {
      mstr_inplace_append (&str, part);
      if (mstr_eq (part, mstrv_lit ("bar"))) {
         break;
      }
   }
   MSTR_ASSERT_EQ (str.view, mstrv_lit ("foobar"));
   mstr_free (str);

   // rfind at the beginning of the string
   CHECK (mstr_rfind (mstrv_lit ("foobar"), mstrv_lit ("foo")) == 0);

   str = mstr_splice (mstrv_lit ("foobar"), 1, 2, MSTRV_NULL);
   MSTR_ASSERT_EQ (str.view, mstrv_lit ("fbar"));
   mstr_free (str);

   test_predicate (true, "foo", contains, "o");
   test_predicate (true, "foo", contains, "oo");
   test_predicate (true, "foo", contains, "foo");
   test_predicate (true, "foo", contains, "fo");
   test_predicate (true, "foo", contains, "f");
   test_predicate (true, "foo", contains, "");
   test_predicate (false, "foo", contains, "fooo");
   test_predicate (false, "foo", contains, "ofo");
   test_predicate (false, "foo", contains, "of");
   test_predicate (false, "foo", contains, "bar");

   test_predicate (true, "foo", starts_with, "f");
   test_predicate (true, "foo", starts_with, "fo");
   test_predicate (true, "foo", starts_with, "foo");
   test_predicate (true, "foo", starts_with, "");
   test_predicate (false, "foo", starts_with, "o");
   test_predicate (false, "foo", starts_with, "oo");
   test_predicate (false, "foo", starts_with, "oof");
   test_predicate (false, "foo", starts_with, "bar");

   test_predicate (true, "foo", ends_with, "o");
   test_predicate (true, "foo", ends_with, "oo");
   test_predicate (true, "foo", ends_with, "foo");
   test_predicate (true, "foo", ends_with, "");
   test_predicate (false, "foo", ends_with, "f");
   test_predicate (false, "foo", ends_with, "fo");
   test_predicate (false, "foo", ends_with, "oof");
   test_predicate (false, "foo", ends_with, "bar");

#ifdef _WIN32
   const wchar_t *wide = L"🕴️";
   mstr_narrow_result narrow = mstr_win32_narrow (wide);
   CHECK (narrow.error == 0);
   MSTR_ASSERT_EQ (
      narrow.string.view,
      mstrv_lit (
         "\xc3\xb0\xc5\xb8\xe2\x80\xa2\xc2\xb4\xc3\xaf\xc2\xb8\xc2\x8f"));
   mstr_free (narrow.string);
#endif
}
