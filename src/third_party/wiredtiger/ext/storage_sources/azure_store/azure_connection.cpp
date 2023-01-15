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

azure_connection::azure_connection(const std::string &bucket_name, const std::string &obj_prefix)
    : _azure_client(Azure::Storage::Blobs::BlobContainerClient::CreateFromConnectionString(
        std::getenv("AZURE_STORAGE_CONNECTION_STRING"), bucket_name)),
      _bucket_name(bucket_name), _object_prefix(obj_prefix)
{
}

int
azure_connection::list_objects(std::vector<std::string> &objects) const
{
    return 0;
}

int
azure_connection::put_object(const std::string &file_name) const
{
    return 0;
}

int
azure_connection::delete_object() const
{
    return 0;
}

int
azure_connection::get_object(const std::string &path) const
{
    return 0;
}
