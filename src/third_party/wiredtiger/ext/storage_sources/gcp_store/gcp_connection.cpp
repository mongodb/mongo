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

#include "gcp_connection.h"

#include <fstream>

namespace gcs = google::cloud::storage;
using namespace gcs;

gcp_connection::gcp_connection(const std::string &bucket_name, const std::string &prefix)
    : _gcp_client(gcs::Client()), _bucket_name(bucket_name), _bucket_prefix(prefix)
{
    // StatusOr either contains a usable BucketMetadata value or a Status object explaining why the
    // value is not present. The value's validity is checked by StatusOr::ok().
    google::cloud::StatusOr<gcs::BucketMetadata> metadata =
      _gcp_client.GetBucketMetadata(_bucket_name);

    // Check if bucket exists and is accessible.
    if (!metadata.ok())
        throw std::invalid_argument(_bucket_name + ": " + metadata.status().message());
}

// Builds a list of object names from the bucket.
int
gcp_connection::list_objects(
  std::string search_prefix, std::vector<std::string> &objects, bool list_single)
{
    // Fetch the objects from the given bucket that match with the prefix given.
    for (auto &&object_metadata :
      _gcp_client.ListObjects(_bucket_name, gcs::Prefix(_bucket_prefix + search_prefix))) {
        // Check if the current object is accessible (object exists but the user does not have
        // permissions to access)
        if (!object_metadata) {
            // This is an error, but is non-fatal. We call handle_error to print the error message
            // and then continue listing objects.
            handle_error(object_metadata.status(),
              "List of '" + object_metadata->name() +
                "' failed: " + object_metadata.status().message());
        }

        objects.push_back(object_metadata->name().substr(_bucket_prefix.length()));

        if (list_single)
            break;
    }

    return 0;
}

// Puts an object into a google cloud bucket.
int
gcp_connection::put_object(const std::string &object_key, const std::string &file_path)
{
    // Client library automatically computes a hash on the client-side to
    // verify data integrity.
    google::cloud::StatusOr<gcs::ObjectMetadata> metadata =
      _gcp_client.UploadFile(file_path, _bucket_name, _bucket_prefix + object_key);

    // Check if file has been successfully uploaded.
    if (!metadata)
        return handle_error(metadata.status(),
          "Upload of '" + _bucket_prefix + object_key + "' failed: " + metadata.status().message());

    return 0;
}

// Deletes an object from google cloud bucket.
int
gcp_connection::delete_object(const std::string &object_key)
{
    auto status = _gcp_client.DeleteObject(_bucket_name, _bucket_prefix + object_key);

    if (!status.ok())
        return handle_error(
          status, "Delete '" + _bucket_prefix + object_key + "' failed: " + status.message());

    return 0;
}

// Retrieves an object from the google cloud bucket.
int
gcp_connection::read_object(const std::string &object_key, int64_t offset, size_t len, void *buf)
{
    bool exists = false;
    size_t object_size;

    // The object_exists function will check if the given object exists and print out any error
    // messages.
    int ret = object_exists(object_key, exists, object_size);
    if (ret != 0)
        return ret;

    if (!exists) {
        std::cerr << "Object '" << object_key << "' does not exist." << std::endl;
        return ENOENT;
    }

    if (offset < 0) {
        std::cerr << "Offset " << offset << " is invalid. The offset cannot be less than zero."
                  << std::endl;
        return EINVAL;
    }

    if (offset + len > object_size) {
        std::cerr << "Length " << len << " plus offset " << offset
                  << " must not exceed the object size " << object_size << "." << std::endl;
        return EINVAL;
    }

    gcs::ObjectReadStream stream = _gcp_client.ReadObject(
      _bucket_name, _bucket_prefix + object_key, gcs::ReadFromOffset(offset));

    if (stream.bad())
        return handle_error(stream.status(),
          "Read '" + _bucket_prefix + object_key + "' failed: " + stream.status().message());

    std::istreambuf_iterator<char> begin{stream}, end;
    std::string buffer{begin, end};

    memcpy(static_cast<char *>(buf), buffer.c_str(), len);
    return 0;
}

// Checks whether an object with the given key exists in the google cloud bucket and also retrieves
// size of the object.
int
gcp_connection::object_exists(const std::string &object_key, bool &exists, size_t &object_size)
{
    object_size = 0;

    google::cloud::StatusOr<gcs::ObjectMetadata> metadata =
      _gcp_client.GetObjectMetadata(_bucket_name, _bucket_prefix + object_key);

    // Check if object exists and is accessible.
    if (metadata.ok()) {
        exists = true;
        object_size = metadata.value().size();
        return 0;
    }

    // Check if object doesn't exist.
    if (metadata.status().code() == google::cloud::StatusCode::kNotFound) {
        exists = false;
        // This is an expected response so do not fail.
        return 0;
    }

    return handle_error(metadata.status(),
      "Object exists check for '" + _bucket_prefix + object_key +
        "' failed: " + metadata.status().message());
}

// Maps Google Cloud Status code to a system error number which is returned.
// If a mapping cannot be found the Google Cloud Status code is printed and -1 is returned.
int
gcp_connection::handle_error(
  const google::cloud::Status status, const std::string &error_message) const
{
    std::cerr << error_message << std::endl;
    if (toErrno.find(status.code()) != toErrno.end())
        return (toErrno.at(status.code()));
    return -1;
}
