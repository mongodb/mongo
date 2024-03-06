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

#include <sys/mman.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <vector>

#include "wiredtiger.h"
extern "C" {
#include "wt_internal.h"
}

#include "model/util.h"

namespace model {

/*
 * config_map::from_string --
 *     Parse config map from a string.
 */
config_map
config_map::from_string(const char *str, const char **end)
{
    std::ostringstream key_buf, value_buf;
    bool in_key, in_quotes;
    config_map m;
    const char *p;

    in_key = true;
    in_quotes = false;

    for (p = str; *p != '\0' && (in_quotes || *p != ')'); p++) {
        char c = *p;

        /* Handle quotes. */
        if (in_quotes) {
            if (c == '\"') {
                in_quotes = false;
                continue;
            }
        } else if (c == '\"') {
            in_quotes = true;
            continue;
        }

        if (in_key) {
            /* Handle keys. */
            if (!in_quotes && c == '=')
                in_key = false;
            else if (!in_quotes && c == ',') {
                /* Empty value. */
                if (!key_buf.str().empty()) {
                    m._map[key_buf.str()] = "";
                    key_buf.str("");
                }
            } else
                key_buf << c;
        } else {
            /* Handle nested config maps. */
            if (!in_quotes && c == '(') {
                if (value_buf.str() != "")
                    throw model_exception("Invalid nested configuration string");
                m._map[key_buf.str()] = std::make_shared<config_map>(from_string(p + 1, &p));
                if (*p != ')')
                    throw model_exception("Invalid nesting within a configuration string");
                key_buf.str("");
                in_key = true;
            }
            /* Handle arrays. */
            else if (!in_quotes && c == '[') {
                if (value_buf.str() != "")
                    throw model_exception("Invalid array in the configuration string");
                m._map[key_buf.str()] = parse_array(p + 1, &p);
                if (*p != ']')
                    throw model_exception("Unmatched '[' in a configuration string");
                key_buf.str("");
                in_key = true;
            }
            /* Handle regular values. */
            else if (!in_quotes && c == ',') {
                m._map[key_buf.str()] = value_buf.str();
                key_buf.str("");
                value_buf.str("");
                in_key = true;
            }
            /* Else we just get the next character. */
            else
                value_buf << c;
        }
    }

    /* Handle the last value. */
    if (in_quotes)
        throw model_exception("Unmatched quotes within a configuration string");
    if (!in_key)
        m._map[key_buf.str()] = value_buf.str();

    /* Handle the end of a nested map. */
    if (end == NULL) {
        if (*p != '\0')
            throw model_exception("Invalid configuration string");
    } else
        *end = p;

    return m;
}

/*
 * config_map::parse_array --
 *     Parse an array.
 */
std::shared_ptr<std::vector<std::string>>
config_map::parse_array(const char *str, const char **end)
{
    std::shared_ptr<std::vector<std::string>> v = std::make_shared<std::vector<std::string>>();

    std::ostringstream buf;
    bool in_quotes = false;
    const char *p;

    for (p = str; *p != '\0' && (in_quotes || *p != ']'); p++) {
        char c = *p;

        /* Handle quotes. */
        if (in_quotes) {
            if (c == '\"') {
                in_quotes = false;
                continue;
            }
        } else if (c == '\"') {
            in_quotes = true;
            continue;
        }

        /* We found the end of the value. */
        if (c == ',') {
            std::string s = buf.str();
            if (!s.empty()) {
                v->push_back(s);
                buf.str("");
            }
        }
        /* Else we just get the next character. */
        else
            buf << c;
    }

    /* Handle the last value. */
    if (in_quotes)
        throw model_exception("Unmatched quotes within a configuration string");
    std::string last = buf.str();
    if (!last.empty())
        v->push_back(last);

    /* Handle the end of the array. */
    if (end != nullptr)
        *end = p;
    return v;
}

/*
 * config_map::merge --
 *     Merge two config maps.
 */
config_map
config_map::merge(const config_map &a, const config_map &b)
{
    config_map m;
    std::merge(a._map.begin(), a._map.end(), b._map.begin(), b._map.end(),
      std::inserter(m._map, m._map.begin()));
    return m;
}

/*
 * shared_memory::shared_memory --
 *     Create a shared memory object of the given size.
 */
shared_memory::shared_memory(size_t size) : _data(nullptr), _size(size)
{
    /* Create a unique name using the PID and the memory offset of this object. */
    std::ostringstream name_stream;
    name_stream << "/wt-" << getpid() << "-" << this;
    _name = name_stream.str();

    /* Create the shared memory object. */
    int fd = shm_open(_name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::ostringstream err;
        err << "Failed to open shared memory object \"" << _name << "\": " << strerror(errno)
            << " (" << errno << ")";
        throw std::runtime_error(err.str());
    }

    /* Unlink the object from the namespace, as it is no longer needed. */
    int ret = shm_unlink(_name.c_str());
    if (ret < 0) {
        (void)close(fd);
        std::ostringstream err;
        err << "Failed to unlink shared memory object \"" << _name << "\": " << strerror(errno)
            << " (" << errno << ")";
        _data = nullptr;
        throw std::runtime_error(err.str());
    }

    /* Set the initial size. */
    ret = ftruncate(fd, (wt_off_t)size);
    if (ret < 0) {
        (void)close(fd);
        throw std::runtime_error("Setting shared memory size failed");
    }

    /* Map the shared memory. */
    _data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (_data == MAP_FAILED) {
        (void)close(fd);
        std::ostringstream err;
        err << "Failed to map shared memory object \"" << _name << "\": " << strerror(errno) << " ("
            << errno << ")";
        _data = nullptr;
        throw std::runtime_error(err.str());
    }

    /* Close the handle. */
    ret = close(fd);
    if (ret < 0) {
        (void)munmap(_data, _size);
        std::ostringstream err;
        err << "Failed to close shared memory object \"" << _name << "\": " << strerror(errno)
            << " (" << errno << ")";
        _data = nullptr;
        throw std::runtime_error(err.str());
    }

    /* Zero the object. */
    memset(_data, 0, _size);
}

/*
 * shared_memory::~shared_memory --
 *     Free the memory object.
 */
shared_memory::~shared_memory()
{
    if (_data == nullptr)
        return;

    if (munmap(_data, _size) < 0) {
        /* Cannot throw an exception from out of a destructor, so just fail. */
        std::cerr << "PANIC: Failed to unmap shared memory object \"" << _name
                  << "\": " << strerror(errno) << " (" << errno << ")" << std::endl;
        abort();
    }
}

/*
 * parse_uint64 --
 *     Parse the string into a number. Throw an exception on error.
 */
uint64_t
parse_uint64(const char *str, char **end)
{
    if (str == nullptr || str[0] == '\0')
        throw std::runtime_error("Cannot parse a number");

    char *p = nullptr;
    uint64_t r =
      (uint64_t)std::strtoull(str, &p, 0 /* automatically detect "0x" for hex numbers */);
    if (end != nullptr)
        *end = p;
    if (str == p || (end == nullptr && p[0] != '\0'))
        throw std::runtime_error("Cannot parse a number");

    return r;
}

/*
 * wt_list_tables --
 *     Get the list of WiredTiger tables.
 */
std::vector<std::string>
wt_list_tables(WT_CONNECTION *conn)
{
    int ret;
    std::vector<std::string> tables;

    WT_SESSION *session;
    ret = conn->open_session(conn, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a session: ", ret);
    wiredtiger_session_guard session_guard(session);

    WT_CURSOR *cursor;
    ret = session->open_cursor(session, WT_METADATA_URI, NULL, NULL, &cursor);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a metadata cursor: ", ret);
    wiredtiger_cursor_guard cursor_guard(cursor);

    const char *key;
    while ((ret = cursor->next(cursor)) == 0) {
        /* Get the key. */
        if ((ret = cursor->get_key(cursor, &key)) != 0)
            throw wiredtiger_exception("Cannot get key: ", ret);

        if (strncmp(key, "table:", 6) == 0)
            tables.push_back(key + 6);
    }

    return tables;
}

} /* namespace model */
