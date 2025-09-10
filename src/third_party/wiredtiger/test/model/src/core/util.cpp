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
#include <iconv.h>
#include <iostream>
#include <sstream>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#include <libgen.h>
#endif

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
 * decode_utf8 --
 *     Decode a UTF-8 string into one-byte code points. Throw an exception on error.
 */
std::string
decode_utf8(const std::string &str)
{
    /*
     * The main use case for this function is to recover the exact byte sequence specified in a JSON
     * document produced by "wt printlog -u", which uses the \u notation to encode non-printable
     * bytes. For example, WiredTiger packs key 1 as byte 81 (hex), which results in the following
     * line in the output of printlog:
     *
     *     "key": "\u0081"
     *
     * Our JSON parser encodes these Unicode characters using UTF-8, so reading this key results in
     * a two-byte string C2 81. This function decodes this string back to 81, which the caller can
     * then use in data_value::unpack to get the original integer 1.
     *
     * We do this in two steps:
     *   1. Convert the UTF-8 string to a WCHAR_T array.
     *   2. Convert the WCHAR_T array to a char array.
     */

    /* Initialize the conversion. */
    iconv_t conv = iconv_open("WCHAR_T", "UTF-8");
    if (conv == (iconv_t)-1)
        throw std::runtime_error(
          "Cannot initialize charset conversion: " + std::string(wiredtiger_strerror(errno)));
    at_cleanup close_conv([conv] { (void)iconv_close(conv); });

    /* Copy the input buffer, since the conversion library needs a mutable buffer. */
    size_t src_size = str.size();
    std::vector<char> src(str.begin(), str.end());

    /* Allocate the output buffer. */
    size_t dest_size = src_size + 4; /* Big enough because we're using 1-byte code points. */
    std::vector<wchar_t> dest(dest_size, 0);

    /* Now do the actual conversion. */
    char *p_src = src.data();
    char *p_dest = (char *)dest.data();
    size_t src_bytes = src_size;
    size_t dest_bytes = dest_size * sizeof(wchar_t);
    size_t dest_bytes_start = dest_bytes;
    size_t r = iconv(conv, &p_src, &src_bytes, &p_dest, &dest_bytes);
    if (r == (size_t)-1)
        throw std::runtime_error(
          "Charset conversion failed: " + std::string(wiredtiger_strerror(errno)));
    if (src_bytes != 0)
        throw std::runtime_error(
          "Charset conversion did not decode " + std::to_string(src_bytes) + " byte(s)");

    /* Figure out how many code points were actually decoded. */
    size_t decoded_size = (dest_bytes_start - dest_bytes) / sizeof(wchar_t);

    /* Extract the byte-long code points from the WCHAR_T array into a char array. */
    std::vector<char> decoded(decoded_size, 0);
    for (size_t i = 0; i < decoded_size; i++) {
        if (dest[i] > 0xFF)
            throw std::runtime_error(
              "Not byte-long code point: " + std::to_string((uint64_t)dest[i]));
        decoded[i] = (char)dest[i];
    }

    return std::string(decoded.data(), decoded_size);
}

/*
 * directory_path --
 *     Get path to the parent directory. Throw an exception on error.
 */
std::string
directory_path(const std::string &path)
{
#if defined(__linux__) || defined(__APPLE__)
    char *buf;

    buf = strdup(path.c_str());
    if (buf == nullptr)
        throw std::runtime_error("Out of memory");

    std::string s{dirname(buf)};
    free(buf);
    return std::move(s);
#else
    throw std::runtime_error("Not implemented for this platform");
#endif
}

/*
 * executable_path --
 *     Path to the current executable. Throw an exception on error.
 */
std::string
executable_path()
{
#if defined(__linux__) || defined(__APPLE__)
    char buf[PATH_MAX];
    int ret = readlink("/proc/self/exe", buf, sizeof(buf));
    if (ret != 0)
        throw std::runtime_error("Cannot read /proc/self/exe");
    return std::string(buf);
#else
    throw std::runtime_error("Not implemented for this platform");
#endif
}

/*
 * model_library_path --
 *     Path to this library. Throw an exception on error.
 */
std::string
model_library_path()
{
#if defined(__linux__) || defined(__APPLE__)
    Dl_info info;
    if (dladdr((const void *)model_library_path, &info) == 0)
        throw std::runtime_error("Cannot find the location of the model library");
    return std::string(info.dli_fname);
#else
    throw std::runtime_error("Not implemented for this platform");
#endif
}

/*
 * parse_uint64 --
 *     Parse the string into a number. Throw an exception on error.
 */
uint64_t
parse_uint64(const char *str, const char **end)
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
 * quote --
 *     Return a string in quotes, with appropriate escaping.
 */
std::string
quote(const std::string &str)
{
    std::ostringstream out;
    out << "\"";

    for (char c : str) {
        if (c == '"')
            out << "\\\"";
        else
            out << c;
    }

    out << "\"";
    return out.str();
}

/*
 * wt_build_dir_path --
 *     Path to the WiredTiger build directory, assuming that this library is used from the build
 *     directory. Throw an exception on error.
 */
std::string
wt_build_dir_path()
{
    std::string path = model_library_path();
#if defined(__linux__)
    const std::string library_name = "libwiredtiger.so";
#elif defined(__APPLE__)
    const std::string library_name = "libwiredtiger.dylib";
#else
    throw std::runtime_error("Not implemented for this platform");
#endif

    for (path = directory_path(path); path != "." && path != "/"; path = directory_path(path)) {
        std::string s = join(path, library_name, "/");
        if (access(s.c_str(), F_OK) == 0)
            return path;
    }

    throw std::runtime_error("Could not find WiredTiger's build directory");
}

/*
 * wt_disagg_config_string --
 *     Get the config string for disaggregated storage.
 */
std::string
wt_disagg_config_string()
{
    std::string extension = wt_extension_path("page_log/palm/libwiredtiger_palm.so");

    std::ostringstream config;
    config << "precise_checkpoint=true,";
    config << "extensions=[" << extension << "],";
    /* config << "extensions=[" << extension << "=(config=\"(verbose=1)\")" << "],"; */
    config << "disaggregated=(page_log=palm,role=follower)";

    return config.str();
}

/*
 * wt_disagg_pick_up_latest_checkpoint --
 *     Pick up the latest WiredTiger checkpoint.
 */
bool
wt_disagg_pick_up_latest_checkpoint(WT_CONNECTION *conn, model::timestamp_t &checkpoint_timestamp)
{
    int ret;
    checkpoint_timestamp = model::k_timestamp_none;

    WT_PAGE_LOG *page_log;
    ret = conn->get_page_log(conn, "palm", &page_log);
    if (ret != 0)
        throw wiredtiger_exception("Cannot get page log \"palm\"", ret);

    WT_SESSION *session;
    ret = conn->open_session(conn, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Failed to open a session", ret);
    wiredtiger_session_guard wiredtiger_session_guard(session);

    WT_ITEM metadata{};
    uint64_t timestamp;
    ret = page_log->pl_get_complete_checkpoint_ext(
      page_log, session, nullptr, nullptr, &timestamp, &metadata);
    if (ret == WT_NOTFOUND)
        return false;
    if (ret != 0)
        throw wiredtiger_exception("Cannot get checkpoint metadata", ret);
    char *checkpoint_meta = strndup((const char *)metadata.data, metadata.size);
    free(metadata.mem);

    std::ostringstream config;
    config << "disaggregated=(checkpoint_meta=\"" << checkpoint_meta << "\")";
    free(checkpoint_meta);

    std::string config_str = config.str();
    ret = conn->reconfigure(conn, config_str.c_str());
    if (ret != 0)
        throw wiredtiger_exception("Cannot reconfigure WiredTiger", ret);

    checkpoint_timestamp = model::timestamp_t(timestamp);
    return true;
}

/*
 * wt_evict --
 *     Evict a WiredTiger page with the given key.
 */
void
wt_evict(WT_CONNECTION *conn, const char *uri, const data_value &key)
{
    WT_SESSION *session;
    int ret = conn->open_session(conn, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a session: ", ret);
    wiredtiger_session_guard session_guard(session);

    ret = session->begin_transaction(session, "ignore_prepare=true");
    if (ret != 0)
        throw wiredtiger_exception("Transaction begin failed: ", ret);

    WT_CURSOR *cursor;
    ret = session->open_cursor(session, uri, nullptr, "debug=(release_evict)", &cursor);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a cursor: ", ret);
    wiredtiger_cursor_guard cursor_guard(cursor);

    set_wt_cursor_key(cursor, key);
    ret = cursor->search_near(cursor, nullptr);
    if (ret != 0 && ret != WT_NOTFOUND)
        throw wiredtiger_exception("Search failed: ", ret);

    ret = cursor->reset(cursor);
    if (ret != 0)
        throw wiredtiger_exception("Cursor reset failed: ", ret);
}

/*
 * wt_extension_path --
 *     Path to the given WiredTiger extension given its relative path. Throw an exception on error.
 */
std::string
wt_extension_path(const std::string &path)
{
    return join(join(wt_build_dir_path(), "ext", "/"), path, "/");
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
