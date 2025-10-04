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
#pragma once

// Includes necessary for connection
#include <azure/core.hpp>
#include <azure/storage/blobs.hpp>

#include <string>
#include <vector>

// Mapping between HTTP response codes and corresponding errno values to be used by the Azure
// connection methods to return errno values expected by the filesystem interface.
static const std::map<Azure::Core::Http::HttpStatusCode, int32_t> to_errno = {
  {Azure::Core::Http::HttpStatusCode::NotFound, ENOENT},
  {Azure::Core::Http::HttpStatusCode::Forbidden, EACCES},
  {Azure::Core::Http::HttpStatusCode::Conflict, EBUSY},
  {Azure::Core::Http::HttpStatusCode::BadRequest, EINVAL},
  {Azure::Core::Http::HttpStatusCode::InternalServerError, EAGAIN}};

/*
 * This class represents an active connection to the Azure endpoint and allows for interaction with
 * the Azure client. The Azure cloud storage names buckets as containers and objects as blobs. The
 * azure_connection allows for the following functionality: Listing the container contents filtered
 * by a given prefix and output all or output single, puts a blob to the cloud, gets a blob from the
 * cloud, and deletes a blob from the cloud. It also can check for the existence of a unique
 * container or blob. Each azure_connection is associated with a unique azure_client with its own
 * unique container.
 */
class azure_connection {
public:
    azure_connection(const std::string &bucket_name, const std::string &bucket_prefix = "");
    int list_objects(
      const std::string &search_prefix, std::vector<std::string> &objects, bool list_single) const;
    int put_object(const std::string &object_key, const std::string &file_path) const;
    int delete_object(const std::string &object_key) const;
    int read_object(const std::string &object_key, int64_t offset, size_t len, void *buf) const;
    int object_exists(const std::string &object_key, bool &exists, size_t &object_size) const;

private:
    const std::string _bucket_name;
    const std::string _bucket_prefix;
    const Azure::Storage::Blobs::BlobContainerClient _azure_client;

    const int http_to_errno(const Azure::Core::RequestFailedException &e) const;
    int bucket_exists(bool &exists) const;
};
