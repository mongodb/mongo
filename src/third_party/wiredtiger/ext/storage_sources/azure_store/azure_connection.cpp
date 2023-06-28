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
#include "azure_connection.h"

// Includes necessary for connection
#include <azure/core.hpp>
#include <azure/storage/blobs.hpp>

#include <filesystem>
#include <iostream>

azure_connection::azure_connection(const std::string &bucket_name, const std::string &bucket_prefix)
    : _azure_client(Azure::Storage::Blobs::BlobContainerClient::CreateFromConnectionString(
        std::getenv("AZURE_STORAGE_CONNECTION_STRING"), bucket_name)),
      _bucket_name(bucket_name), _bucket_prefix(bucket_prefix)
{
    // Confirm that we can access the bucket, else fail.
    bool exists;
    int ret = bucket_exists(exists);
    if (ret != 0)
        throw std::runtime_error(_bucket_name + " : Unable to access bucket.");
    if (exists == false)
        throw std::runtime_error(_bucket_name + " : No such bucket.");
}

// Build a list of all of the objects in the bucket.
int
azure_connection::list_objects(
  const std::string &search_prefix, std::vector<std::string> &objects, bool list_single) const
{
    Azure::Storage::Blobs::ListBlobsOptions blob_parameters;
    blob_parameters.Prefix = _bucket_prefix + search_prefix;
    // If list_single is true, set the maximum number of returned blobs in the list_blob_response to
    // one.
    if (list_single)
        blob_parameters.PageSizeHint = 1;

    try {
        auto list_blobs_response = _azure_client.ListBlobs(blob_parameters);
        do {
            for (const auto &blob_item : list_blobs_response.Blobs)
                objects.push_back(blob_item.Name.substr(_bucket_prefix.length()));

            // Do not traverse all the blob pages if we are only looking for one object.
            if (list_single)
                break;
            list_blobs_response.MoveToNextPage();
        } while (list_blobs_response.HasPage());
    } catch (const Azure::Core::RequestFailedException &e) {
        return http_to_errno(e);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }

    return 0;
}

// Puts an object into the cloud storage using the prefix and file name.
int
azure_connection::put_object(const std::string &object_key, const std::string &file_path) const
{
    auto blob_client = _azure_client.GetBlockBlobClient(_bucket_prefix + object_key);
    // UploadFrom returns a UploadBlockBlobFromResult object describing the state of the updated
    // block blob on success and throws an exception on failure.
    try {
        blob_client.UploadFrom(file_path);
    } catch (const Azure::Core::RequestFailedException &e) {
        return http_to_errno(e);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
    return 0;
}

// Delete an object in the bucket given the object name.
int
azure_connection::delete_object(const std::string &object_key) const
{
    std::string obj = _bucket_prefix + object_key;

    auto object_client = _azure_client.GetBlobClient(obj);

    // Delete will delete the blob if it exists or throw an exception on failure.
    try {
        object_client.Delete();
    } catch (const Azure::Core::RequestFailedException &e) {
        return http_to_errno(e);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
    return 0;
}

// Reads an object and outputs the content into a buffer.
int
azure_connection::read_object(
  const std::string &object_key, int64_t offset, size_t len, void *buf) const
{
    auto blob_client = _azure_client.GetBlockBlobClient(_bucket_prefix + object_key);

    // GetProperties returns a BlobProperties object containing the blob size on success
    // and throws an exception on failure.
    try {
        blob_client.GetProperties();
    } catch (const Azure::Core::RequestFailedException &e) {
        return http_to_errno(e);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }

    auto blob_properties = blob_client.GetProperties().Value;

    // Checks whether the offset is before the start of the blob.
    if (offset < 0 || offset > blob_properties.BlobSize) {
        std::cerr << "Invalid argument: Offset!" << std::endl;
        return -1;
    }

    // Checks whether the offset and length is greater than the blob size.
    if (offset + len > blob_properties.BlobSize) {
        std::cerr << "Reading past end of file!" << std::endl;
        return -1;
    }

    // Utilise the inbuilt DownloadTo options to avoid having to store the blob's content
    // in memory to save space.
    Azure::Core::Http::HttpRange range;
    range.Length = len;
    range.Offset = offset;

    Azure::Storage::Blobs::DownloadBlobToOptions options;
    options.Range = range;

    // Downloads the content of the blob with the specified length to the provided buffer.
    blob_client.DownloadTo(static_cast<uint8_t *>(buf), len, options);

    return 0;
}

// Check if an object exists in the bucket and find the object size.
int
azure_connection::object_exists(
  const std::string &object_key, bool &exists, size_t &object_size) const
{
    exists = false;
    std::string obj = _bucket_prefix + object_key;
    object_size = 0;
    Azure::Storage::Blobs::ListBlobsOptions blob_parameters;

    blob_parameters.Prefix = _bucket_prefix + object_key;
    auto list_blob_response = _azure_client.ListBlobs(blob_parameters);
    do {
        for (const auto &blob_item : list_blob_response.Blobs) {
            // Check if object exists.
            if (blob_item.Name.compare(obj) == 0) {
                // Check if object is deleted and has not been cleared by garbage collection.
                if (blob_item.IsDeleted) {
                    return -1;
                }
                object_size = blob_item.BlobSize;
                exists = true;
                break;
            }
        }
        list_blob_response.MoveToNextPage();
    } while (list_blob_response.HasPage());
    return 0;
}

// Check if a bucket exists in the Azure storage source.
int
azure_connection::bucket_exists(bool &exists) const
{
    exists = false;

    auto service_client = Azure::Storage::Blobs::BlobServiceClient::CreateFromConnectionString(
      std::getenv("AZURE_STORAGE_CONNECTION_STRING"));

    // Get list of containers associated with the class Azure client.
    auto list_container_response = service_client.ListBlobContainers();

    for (const auto container_item : list_container_response.BlobContainers) {
        // Check if bucket exists.
        if (container_item.Name.compare(_bucket_name) == 0) {
            // Check if bucket is deleted and has not been cleared by garbage collection.
            if (container_item.IsDeleted) {
                return -1;
            }
            exists = true;
            break;
        }
    }
    return 0;
}

// Return a system error code corresponding to the HTTP code returned by
// the Azure SDK. If no such mapping is defined, return -1.
const int
azure_connection::http_to_errno(const Azure::Core::RequestFailedException &e) const
{
    std::cerr << e.ReasonPhrase << std::endl;
    if (to_errno.find(e.StatusCode) != to_errno.end())
        return to_errno.at(e.StatusCode);
    return -1;
}
