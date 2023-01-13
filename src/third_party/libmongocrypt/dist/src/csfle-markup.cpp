#ifdef NDEBUG
#undef NDEBUG
#endif

#include <bson/bson.h>

extern "C" {
#include "./mongocrypt-dll-private.h"
}

#include <sstream>
#include <iostream>
#include <fstream>
#include <string>

#include <mongo_crypt-v1.h>

namespace
{
void
print_usage ()
{
   std::cout << "Usage:\n"
             << "  csfle-markup <csfle-dll-path> [doc-namespace]\n"
             << "\n"
             << "Write a JSON document to stdin, and the resulting marked-up\n"
             << "document will be written to stdout.\n\n"
             << "Hint: Pipe through \"jq .\" for prettier output.\n";
}

template <typename Fn> struct deferred_fn {
   Fn fn;

   ~deferred_fn ()
   {
      fn ();
   }
};

template <typename F>
deferred_fn<F>
make_deferred (F fn)
{
   return deferred_fn<F>{fn};
}

#define DEFER(...) make_deferred ([&] __VA_ARGS__)

void
bson_child (bson_t *out, bson_iter_t *it)
{
   uint32_t len;
   const uint8_t *data;
   if (BSON_ITER_HOLDS_DOCUMENT (it)) {
      bson_iter_document (it, &len, &data);
   }
   if (BSON_ITER_HOLDS_ARRAY (it)) {
      bson_iter_array (it, &len, &data);
   }
   bool okay = bson_init_static (out, data, len);
   assert (okay);
}

void
print_field_info (bson_t *bson, std::string path)
{
   bson_iter_t iter;
   for (bson_iter_init (&iter, bson); bson_iter_next (&iter);) {
      auto key = bson_iter_key (&iter);
      auto child_path = path + "/" + key;
      if (BSON_ITER_HOLDS_ARRAY (&iter) || BSON_ITER_HOLDS_DOCUMENT (&iter)) {
         bson_t child;
         bson_child (&child, &iter);
         print_field_info (&child, child_path);
      }
      if (BSON_ITER_HOLDS_BINARY (&iter)) {
         bson_subtype_t subtype;
         uint32_t len;
         const uint8_t *data;
         bson_iter_binary (&iter, &subtype, &len, &data);
         if (subtype == BSON_SUBTYPE_ENCRYPTED && len) {
            bson_t encoded;
            uint8_t subsubtype = data[0];
            if (subsubtype == 0) {
               // Intent-to-encrypt
               BSON_ASSERT (len > 0);
               bson_init_static (&encoded, data + 1, len - 1);
               auto s = bson_as_canonical_extended_json (&encoded, nullptr);
               fprintf (stderr,
                        "BSON binary at [%s] has encoded intent-to-encrypt "
                        "content: %s\n",
                        child_path.data (),
                        s);
               bson_free (s);
            }
         }
      }
   }
}

int
do_main (int argc, const char *const *argv)
{
   if (argc < 2 || argc > 3) {
      print_usage ();
      return 2;
   }
   if (argv[1] == std::string ("--help")) {
      print_usage ();
      return 0;
   }
   const auto csfle_path = argv[1];
   const auto doc_ns = argc > 2 ? argv[2] : "";

   mcr_dll csfle = mcr_dll_open (argv[1]);
   auto close_csfle = DEFER ({ mcr_dll_close (csfle); });
   if (csfle.error_string.data) {
      std::cerr << "Failed to open [" << argv[1]
                << "] as a dynamic library: " << csfle.error_string.data
                << '\n';
      return 3;
   }

#define LOAD_SYM(Name)                                                       \
   auto Name =                                                               \
      reinterpret_cast<decltype (&(::Name))> (mcr_dll_sym (csfle, #Name));   \
   if (!Name) {                                                              \
      fprintf (                                                              \
         stderr,                                                             \
         "Failed to load required symbol [%s] from the given csfle library", \
         #Name);                                                             \
      return 4;                                                              \
   }                                                                         \
   static_assert (true, "")

   LOAD_SYM (mongo_crypt_v1_status_create);
   LOAD_SYM (mongo_crypt_v1_status_destroy);
   LOAD_SYM (mongo_crypt_v1_status_get_explanation);
   LOAD_SYM (mongo_crypt_v1_lib_create);
   LOAD_SYM (mongo_crypt_v1_lib_destroy);
   LOAD_SYM (mongo_crypt_v1_get_version_str);
   LOAD_SYM (mongo_crypt_v1_query_analyzer_create);
   LOAD_SYM (mongo_crypt_v1_query_analyzer_destroy);
   LOAD_SYM (mongo_crypt_v1_analyze_query);
   LOAD_SYM (mongo_crypt_v1_bson_free);

   fprintf (stderr,
            "Loaded csfle library [%s]: %s\n",
            csfle_path,
            mongo_crypt_v1_get_version_str ());

   // Read from stdin
   std::stringstream strm;
   strm << std::cin.rdbuf ();
   std::string input_json = strm.str ();

   bson_error_t error;
   bson_t *input = bson_new_from_json (
      reinterpret_cast<const uint8_t *> (input_json.c_str ()),
      static_cast<ssize_t> (input_json.size ()),
      &error);
   if (!input) {
      std::cerr << "Failed to read JSON data: " << error.message << '\n';
      return 5;
   }
   auto del_bson = DEFER ({ bson_destroy (input); });

   bson_iter_t find_db;
   if (!bson_iter_init_find (&find_db, input, "$db")) {
      fputs ("Note: Added a {\"$db\": \"unspecified\"} field to the document "
             "root. This field is required for csfle.\n",
             stderr);
      BSON_APPEND_UTF8 (input, "$db", "unspecified");
   }

   auto status = mongo_crypt_v1_status_create ();
   auto del_status = DEFER ({ mongo_crypt_v1_status_destroy (status); });

   auto lib = mongo_crypt_v1_lib_create (status);
   if (!lib) {
      fprintf (stderr,
               "Failed to lib_create for csfle: %s\n",
               mongo_crypt_v1_status_get_explanation (status));
      return 6;
   }
   auto del_lib = DEFER ({ mongo_crypt_v1_lib_destroy (lib, nullptr); });

   auto qa = mongo_crypt_v1_query_analyzer_create (lib, status);
   if (!qa) {
      fprintf (stderr,
               "Failed to create a query analyzer for csfle: %s\n",
               mongo_crypt_v1_status_get_explanation (status));
      return 7;
   }
   auto del_qa = DEFER ({ mongo_crypt_v1_query_analyzer_destroy (qa); });

   uint32_t data_len = input->len;
   uint8_t *data_ptr = mongo_crypt_v1_analyze_query (
      qa,
      bson_get_data (input),
      doc_ns,
      static_cast<std::uint32_t> (strlen (doc_ns)),
      &data_len,
      status);
   if (!data_ptr) {
      fprintf (stderr,
               "Failed to analyze the given query: %s\n",
               mongo_crypt_v1_status_get_explanation (status));
      return 8;
   }
   auto del_data = DEFER ({ mongo_crypt_v1_bson_free (data_ptr); });

   bson_t *output = bson_new_from_data (data_ptr, data_len);
   assert (output);
   auto del_output = DEFER ({ bson_destroy (output); });

   auto s = bson_as_canonical_extended_json (output, nullptr);
   fputs (s, stdout);
   bson_free (s);

   print_field_info (output, "");

   return 0;
}
} // namespace

int
main (int argc, const char **argv)
{
   try {
      return do_main (argc, argv);
   } catch (const std::exception &e) {
      std::cerr << "Unhandled exception: " << e.what () << '\n';
      return 2;
   }
}
