/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define CATCH_CONFIG_MAIN
#include "gcp_connection.h"

#include <catch2/catch.hpp>

namespace gcs = google::cloud::storage;
using namespace gcs;

static std::string
create_file(const std::string file_name, const std::string payload)
{
    std::ofstream file(file_name);
    file << payload;
    file.close();
    return file_name;
}

static auto
upload_file(gcs::Client client, std::string bucket_name, const std::string bucket_prefix,
  const std::string file_name, const std::string object_name)
{
    auto metadata = client.UploadFile("./" + file_name, bucket_name, bucket_prefix + object_name);

    return metadata;
}

static bool
file_exists_in_bucket(gcs::Client client, std::string bucket_name, const std::string bucket_prefix,
  const std::string object_name)
{
    auto metadata = client.GetObjectMetadata(bucket_name, bucket_prefix + object_name);

    // Metadata ok implies that the file is present.
    return metadata.ok();
}

static int
num_objects_in_bucket(gcs::Client client, std::string bucket_name, const std::string bucket_prefix)
{
    auto objects_iterator = client.ListObjects(bucket_name, gcs::Prefix(bucket_prefix));
    return std::distance(objects_iterator.begin(), objects_iterator.end());
}

// Concatenates a random suffix to the prefix being used for the test object keys. Example of
// generated test prefix: "gcptest/unit/2022-31-01-16-34-10/623843294/".
static std::string
generate_test_prefix()
{
    std::string prefix = "gcptest/unit/";
    char time_str[100];
    std::time_t t = std::time(nullptr);

    REQUIRE(std::strftime(time_str, sizeof(time_str), "%F-%H-%M-%S", std::localtime(&t)) != 0);

    prefix += time_str;

    // Create a random device and use it to generate a random seed to initialize the generator.
    std::random_device my_random_device;
    unsigned seed = my_random_device();
    std::default_random_engine my_random_engine(seed);

    prefix += "/" + std::to_string(my_random_engine()) + "/";

    return prefix;
}

TEST_CASE("Testing class gcpConnection", "gcp-connection")
{
    std::string test_bucket_name = "unit_testing_gcp";

    // Set up the test environment.
    std::string test_bucket_prefix = generate_test_prefix();
    gcp_connection conn(test_bucket_name, test_bucket_prefix);

    const std::string object_name = "test_object";
    const std::string file_name = object_name + ".txt";
    const std::string non_existant_object_name = "test_non_exist";
    const std::string non_existant_file_name = non_existant_object_name + ".txt";
    std::vector<std::string> objects;

    gcs::Client client = gcs::Client();

    std::string payload = "Test payload :)";
    create_file(file_name, payload);

    SECTION("Simple list test", "[gcp-connection]")
    {
        // Search prefix is not used in this unit test, empty string is provided.
        const std::string search_prefix = "";

        // No matching objects. Objects list should be empty.
        REQUIRE(conn.list_objects(search_prefix, objects, false) == 0);
        REQUIRE(objects.empty());

        // No matching objects with list_single. Objects list should be empty.
        REQUIRE(conn.list_objects(search_prefix, objects, true) == 0);
        REQUIRE(objects.empty());

        // Upload 1 file to the bucket and test list_objects function.
        // List_objects should return an objects list with size 1.
        REQUIRE(
          upload_file(client, test_bucket_name, test_bucket_prefix, file_name, object_name).ok());
        REQUIRE(file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));
        REQUIRE(conn.list_objects(search_prefix, objects, false) == 0);
        REQUIRE(objects.size() == 1);
        objects.clear();

        // Delete the object we uploaded and test list_objects function.
        // List_objects should return an empty objects list.
        client.DeleteObject(test_bucket_name, test_bucket_prefix + object_name);
        REQUIRE(conn.list_objects(search_prefix, objects, false) == 0);
        REQUIRE(objects.empty());
        objects.clear();

        // Upload multiple files and test list.
        const int32_t total_objects = 20;
        for (int i = 0; i < total_objects; i++) {
            std::string multi_file_name = object_name + std::to_string(i);
            REQUIRE(
              upload_file(client, test_bucket_name, test_bucket_prefix, file_name, multi_file_name)
                .ok());
        }

        // List objects should return a list with size total_objects.
        REQUIRE(conn.list_objects(search_prefix, objects, false) == 0);
        REQUIRE(objects.size() == total_objects);
        objects.clear();

        // Test if list single correctly returns one object.
        REQUIRE(conn.list_objects(search_prefix, objects, true) == 0);
        REQUIRE(objects.size() == 1);
        objects.clear();

        // Delete all object we uploaded.
        for (int i = 0; i < total_objects; i++) {
            client.DeleteObject(
              test_bucket_name, test_bucket_prefix + object_name + std::to_string(i));
        }

        // Bucket should be cleared.
        REQUIRE(conn.list_objects(search_prefix, objects, false) == 0);
        REQUIRE(objects.empty());
    }

    SECTION("Simple put test", "[gcp-connection]")
    {

        // Upload a file that does not exist locally - should fail.
        REQUIRE(conn.put_object(non_existant_object_name, non_existant_file_name) == ENOENT);

        // Check number of files with the given prefix that are currently in the bucket.
        REQUIRE(num_objects_in_bucket(client, test_bucket_name, test_bucket_prefix) == 0);

        // Upload a test file.
        REQUIRE(conn.put_object(object_name, "./" + file_name) == 0);

        // Check the bucket contains the uploaded file.
        REQUIRE(file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));

        // Delete the uploaded file.
        client.DeleteObject(test_bucket_name, test_bucket_prefix + object_name);
    }

    SECTION("Simple delete test", "[gcp-connection]")
    {

        // Delete a file that does not exist in the bucket - should fail.
        REQUIRE(conn.delete_object(non_existant_object_name) == ENOENT);

        // Upload a test file.
        REQUIRE(
          upload_file(client, test_bucket_name, test_bucket_prefix, file_name, object_name).ok());
        REQUIRE(file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));

        // Delete the uploaded file.
        REQUIRE(conn.delete_object(object_name) == 0);

        // Check that the file has been deleted.
        REQUIRE_FALSE(
          file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));
    }

    SECTION("Simple object exists test", "[gcp-connection]")
    {
        bool exists;
        size_t size;

        REQUIRE(conn.object_exists(object_name, exists, size) == 0);
        REQUIRE(exists == false);
        REQUIRE(size == 0);

        // Upload a test file.
        REQUIRE(
          upload_file(client, test_bucket_name, test_bucket_prefix, file_name, object_name).ok());
        REQUIRE(num_objects_in_bucket(client, test_bucket_name, test_bucket_prefix) == 1);

        // Check the bucket contains the uploaded file.
        REQUIRE(conn.object_exists(object_name, exists, size) == 0);
        REQUIRE(exists);
        REQUIRE(size != 0);

        // Delete the uploaded file.
        auto dl_metadata = client.DeleteObject(test_bucket_name, test_bucket_prefix + object_name);
        REQUIRE(dl_metadata.ok());

        // Check that the file has been deleted.
        REQUIRE_FALSE(
          file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));

        // Check for a file that is not in the bucket.
        // Object exists should modify the exists variable and set it to false.
        REQUIRE(conn.object_exists(object_name, exists, size) == 0);
        REQUIRE(exists == false);
        REQUIRE(size == 0);
    }

    SECTION("Read tests", "[gcp-connection]")
    {
        // Upload a test file.
        REQUIRE(
          upload_file(client, test_bucket_name, test_bucket_prefix, file_name, object_name).ok());
        REQUIRE(file_exists_in_bucket(client, test_bucket_name, test_bucket_prefix, object_name));

        // Read GCP objects under the test bucket with no offset.
        char buf[1024];

        REQUIRE(conn.read_object(object_name, 0, payload.length(), buf) == 0);
        REQUIRE(payload.compare(buf) == 0);
        memset(buf, 0, 1000);

        // Read GCP objects under the test bucket with offset.
        const int str_len = payload.length() - payload.find(" ");
        REQUIRE(conn.read_object(object_name, payload.find(" "), str_len, buf) == 0);
        REQUIRE(payload.substr(payload.find(" "), str_len).compare(buf) == 0);
        memset(buf, 0, 1000);

        // Read GCP objects under the test bucket with len > file length.
        REQUIRE(conn.read_object(object_name, 0, 100000, buf) == EINVAL);

        // Read GCP objects under the test bucket with offset < 0.
        REQUIRE(conn.read_object(object_name, -5, 15, buf) == EINVAL);

        // Read GCP objects under the test bucket with offset > file length.
        REQUIRE(conn.read_object(object_name, 1000, 15, buf) == EINVAL);
    }

    // Cleanup
    // List and loop through objects with prefix.
    for (auto &&object_metadata :
      client.ListObjects(test_bucket_name, gcs::Prefix(test_bucket_prefix))) {
        // Delete the test file.
        if (object_metadata) {
            auto dl_metadata =
              client.DeleteObject(test_bucket_name, object_metadata.value().name());
            REQUIRE(dl_metadata.ok());
        }
    }
}
