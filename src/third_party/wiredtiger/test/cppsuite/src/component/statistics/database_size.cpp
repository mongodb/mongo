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

#include "database_size.h"

#include "src/common/logger.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {

static std::string
collection_name_to_file_name(const std::string &collection_name)
{
    /* Strip out the URI prefix. */
    const size_t colon_pos = collection_name.find(':');
    testutil_assert(colon_pos != std::string::npos);
    const auto stripped_name = collection_name.substr(colon_pos + 1);

    /* Now add the directory and file extension. */
    return (std::string(DEFAULT_DIR) + "/" + stripped_name + ".wt");
}

database_size::database_size(configuration &config, const std::string &name, database &database)
    : statistics(config, name, -1), _database(database)
{
#ifdef _WIN32
    Logger::log_msg("Database size checking is not implemented on Windows", LOG_ERROR);
#endif
}

void
database_size::check(scoped_cursor &)
{
#ifndef _WIN32
    const auto file_names = get_file_names();
    size_t db_size = get_db_size();
    logger::log_msg(LOG_TRACE, "Current database size is " + std::to_string(db_size) + " bytes");

    if (db_size > max) {
        const std::string error_string =
          "metrics_monitor: Database size limit exceeded during test! Limit: " +
          std::to_string(max) + " db size: " + std::to_string(db_size);
        testutil_die(-1, error_string.c_str());
    }
#endif
}

std::string
database_size::get_value_str(scoped_cursor &)
{
    return std::to_string(get_db_size());
}

size_t
database_size::get_db_size() const
{
    const auto file_names = get_file_names();
    size_t db_size = 0;

    for (const auto &name : file_names) {
        struct stat sb;
        if (stat(name.c_str(), &sb) == 0) {
            db_size += sb.st_size;
            logger::log_msg(LOG_TRACE, name + " was " + std::to_string(sb.st_size) + " bytes");
        } else
            /* The only good reason for this to fail is if the file hasn't been created yet. */
            testutil_assert(errno == ENOENT);
    }

    return db_size;
}

const std::vector<std::string>
database_size::get_file_names() const
{
    std::vector<std::string> file_names;
    for (const auto &name : _database.get_collection_names())
        file_names.push_back(collection_name_to_file_name(name));

    /* Add WiredTiger internal tables. */
    file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_HS_FILE);
    file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_METAFILE);

    return (file_names);
}
} // namespace test_harness
