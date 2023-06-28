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
#include <catch2/catch.hpp>

#include "azure_connection.h"
#include <fstream>
#include <string>

static std::string
create_file(const std::string &object_name, const std::string &payload)
{
    std::ofstream file(object_name + ".txt");
    file << payload;
    file.close();
    return object_name + ".txt";
}

// Concatenates a random suffix to the prefix being used for the test object keys. Example of
// generated test prefix: "azuretest/unit/2023-31-01-16-34-10/623843294--".
static std::string
randomize_test_prefix()
{
    char time_str[100];
    std::time_t t = std::time(nullptr);

    REQUIRE(std::strftime(time_str, sizeof(time_str), "%F-%H-%M-%S", std::localtime(&t)) != 0);

    std::string bucket_prefix("azuretest/unit/"); // To be concatenated with a random string.

    bucket_prefix += time_str;

    // Create a random device and use it to generate a random seed to initialize the generator.
    std::random_device my_random_device;
    unsigned seed = my_random_device();
    std::default_random_engine my_random_engine(seed);

    bucket_prefix += '/' + std::to_string(my_random_engine());
    bucket_prefix += "--";

    return bucket_prefix;
}

TEST_CASE("Testing Azure Connection Class", "azure-connection")
{
    auto azure_client = Azure::Storage::Blobs::BlobContainerClient::CreateFromConnectionString(
      std::getenv("AZURE_STORAGE_CONNECTION_STRING"), "myblobcontainer1");
    bool exists = false;
    size_t object_size = 0;

    std::string bucket_prefix = randomize_test_prefix();

    azure_connection conn = azure_connection("myblobcontainer1", bucket_prefix);
    azure_connection conn_bad = azure_connection("myblobcontainer1", "bad_prefix_");

    std::vector<std::pair<std::string, std::string>> blob_objects;

    const std::string object_name = "test_object";
    const std::string object_name_2 = "test_object_2";
    const std::string non_exist_object_key = "test_non_exist";

    // Payloads for blobs uploaded to container.
    const std::string payload = "payload";
    const std::string payload_2 = "Testing offset and substring";
    blob_objects.push_back(std::make_pair(object_name, payload));
    blob_objects.push_back(std::make_pair(object_name_2, payload_2));

    // Add objects into the container.
    for (auto pair : blob_objects) {
        auto blob_client = azure_client.GetBlockBlobClient(bucket_prefix + pair.first);
        blob_client.UploadFrom(create_file(pair.first, pair.second));
    }

    SECTION("Check Azure connection constructor.", "[azure-connection]")
    {
        REQUIRE_THROWS_WITH(azure_connection("Bad_bucket", ""), "Bad_bucket : No such bucket.");
    }

    SECTION("Check object exists in Azure.", "[azure-connection]")
    {
        // Object exists so there should be 1 object.
        REQUIRE(conn.object_exists(object_name, exists, object_size) == 0);
        REQUIRE(exists == true);
        auto blob_client = azure_client.GetBlockBlobClient(bucket_prefix + object_name);
        auto blob_properties = blob_client.GetProperties();
        size_t blob_size = blob_properties.Value.BlobSize;
        REQUIRE(object_size == blob_size);

        // Object does not exist so there should be 0 objects.
        REQUIRE(conn.object_exists(non_exist_object_key, exists, object_size) == 0);
        REQUIRE(exists == false);
    }

    SECTION("List Azure objects under the test bucket.", "[azure-connection]")
    {
        std::vector<std::string> objects;

        // No matching objects. Object size should be 0.
        REQUIRE(conn.list_objects(non_exist_object_key, objects, false) == 0);
        REQUIRE(objects.size() == 0);

        // No matching objects with list single functionality. Object size should be 0.
        REQUIRE(conn.list_objects(non_exist_object_key, objects, true) == 0);
        REQUIRE(objects.size() == 0);

        // List all objects. Object size should be 2.
        REQUIRE(conn.list_objects(object_name, objects, false) == 0);
        REQUIRE(objects.size() == blob_objects.size());
        objects.clear();

        // List single. Object size should be 1.
        REQUIRE(conn.list_objects(object_name, objects, true) == 0);
        REQUIRE(objects.size() == 1);
        objects.clear();
    }

    SECTION("Test delete functionality for Azure.", "[azure-connection]")
    {
        auto blob_client = azure_client.GetBlockBlobClient(bucket_prefix + object_name + "1");
        std::vector<std::string> container;

        blob_client.UploadFrom(create_file(object_name + "1", payload));

        // Test that an object can be deleted.
        REQUIRE(conn.delete_object(object_name + "1") == 0);

        // Test that removing an object that doesn't exist returns a ENOENT.
        REQUIRE(conn.delete_object(non_exist_object_key) == ENOENT);

        auto list_blob_response = azure_client.ListBlobs();
        for (const auto blob_item : list_blob_response.Blobs) {
            container.push_back(blob_item.Name);
        }

        // Check that the object does not exist in the container.
        REQUIRE(std::find(container.begin(), container.end(), bucket_prefix + object_name + "1") ==
          std::end(container));
    }

    SECTION("Check put functionality in Azure.", "[azure-connection]")
    {
        const std::string path = "./" + create_file(object_name, payload);
        std::vector<std::string> container;

        REQUIRE(conn.put_object(object_name + "1", path) == 0);

        auto blob_client = azure_client.GetBlockBlobClient(bucket_prefix + object_name + "1");

        // Test that putting an object that doesn't exist locally returns -1.
        REQUIRE(conn.put_object(non_exist_object_key, non_exist_object_key + ".txt") == -1);

        auto list_blob_response = azure_client.ListBlobs();
        for (const auto blob_item : list_blob_response.Blobs) {
            container.push_back(blob_item.Name);
        }

        // Check that the object exists in the container.
        REQUIRE(std::find(container.begin(), container.end(), bucket_prefix + object_name + "1") !=
          std::end(container));
        // Check that when putting an object fails that object is not in the container.
        REQUIRE(std::find(container.begin(), container.end(),
                  bucket_prefix + non_exist_object_key) == std::end(container));

        blob_client.Delete();
    }

    SECTION("Check read functionality in Azure.", "[azure-connection]")
    {
        char buffer[1024];

        // Test reading whole file.
        REQUIRE(conn.read_object(object_name, 0, payload.length(), buffer) == 0);
        REQUIRE(payload.compare(buffer) == 0);
        memset(buffer, 0, 1024);

        // Check that read works from the first ' ' to the end.
        const int str_len = payload_2.length() - payload_2.find(" ");
        REQUIRE(conn.read_object(object_name_2, payload_2.find(" "), str_len, buffer) == 0);
        REQUIRE(payload_2.substr(payload_2.find(" ")).compare(buffer) == 0);
        memset(buffer, 0, 1024);

        // Test overflow on positive offset but past EOF.
        REQUIRE(conn.read_object(object_name, 1, 1000, buffer) == -1);

        // Test overflow on negative offset but past EOF.
        REQUIRE(conn.read_object(object_name, -1, 1000, buffer) == -1);

        // Test overflow with negative offset.
        REQUIRE(conn.read_object(object_name, -1, 12, buffer) == -1);

        // Tests that reading a existing object but wrong prefix returns a ENOENT.
        REQUIRE(conn_bad.read_object(object_name, 0, 1, buffer) == ENOENT);

        // Test that reading a non existent object returns a ENOENT.
        REQUIRE(conn.read_object(non_exist_object_key, 0, 1, buffer) == ENOENT);
    }

    // Delete the objects we added earlier so we have no objects in the container.
    for (auto pair : blob_objects) {
        auto blob_client = azure_client.GetBlockBlobClient(bucket_prefix + pair.first);
        blob_client.Delete();
    }

    // Sanity check that nothing exists.
    Azure::Storage::Blobs::ListBlobsOptions blob_parameters;
    blob_parameters.Prefix = bucket_prefix;
    REQUIRE(azure_client.ListBlobs(blob_parameters).Blobs.size() == 0);
}
