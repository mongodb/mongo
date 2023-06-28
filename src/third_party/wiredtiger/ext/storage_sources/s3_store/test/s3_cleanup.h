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
#ifndef S3_CLEANUP_H
#define S3_CLEANUP_H
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include "s3_connection.h"
#include <string>

class S3Cleanup {
public:
    S3Cleanup(S3Connection &conn, const std::string &prefix, const std::string &fileName)
        : _conn(conn), _prefix(prefix), _fileName(fileName), _totalObjects(0)
    {
    }
    void
    setTotalObjects(int totalObjects)
    {
        _totalObjects = totalObjects;
    }

    ~S3Cleanup()
    {
        // Delete objects and file at end of test.
        for (int i = 0; i < _totalObjects; i++) {
            REQUIRE(_conn.DeleteObject(_prefix + std::to_string(i) + ".txt") == 0);
        }
        std::remove(_fileName.c_str());
    }

private:
    S3Connection &_conn;
    const std::string &_prefix;
    const std::string &_fileName;
    int _totalObjects;
};
#endif // S3_CLEANUP_H
