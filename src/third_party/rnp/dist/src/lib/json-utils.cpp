/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "json-utils.h"
#include "logging.h"
#include "crypto/mem.h"

/* Shortcut function to add field checking it for null to avoid allocation failure.
   Please note that it deallocates val on failure. */
bool
json_add(json_object *obj, const char *name, json_object *val)
{
    if (!val) {
        return false;
    }
    // TODO: in JSON-C 0.13 json_object_object_add returns bool instead of void
    json_object_object_add(obj, name, val);
    if (!json_object_object_get_ex(obj, name, NULL)) {
        json_object_put(val);
        return false;
    }

    return true;
}

bool
json_add(json_object *obj, const char *name, const char *value)
{
    return json_add(obj, name, json_object_new_string(value));
}

bool
json_add(json_object *obj, const char *name, bool value)
{
    return json_add(obj, name, json_object_new_boolean(value));
}

bool
json_add(json_object *obj, const char *name, int value)
{
    return json_add(obj, name, json_object_new_int(value));
}

bool
json_add(json_object *obj, const char *name, uint64_t value)
{
#if (JSON_C_MAJOR_VERSION == 0) && (JSON_C_MINOR_VERSION < 14)
    return json_add(obj, name, json_object_new_int64(value));
#else
    return json_add(obj, name, json_object_new_uint64(value));
#endif
}

bool
json_add(json_object *obj, const char *name, const char *value, size_t len)
{
    return json_add(obj, name, json_object_new_string_len(value, len));
}

bool
json_add(json_object *obj, const char *name, const std::string &value)
{
    return json_add(obj, name, json_object_new_string_len(value.data(), value.size()));
}

bool
json_add_hex(json_object *obj, const char *name, const uint8_t *val, size_t val_len)
{
    if (val_len > 1024 * 1024) {
        RNP_LOG("too large json hex field: %zu", val_len);
        val_len = 1024 * 1024;
    }

    return json_add(obj, name, bin_to_hex(val, val_len, rnp::HexFormat::Lowercase));
}

bool
json_add_hex(json_object *obj, const char *name, const std::vector<uint8_t> &vec)
{
    return json_add_hex(obj, name, vec.data(), vec.size());
}

bool
json_add(json_object *obj, const char *name, const pgp::KeyID &keyid)
{
    return json_add_hex(obj, name, keyid.data(), keyid.size());
}

bool
json_add(json_object *obj, const char *name, const pgp::Fingerprint &fp)
{
    return json_add_hex(obj, name, fp.data(), fp.size());
}

bool
json_array_add(json_object *obj, const char *val)
{
    return json_array_add(obj, json_object_new_string(val));
}

bool
json_array_add(json_object *obj, json_object *val)
{
    if (!val) {
        return false;
    }
    if (json_object_array_add(obj, val)) {
        json_object_put(val);
        return false;
    }
    return true;
}

static json_object *
json_get_field(json_object *obj, const char *name, json_type type)
{
    json_object *res = NULL;
    if (!json_object_object_get_ex(obj, name, &res) || !json_object_is_type(res, type)) {
        return NULL;
    }
    return res;
}

bool
json_get_str(json_object *obj, const char *name, std::string &value, bool del)
{
    auto str = json_get_field(obj, name, json_type_string);
    if (!str) {
        return false;
    }
    value = json_object_get_string(str);
    if (del) {
        json_object_object_del(obj, name);
    }
    return true;
}

bool
json_get_int(json_object *obj, const char *name, int &value, bool del)
{
    auto num = json_get_field(obj, name, json_type_int);
    if (!num) {
        return false;
    }
    value = json_object_get_int(num);
    if (del) {
        json_object_object_del(obj, name);
    }
    return true;
}

bool
json_get_uint64(json_object *obj, const char *name, uint64_t &value, bool del)
{
    auto num = json_get_field(obj, name, json_type_int);
    if (!num) {
        return false;
    }
    value = (uint64_t) json_object_get_int64(num);
    if (del) {
        json_object_object_del(obj, name);
    }
    return true;
}

bool
json_get_str_arr(json_object *obj, const char *name, std::vector<std::string> &value, bool del)
{
    auto arr = json_get_field(obj, name, json_type_array);
    if (!arr) {
        return false;
    }
    value.clear();
    for (size_t i = 0; i < (size_t) json_object_array_length(arr); i++) {
        json_object *item = json_object_array_get_idx(arr, i);
        if (!json_object_is_type(item, json_type_string)) {
            return false;
        }
        value.push_back(json_object_get_string(item));
    }
    if (del) {
        json_object_object_del(obj, name);
    }
    return true;
}

json_object *
json_get_obj(json_object *obj, const char *name)
{
    return json_get_field(obj, name, json_type_object);
}
