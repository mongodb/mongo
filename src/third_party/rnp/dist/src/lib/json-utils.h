/*
 * Copyright (c) 2019, [Ribose Inc](https://www.ribose.com).
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
#ifndef RNP_JSON_UTILS_H_
#define RNP_JSON_UTILS_H_

#include <stdio.h>
#include "types.h"
#include <limits.h>
#include "json_object.h"
#include "json.h"
#include "types.h"
#include "fingerprint.hpp"

/**
 * @brief Add field to the json object.
 *        Note: this function is for convenience, it will check val for NULL and destroy val
 *        on failure.
 * @param obj allocated json_object of object type.
 * @param name name of the field
 * @param val json object of any type. Will be checked for NULL.
 * @return true if val is not NULL and field was added successfully, false otherwise.
 */
bool json_add(json_object *obj, const char *name, json_object *val);

/**
 * @brief Shortcut to add string via json_add().
 */
bool json_add(json_object *obj, const char *name, const char *value);

/**
 * @brief Shortcut to add string with length via json_add().
 */
bool json_add(json_object *obj, const char *name, const char *value, size_t len);

bool json_add(json_object *obj, const char *name, const std::string &value);

/**
 * @brief Shortcut to add bool via json_add().
 */
bool json_add(json_object *obj, const char *name, bool value);

/**
 * @brief Shortcut to add int via json_add().
 */
bool json_add(json_object *obj, const char *name, int value);

/**
 * @brief Shortcut to add uint64 via json_add().
 */
bool json_add(json_object *obj, const char *name, uint64_t value);

/**
 * @brief Add hex representation of binary data as string field to JSON object.
 *        Note: this function follows conventions of json_add().
 */
bool json_add_hex(json_object *obj, const char *name, const uint8_t *val, size_t val_len);

bool json_add_hex(json_object *obj, const char *name, const std::vector<uint8_t> &vec);

/**
 * @brief Shortcut to add keyid via json_add_hex().
 */
bool json_add(json_object *obj, const char *name, const pgp::KeyID &keyid);

/**
 * @brief Shortcut to add fingerprint via json_add_hex().
 */
bool json_add(json_object *obj, const char *name, const pgp::Fingerprint &fp);

/**
 * @brief Shortcut to add string to the json array.
 */
bool json_array_add(json_object *obj, const char *val);

/**
 * @brief Add element to JSON array.
 *        Note: this function follows convention of the json_add.
 */
bool json_array_add(json_object *obj, json_object *val);

/**
 * @brief Get string from the object, and optionally delete the field.
 *        Would check field's type as well.
 *
 * @param obj json object
 * @param name field name
 * @param value on success field value will be stored here.
 * @param del true to delete field after the extraction.
 * @return true on success or false otherwise.
 */
bool json_get_str(json_object *obj, const char *name, std::string &value, bool del = true);

/**
 * Analog of the previous but extracts int value.
 */
bool json_get_int(json_object *obj, const char *name, int &value, bool del = true);
bool json_get_uint64(json_object *obj, const char *name, uint64_t &value, bool del = true);

/**
 * Analog of previous which extract array of string values.
 */
bool json_get_str_arr(json_object *             obj,
                      const char *              name,
                      std::vector<std::string> &value,
                      bool                      del = true);

/* Get object with specified name, but do not delete it from json */
json_object *json_get_obj(json_object *obj, const char *name);

namespace rnp {
class JSONObject {
    json_object *obj_;

  public:
    JSONObject(json_object *obj) : obj_(obj)
    {
    }

    ~JSONObject()
    {
        if (obj_) {
            json_object_put(obj_);
        }
    }

    json_object *
    release()
    {
        json_object *res = obj_;
        obj_ = NULL;
        return res;
    }
};
} // namespace rnp

#endif
