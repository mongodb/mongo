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

gcp_connection::gcp_connection(const std::string &bucket_name)
    : _gcp_client(google::cloud::storage::Client()), _bucket_name(bucket_name)
{
}

// Builds a list of object names from the bucket.
int
gcp_connection::list_objects(std::vector<std::string> &objects) const
{
    return 0;
}

// Puts an object into a google cloud bucket.
int
gcp_connection::put_object(const std::string &object_key, const std::string &file_name) const
{
    return 0;
}

// Deletes an object from google cloud bucket.
int
gcp_connection::delete_object(const std::string &object_key) const
{
    return 0;
}

// Retrieves an object from the google cloud bucket.
int
gcp_connection::get_object(const std::string &object_key, const std::string &path) const
{
    return 0;
}

// Checks whether an object with the given key exists in the google cloud bucket and also retrieves
// size of the object.
int
gcp_connection::object_exists(
  const std::string &object_key, bool &exists, size_t &object_size) const
{
    return 0;
}

// Checks whether the google cloud bucket is accessible to us or not.
int
gcp_connection::bucket_exists(bool &exists) const
{
    return 0;
}
