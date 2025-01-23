/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/JsonObject.h>

#include <aws/common/json.h>

#include <algorithm>
#include <iterator>

namespace Aws
{
    namespace Crt
    {
        JsonObject::JsonObject() : m_value(nullptr) {}

        JsonObject::JsonObject(const aws_json_value *valueToCopy)
        {
            m_value = valueToCopy ? aws_json_value_duplicate(valueToCopy) : nullptr;
        }

        JsonObject::JsonObject(const String &stringToParse)
        {
            m_value = aws_json_value_new_from_string(ApiAllocator(), ByteCursorFromString(stringToParse));
        }

        JsonObject::JsonObject(const JsonObject &other) : JsonObject(other.m_value) {}

        JsonObject::JsonObject(JsonObject &&other) noexcept
        {
            m_value = other.m_value;
            other.m_value = nullptr;
        }

        JsonObject::~JsonObject()
        {
            aws_json_value_destroy(m_value);
        }

        JsonObject &JsonObject::operator=(const JsonObject &other)
        {
            if (this != &other)
            {
                this->~JsonObject();
                new (this) JsonObject(other);
            }

            return *this;
        }

        JsonObject &JsonObject::operator=(JsonObject &&other) noexcept
        {
            if (this != &other)
            {
                this->~JsonObject();
                new (this) JsonObject(std::move(other));
            }

            return *this;
        }

        JsonObject &JsonObject::AsNewValue(aws_json_value *valueToOwn)
        {
            this->~JsonObject();
            new (this) JsonObject();
            m_value = valueToOwn;
            return *this;
        }

        JsonObject &JsonObject::WithNewKeyValue(const char *key, aws_json_value *valueToOwn)
        {
            // Convert to JSON object if necessary
            if (m_value == nullptr || !aws_json_value_is_object(m_value))
            {
                AsNewValue(aws_json_value_new_object(ApiAllocator()));
            }

            struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(key);

            // Remove any existing item at key
            aws_json_value_remove_from_object(m_value, key_cursor);

            aws_json_value_add_to_object(m_value, key_cursor, valueToOwn);
            return *this;
        }

        JsonObject &JsonObject::WithString(const char *key, const String &value)
        {
            return WithNewKeyValue(key, aws_json_value_new_string(ApiAllocator(), ByteCursorFromString(value)));
        }

        JsonObject &JsonObject::WithString(const String &key, const String &value)
        {
            return WithString(key.c_str(), value);
        }

        JsonObject &JsonObject::AsString(const String &value)
        {
            return AsNewValue(aws_json_value_new_string(ApiAllocator(), ByteCursorFromString(value)));
        }

        JsonObject &JsonObject::WithBool(const char *key, bool value)
        {
            return WithNewKeyValue(key, aws_json_value_new_boolean(ApiAllocator(), value));
        }

        JsonObject &JsonObject::WithBool(const String &key, bool value)
        {
            return WithBool(key.c_str(), value);
        }

        JsonObject &JsonObject::AsBool(bool value)
        {
            return AsNewValue(aws_json_value_new_boolean(ApiAllocator(), value));
        }

        JsonObject &JsonObject::WithInteger(const char *key, int value)
        {
            return WithDouble(key, static_cast<double>(value));
        }

        JsonObject &JsonObject::WithInteger(const String &key, int value)
        {
            return WithDouble(key.c_str(), static_cast<double>(value));
        }

        JsonObject &JsonObject::AsInteger(int value)
        {
            return AsNewValue(aws_json_value_new_number(ApiAllocator(), static_cast<double>(value)));
        }

        JsonObject &JsonObject::WithInt64(const char *key, int64_t value)
        {
            return WithDouble(key, static_cast<double>(value));
        }

        JsonObject &JsonObject::WithInt64(const String &key, int64_t value)
        {
            return WithDouble(key.c_str(), static_cast<double>(value));
        }

        JsonObject &JsonObject::AsInt64(int64_t value)
        {
            return AsDouble(static_cast<double>(value));
        }

        JsonObject &JsonObject::WithDouble(const char *key, double value)
        {
            return WithNewKeyValue(key, aws_json_value_new_number(ApiAllocator(), value));
        }

        JsonObject &JsonObject::WithDouble(const String &key, double value)
        {
            return WithDouble(key.c_str(), value);
        }

        JsonObject &JsonObject::AsDouble(double value)
        {
            return AsNewValue(aws_json_value_new_number(ApiAllocator(), value));
        }

        JsonObject &JsonObject::WithArray(const char *key, const Vector<String> &array)
        {
            auto arrayValue = aws_json_value_new_array(ApiAllocator());
            for (const auto &i : array)
            {
                aws_json_value_add_array_element(
                    arrayValue, aws_json_value_new_string(ApiAllocator(), ByteCursorFromString(i)));
            }

            return WithNewKeyValue(key, arrayValue);
        }

        JsonObject &JsonObject::WithArray(const String &key, const Vector<String> &array)
        {
            return WithArray(key.c_str(), array);
        }

        aws_json_value *JsonObject::NewArray(const Vector<JsonObject> &objectsToCopy)
        {
            aws_json_value *newArray = aws_json_value_new_array(ApiAllocator());
            for (const auto &i : objectsToCopy)
            {
                /* old version of this code would silently ignore invalid items, so continue doing the same */
                if (i.m_value != nullptr)
                {
                    aws_json_value_add_array_element(newArray, aws_json_value_duplicate(i.m_value));
                }
            }
            return newArray;
        }

        aws_json_value *JsonObject::NewArray(Vector<JsonObject> &&objectsToMove)
        {
            aws_json_value *newArray = aws_json_value_new_array(ApiAllocator());
            for (auto &i : objectsToMove)
            {
                /* old version of this code would silently ignore invalid items, so continue doing the same */
                if (i.m_value != nullptr)
                {
                    aws_json_value_add_array_element(newArray, i.m_value);
                    i.m_value = nullptr;
                }
            }
            return newArray;
        }

        JsonObject &JsonObject::WithArray(const String &key, const Vector<JsonObject> &array)
        {
            return WithNewKeyValue(key.c_str(), NewArray(array));
        }

        JsonObject &JsonObject::WithArray(const String &key, Vector<JsonObject> &&array)
        {
            return WithNewKeyValue(key.c_str(), NewArray(std::move(array)));
        }

        JsonObject &JsonObject::AsArray(const Vector<JsonObject> &array)
        {
            return AsNewValue(NewArray(array));
        }

        JsonObject &JsonObject::AsArray(Vector<JsonObject> &&array)
        {
            return AsNewValue(NewArray(std::move(array)));
        }

        JsonObject &JsonObject::AsNull()
        {
            return AsNewValue(aws_json_value_new_null(ApiAllocator()));
        }

        JsonObject &JsonObject::WithObject(const char *key, const JsonObject &value)
        {
            const auto copy = value.m_value == nullptr ? aws_json_value_new_object(ApiAllocator())
                                                       : aws_json_value_duplicate(value.m_value);
            return WithNewKeyValue(key, copy);
        }

        JsonObject &JsonObject::WithObject(const String &key, const JsonObject &value)
        {
            return WithObject(key.c_str(), value);
        }

        JsonObject &JsonObject::WithObject(const char *key, JsonObject &&value)
        {
            auto valueToOwn = value.m_value == nullptr ? aws_json_value_new_object(ApiAllocator()) : value.m_value;
            value.m_value = nullptr;
            return WithNewKeyValue(key, valueToOwn);
        }

        JsonObject &JsonObject::WithObject(const String &key, JsonObject &&value)
        {
            return WithObject(key.c_str(), std::move(value));
        }

        JsonObject &JsonObject::AsObject(const JsonObject &value)
        {
            *this = value;
            return *this;
        }

        JsonObject &JsonObject::AsObject(JsonObject &&value)
        {
            *this = std::move(value);
            return *this;
        }

        bool JsonObject::operator==(const JsonObject &other) const
        {
            if (m_value != nullptr && other.m_value != nullptr)
            {
                return aws_json_value_compare(m_value, other.m_value, true);
            }
            return false;
        }

        bool JsonObject::operator!=(const JsonObject &other) const
        {
            return !(*this == other);
        }

        std::unique_ptr<String> JsonObject::s_errorMessage;
        std::unique_ptr<String> JsonObject::s_okMessage;

        void JsonObject::OnLibraryInit()
        {
            s_errorMessage.reset(new String("Failed to parse JSON"));
            s_okMessage.reset(new String(""));
        }

        void JsonObject::OnLibraryCleanup()
        {
            s_errorMessage.reset();
            s_okMessage.reset();
        }

        const String &JsonObject::GetErrorMessage() const
        {
            return m_value == nullptr ? *s_errorMessage : *s_okMessage;
        }

        JsonView JsonObject::View() const
        {
            return JsonView(*this);
        }

        JsonView::JsonView() : m_value(nullptr) {}

        JsonView::JsonView(const JsonObject &val) : m_value(val.m_value) {}

        JsonView::JsonView(const aws_json_value *val) : m_value(val) {}

        JsonView &JsonView::operator=(const JsonObject &v)
        {
            m_value = v.m_value;
            return *this;
        }

        String JsonView::GetString(const String &key) const
        {
            return GetString(key.c_str());
        }

        String JsonView::GetString(const char *key) const
        {
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    struct aws_byte_cursor cursor;
                    if (aws_json_value_get_string(item, &cursor) == AWS_OP_SUCCESS)
                    {
                        return String((const char *)cursor.ptr, cursor.len);
                    }
                }
            }

            return "";
        }

        String JsonView::AsString() const
        {
            if (m_value != nullptr)
            {
                struct aws_byte_cursor cursor;
                if (aws_json_value_get_string(m_value, &cursor) == AWS_OP_SUCCESS)
                {
                    return String((const char *)cursor.ptr, cursor.len);
                }
            }

            return "";
        }

        bool JsonView::GetBool(const String &key) const
        {
            return GetBool(key.c_str());
        }

        bool JsonView::GetBool(const char *key) const
        {
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    bool boolean = false;
                    if (aws_json_value_get_boolean(item, &boolean) == AWS_OP_SUCCESS)
                    {
                        return boolean;
                    }
                }
            }

            return false;
        }

        bool JsonView::AsBool() const
        {
            if (m_value != nullptr)
            {
                bool boolean = false;
                if (aws_json_value_get_boolean(m_value, &boolean) == AWS_OP_SUCCESS)
                {
                    return boolean;
                }
            }

            return false;
        }

        int JsonView::GetInteger(const String &key) const
        {
            return static_cast<int>(GetDouble(key));
        }

        int JsonView::GetInteger(const char *key) const
        {
            return static_cast<int>(GetDouble(key));
        }

        int JsonView::AsInteger() const
        {
            return static_cast<int>(AsDouble());
        }

        int64_t JsonView::GetInt64(const String &key) const
        {
            return static_cast<int64_t>(GetDouble(key));
        }

        int64_t JsonView::GetInt64(const char *key) const
        {
            return static_cast<int64_t>(GetDouble(key));
        }

        int64_t JsonView::AsInt64() const
        {
            return static_cast<int64_t>(AsDouble());
        }

        double JsonView::GetDouble(const String &key) const
        {
            return GetDouble(key.c_str());
        }

        double JsonView::GetDouble(const char *key) const
        {
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    double number;
                    if (aws_json_value_get_number(item, &number) == AWS_OP_SUCCESS)
                    {
                        return number;
                    }
                }
            }

            return 0.0;
        }

        double JsonView::AsDouble() const
        {
            if (m_value != nullptr)
            {
                double number;
                if (aws_json_value_get_number(m_value, &number) == AWS_OP_SUCCESS)
                {
                    return number;
                }
            }

            return 0.0;
        }

        JsonView JsonView::GetJsonObject(const String &key) const
        {
            return GetJsonObject(key.c_str());
        }

        JsonView JsonView::GetJsonObject(const char *key) const
        {
            if (m_value != nullptr)
            {
                struct aws_json_value *value_at_key =
                    aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (value_at_key != nullptr)
                {
                    return JsonView(value_at_key);
                }
            }

            // failed
            return JsonView();
        }

        JsonObject JsonView::GetJsonObjectCopy(const String &key) const
        {
            return GetJsonObjectCopy(key.c_str());
        }

        JsonObject JsonView::GetJsonObjectCopy(const char *key) const
        {
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    /* force a deep copy */
                    return JsonObject(item);
                }
            }

            // failed
            return JsonObject();
        }

        JsonView JsonView::AsObject() const
        {
            if (m_value != nullptr)
            {
                if (aws_json_value_is_object(m_value))
                {
                    return JsonView(m_value);
                }
            }

            // failed
            return JsonView();
        }

        Vector<JsonView> JsonView::GetArray(const String &key) const
        {
            return GetArray(key.c_str());
        }

        Vector<JsonView> JsonView::GetArray(const char *key) const
        {
            Vector<JsonView> returnArray;
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    return JsonView(item).AsArray();
                }
            }

            return returnArray;
        }

        Vector<JsonView> JsonView::AsArray() const
        {
            Vector<JsonView> returnArray;
            if (m_value != nullptr)
            {
                aws_json_const_iterate_array(
                    m_value,
                    [](size_t index, const aws_json_value *value, bool *out_should_continue, void *user_data)
                    {
                        (void)index;
                        (void)out_should_continue;
                        auto returnArray = static_cast<Vector<JsonView> *>(user_data);
                        returnArray->emplace_back(JsonView(const_cast<aws_json_value *>(value)));
                        return AWS_OP_SUCCESS;
                    },
                    &returnArray);
            }

            return returnArray;
        }

        Map<String, JsonView> JsonView::GetAllObjects() const
        {
            Map<String, JsonView> valueMap;
            if (m_value != nullptr)
            {
                aws_json_const_iterate_object(
                    m_value,
                    [](const aws_byte_cursor *key,
                       const aws_json_value *value,
                       bool *out_should_continue,
                       void *user_data)
                    {
                        (void)out_should_continue;
                        auto valueMap = static_cast<Map<String, JsonView> *>(user_data);
                        valueMap->emplace(
                            String((const char *)key->ptr, key->len), JsonView(const_cast<aws_json_value *>(value)));
                        return AWS_OP_SUCCESS;
                    },
                    &valueMap);
            }

            return valueMap;
        }

        bool JsonView::ValueExists(const String &key) const
        {
            return ValueExists(key.c_str());
        }

        bool JsonView::ValueExists(const char *key) const
        {
            if (m_value != nullptr)
            {
                aws_json_value *item = aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key));
                if (item != nullptr)
                {
                    return !aws_json_value_is_null(item);
                }
            }

            return false;
        }

        bool JsonView::KeyExists(const String &key) const
        {
            return KeyExists(key.c_str());
        }

        bool JsonView::KeyExists(const char *key) const
        {
            if (m_value != nullptr)
            {
                return aws_json_value_get_from_object(m_value, aws_byte_cursor_from_c_str(key)) != nullptr;
            }

            return false;
        }

        bool JsonView::IsObject() const
        {
            return m_value != nullptr && aws_json_value_is_object(m_value);
        }

        bool JsonView::IsBool() const
        {
            return m_value != nullptr && aws_json_value_is_boolean(m_value);
        }

        bool JsonView::IsString() const
        {
            return m_value != nullptr && aws_json_value_is_string(m_value);
        }

        bool JsonView::IsNumber() const
        {
            return m_value != nullptr && aws_json_value_is_number(m_value);
        }

        bool JsonView::IsIntegerType() const
        {
            if (m_value)
            {
                double value_double;
                if (aws_json_value_get_number(m_value, &value_double) == AWS_OP_SUCCESS)
                {
                    return value_double == static_cast<int64_t>(value_double);
                }
            }

            return false;
        }

        bool JsonView::IsFloatingPointType() const
        {
            if (m_value)
            {
                double value_double;
                if (aws_json_value_get_number(m_value, &value_double) == AWS_OP_SUCCESS)
                {
                    return value_double != static_cast<int64_t>(value_double);
                }
            }

            return false;
        }

        bool JsonView::IsListType() const
        {
            return m_value != nullptr && aws_json_value_is_array(m_value);
        }

        bool JsonView::IsNull() const
        {
            return m_value != nullptr && aws_json_value_is_null(m_value);
        }

        String JsonView::Write(bool treatAsObject, bool readable) const
        {
            if (m_value == nullptr)
            {
                if (treatAsObject)
                {
                    return "{}";
                }
                return "";
            }

            String resultString;
            aws_byte_buf buf;
            aws_byte_buf_init(&buf, ApiAllocator(), 0);

            auto aws_json_write_fn =
                readable ? aws_byte_buf_append_json_string_formatted : aws_byte_buf_append_json_string;

            if (aws_json_write_fn(m_value, &buf) == AWS_OP_SUCCESS)
            {
                resultString.assign((const char *)buf.buffer, buf.len);
            }
            aws_byte_buf_clean_up(&buf);
            return resultString;
        }

        String JsonView::WriteCompact(bool treatAsObject) const
        {
            return Write(treatAsObject, false /*readable*/);
        }

        String JsonView::WriteReadable(bool treatAsObject) const
        {
            return Write(treatAsObject, true /*readable*/);
        }

        JsonObject JsonView::Materialize() const
        {
            return m_value;
        }
    } // namespace Crt
} // namespace Aws
