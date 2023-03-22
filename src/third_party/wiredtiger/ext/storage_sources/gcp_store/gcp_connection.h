
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

#include "google/cloud/storage/client.h"
#include <iostream>
#include <string>
#include <vector>

// Mapping between Google Status codes and corresponding system error numbers to be used by the GCP
// connection methods to return system error numbers expected by the filesystem interface.
static const std::map<google::cloud::StatusCode, int32_t> toErrno = {
  {google::cloud::StatusCode::kUnknown, EAGAIN},
  {google::cloud::StatusCode::kInvalidArgument, EINVAL},
  {google::cloud::StatusCode::kNotFound, ENOENT},
  {google::cloud::StatusCode::kAlreadyExists, EBUSY},
  {google::cloud::StatusCode::kPermissionDenied, EACCES}};
class gcp_connection {
public:
    gcp_connection(const std::string &bucket_name, const std::string &prefix);
    int list_objects(
      std::string search_prefix, std::vector<std::string> &objects, bool list_single);
    int put_object(const std::string &object_key, const std::string &file_path);
    int delete_object(const std::string &object_key);
    int object_exists(const std::string &object_key, bool &exists, size_t &object_size);
    int read_object(const std::string &object_key, int64_t offset, size_t len, void *buf);
    int handle_error(const google::cloud::Status status, const std::string &error_message) const;

    ~gcp_connection() = default;

private:
    google::cloud::storage::Client _gcp_client;
    const std::string _bucket_name;
    const std::string _bucket_prefix;
};
