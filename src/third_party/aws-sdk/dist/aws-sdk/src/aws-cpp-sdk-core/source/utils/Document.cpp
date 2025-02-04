/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/Document.h>

#include <iterator>
#include <algorithm>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/json/JsonSerializer.h>

using namespace Aws::Utils;

Document::Document() : m_wasParseSuccessful(true)
{
    m_json = nullptr;
}

Document::Document(cJSON* value) :
    m_json(cJSON_AS4CPP_Duplicate(value, true /* recurse */)),
    m_wasParseSuccessful(true)
{
}

Document::Document(const Aws::String& value) : m_wasParseSuccessful(true)
{
    const char* return_parse_end;
    m_json = cJSON_AS4CPP_ParseWithOpts(value.c_str(), &return_parse_end, 1/*require_null_terminated*/);

    if (!m_json || cJSON_AS4CPP_IsInvalid(m_json))
    {
        m_wasParseSuccessful = false;
        m_errorMessage = "Failed to parse JSON at: ";
        m_errorMessage += return_parse_end;
    }
}

Document::Document(Aws::IStream& istream) : m_wasParseSuccessful(true)
{
    Aws::StringStream memoryStream;
    std::copy(std::istreambuf_iterator<char>(istream), std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(memoryStream));
    const char* return_parse_end;
    const auto input = memoryStream.str();
    m_json = cJSON_AS4CPP_ParseWithOpts(input.c_str(), &return_parse_end, 1/*require_null_terminated*/);

    if (!m_json || cJSON_AS4CPP_IsInvalid(m_json))
    {
        m_wasParseSuccessful = false;
        m_errorMessage = "Failed to parse JSON. Invalid input at: ";
        m_errorMessage += return_parse_end;
    }
}

Document::Document(const Document& value) :
    m_json(cJSON_AS4CPP_Duplicate(value.m_json, true/*recurse*/)),
    m_wasParseSuccessful(value.m_wasParseSuccessful),
    m_errorMessage(value.m_errorMessage)
{
}

Document::Document(Document&& value) :
    m_json(value.m_json),
    m_wasParseSuccessful(value.m_wasParseSuccessful),
    m_errorMessage(std::move(value.m_errorMessage))
{
    value.m_json = nullptr;
}

Document::Document(const Json::JsonView& view) :
    m_json(cJSON_AS4CPP_Duplicate(view.m_value, true/*recurse*/)),
    m_wasParseSuccessful(true),
    m_errorMessage({})
{
}

void Document::Destroy()
{
    cJSON_AS4CPP_Delete(m_json);
}

Document::~Document()
{
    Destroy();
}

Document& Document::operator=(const Document& other)
{
    if (this == &other)
    {
        return *this;
    }

    Destroy();
    m_json = cJSON_AS4CPP_Duplicate(other.m_json, true /*recurse*/);
    m_wasParseSuccessful = other.m_wasParseSuccessful;
    m_errorMessage = other.m_errorMessage;
    return *this;
}

Document& Document::operator=(Document&& other)
{
    if (this == &other)
    {
        return *this;
    }

    using std::swap;
    swap(m_json, other.m_json);
    swap(m_errorMessage, other.m_errorMessage);
    m_wasParseSuccessful = other.m_wasParseSuccessful;
    return *this;
}

Document& Document::operator=(const Json::JsonView& other)
{
    Destroy();
    m_json = cJSON_AS4CPP_Duplicate(other.m_value, true /*recurse*/);
    m_wasParseSuccessful = true;
    m_errorMessage = {};
    return *this;
}

bool Document::operator==(const Document& other) const
{
    return cJSON_AS4CPP_Compare(m_json, other.m_json, true /*case-sensitive*/) != 0;
}

bool Document::operator!=(const Document& other) const
{
    return !(*this == other);
}

static void AddOrReplace(cJSON* root, const char* key, cJSON* value)
{
    const auto existing = cJSON_AS4CPP_GetObjectItemCaseSensitive(root, key);
    if (existing)
    {
        cJSON_AS4CPP_ReplaceItemInObjectCaseSensitive(root, key, value);
    }
    else
    {
        cJSON_AS4CPP_AddItemToObject(root, key, value);
    }
}

Document& Document::WithString(const char* key, const Aws::String& value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    const auto val = cJSON_AS4CPP_CreateString(value.c_str());
    AddOrReplace(m_json, key, val);
    return *this;
}

Document& Document::WithString(const Aws::String& key, const Aws::String& value)
{
    return WithString(key.c_str(), value);
}

Document& Document::AsString(const Aws::String& value)
{
    Destroy();
    m_json = cJSON_AS4CPP_CreateString(value.c_str());
    return *this;
}

Document& Document::WithBool(const char* key, bool value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    const auto val = cJSON_AS4CPP_CreateBool(value);
    AddOrReplace(m_json, key, val);
    return *this;
}

Document& Document::WithBool(const Aws::String& key, bool value)
{
    return WithBool(key.c_str(), value);
}

Document& Document::AsBool(bool value)
{
    Destroy();
    m_json = cJSON_AS4CPP_CreateBool(value);
    return *this;
}

Document& Document::WithInteger(const char* key, int value)
{
    return WithDouble(key, static_cast<double>(value));
}

Document& Document::WithInteger(const Aws::String& key, int value)
{
    return WithDouble(key.c_str(), static_cast<double>(value));
}

Document& Document::AsInteger(int value)
{
    Destroy();
    m_json = cJSON_AS4CPP_CreateNumber(static_cast<double>(value));
    return *this;
}

Document& Document::WithInt64(const char* key, long long value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    const auto val = cJSON_AS4CPP_CreateInt64(value);
    AddOrReplace(m_json, key, val);
    return *this;
}

Document& Document::WithInt64(const Aws::String& key, long long value)
{
    return WithInt64(key.c_str(), value);
}

Document& Document::AsInt64(long long value)
{
    Destroy();
    m_json = cJSON_AS4CPP_CreateInt64(value);
    return *this;
}

Document& Document::WithDouble(const char* key, double value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    const auto val = cJSON_AS4CPP_CreateNumber(value);
    AddOrReplace(m_json, key, val);
    return *this;
}

Document& Document::WithDouble(const Aws::String& key, double value)
{
    return WithDouble(key.c_str(), value);
}

Document& Document::AsDouble(double value)
{
    Destroy();
    m_json = cJSON_AS4CPP_CreateNumber(value);
    return *this;
}

Document& Document::WithArray(const char* key, const Array<Aws::String>& array)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    auto arrayValue = cJSON_AS4CPP_CreateArray();
    for (unsigned i = 0; i < array.GetLength(); ++i)
    {
        cJSON_AS4CPP_AddItemToArray(arrayValue, cJSON_AS4CPP_CreateString(array[i].c_str()));
    }

    AddOrReplace(m_json, key, arrayValue);
    return *this;
}

Document& Document::WithArray(const Aws::String& key, const Array<Aws::String>& array)
{
    return WithArray(key.c_str(), array);
}

Document& Document::WithArray(const Aws::String& key, const Array<Document>& array)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    auto arrayValue = cJSON_AS4CPP_CreateArray();
    for (unsigned i = 0; i < array.GetLength(); ++i)
    {
        cJSON_AS4CPP_AddItemToArray(arrayValue, cJSON_AS4CPP_Duplicate(array[i].m_json, true /*recurse*/));
    }

    AddOrReplace(m_json, key.c_str(), arrayValue);
    return *this;
}

Document& Document::WithArray(const Aws::String& key, Array<Document>&& array)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    auto arrayValue = cJSON_AS4CPP_CreateArray();
    for (unsigned i = 0; i < array.GetLength(); ++i)
    {
        cJSON_AS4CPP_AddItemToArray(arrayValue, array[i].m_json);
        array[i].m_json = nullptr;
    }

    AddOrReplace(m_json, key.c_str(), arrayValue);
    return *this;
}

Document& Document::AsArray(const Array<Document>& array)
{
    auto arrayValue = cJSON_AS4CPP_CreateArray();
    for (unsigned i = 0; i < array.GetLength(); ++i)
    {
        cJSON_AS4CPP_AddItemToArray(arrayValue, cJSON_AS4CPP_Duplicate(array[i].m_json, true /*recurse*/));
    }

    Destroy();
    m_json = arrayValue;
    return *this;
}

Document& Document::AsArray(Array<Document>&& array)
{
    auto arrayValue = cJSON_AS4CPP_CreateArray();
    for (unsigned i = 0; i < array.GetLength(); ++i)
    {
        cJSON_AS4CPP_AddItemToArray(arrayValue, array[i].m_json);
        array[i].m_json = nullptr;
    }

    Destroy();
    m_json = arrayValue;
    return *this;
}

Document& Document::WithObject(const char* key, const Document& value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    const auto copy = value.m_json == nullptr ? cJSON_AS4CPP_CreateObject() : cJSON_AS4CPP_Duplicate(value.m_json, true /*recurse*/);
    AddOrReplace(m_json, key, copy);
    return *this;
}

Document& Document::WithObject(const Aws::String& key, const Document& value)
{
    return WithObject(key.c_str(), value);
}

Document& Document::WithObject(const char* key, Document&& value)
{
    if (!m_json)
    {
        m_json = cJSON_AS4CPP_CreateObject();
    }

    AddOrReplace(m_json, key, value.m_json == nullptr ? cJSON_AS4CPP_CreateObject() : value.m_json);
    value.m_json = nullptr;
    return *this;
}

Document& Document::WithObject(const Aws::String& key, Document&& value)
{
    return WithObject(key.c_str(), std::move(value));
}

Document& Document::AsObject(const Document& value)
{
    *this = value;
    return *this;
}

Document& Document::AsObject(Document && value)
{
    *this = std::move(value);
    return *this;
}

DocumentView Document::View() const
{
    return *this;
}

DocumentView::DocumentView() : m_json(nullptr)
{
}

DocumentView::DocumentView(const Document& value) : m_json(value.m_json)
{
}

DocumentView::DocumentView(cJSON* v) : m_json(v)
{
}

DocumentView& DocumentView::operator=(const Document& value)
{
    m_json = value.m_json;
    return *this;
}

DocumentView& DocumentView::operator=(cJSON* value)
{
    m_json = value;
    return *this;
}

Aws::String DocumentView::GetString(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    auto str = cJSON_AS4CPP_GetStringValue(item);
    return str ? str : "";
}

Aws::String DocumentView::AsString() const
{
    const char* str = cJSON_AS4CPP_GetStringValue(m_json);
    if (str == nullptr)
    {
        return {};
    }
    return str;
}

bool DocumentView::IsString() const
{
    return cJSON_AS4CPP_IsString(m_json) != 0;
}

bool DocumentView::GetBool(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    assert(item);
    return item->valueint != 0;
}

bool DocumentView::AsBool() const
{
    assert(cJSON_AS4CPP_IsBool(m_json));
    return cJSON_AS4CPP_IsTrue(m_json) != 0;
}

bool DocumentView::IsBool() const
{
    return cJSON_AS4CPP_IsBool(m_json) != 0;
}

int DocumentView::GetInteger(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    assert(item);
    return item->valueint;
}

int DocumentView::AsInteger() const
{
    assert(cJSON_AS4CPP_IsNumber(m_json)); // can be double or value larger than int_max, but at least not UB
    return m_json->valueint;
}

bool DocumentView::IsIntegerType() const
{
    if (!cJSON_AS4CPP_IsNumber(m_json))
    {
        return false;
    }

    if (m_json->valuestring)
    {
        Aws::String valueString = m_json->valuestring;
        return std::all_of(valueString.begin(), valueString.end(), [](unsigned char c){ return ::isdigit(c) || c == '+' || c == '-'; });
    }
    return m_json->valuedouble == static_cast<long long>(m_json->valuedouble);
}

int64_t DocumentView::GetInt64(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    assert(item);
    if (item->valuestring)
    {
        return Aws::Utils::StringUtils::ConvertToInt64(item->valuestring);
    }
    else
    {
        return static_cast<int64_t>(item->valuedouble);
    }
}

int64_t DocumentView::AsInt64() const
{
    assert(cJSON_AS4CPP_IsNumber(m_json));
    if (m_json->valuestring)
    {
        return Aws::Utils::StringUtils::ConvertToInt64(m_json->valuestring);
    }
    else
    {
        return static_cast<int64_t>(m_json->valuedouble);
    }
}

double DocumentView::GetDouble(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    assert(item);
    return item->valuedouble;
}

double DocumentView::AsDouble() const
{
    assert(cJSON_AS4CPP_IsNumber(m_json));
    return m_json->valuedouble;
}

bool DocumentView::IsFloatingPointType() const
{
    if (!cJSON_AS4CPP_IsNumber(m_json))
    {
        return false;
    }

    if (m_json->valuestring)
    {
        Aws::String valueString = m_json->valuestring;
        return std::any_of(valueString.begin(), valueString.end(), [](unsigned char c){ return !::isdigit(c) && c != '+' && c != '-'; });
    }
    return m_json->valuedouble != static_cast<long long>(m_json->valuedouble);
}

Array<DocumentView> DocumentView::GetArray(const Aws::String& key) const
{
    assert(m_json);
    auto array = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    assert(cJSON_AS4CPP_IsArray(array));
    Array<DocumentView> returnArray(cJSON_AS4CPP_GetArraySize(array));

    auto element = array->child;
    for (unsigned i = 0; element && i < returnArray.GetLength(); ++i, element = element->next)
    {
        returnArray[i] = element;
    }

    return returnArray;
}

Array<DocumentView> DocumentView::AsArray() const
{
    assert(cJSON_AS4CPP_IsArray(m_json));
    Array<DocumentView> returnArray(cJSON_AS4CPP_GetArraySize(m_json));

    auto element = m_json->child;

    for (unsigned i = 0; element && i < returnArray.GetLength(); ++i, element = element->next)
    {
        returnArray[i] = element;
    }

    return returnArray;
}

bool DocumentView::IsListType() const
{
    return cJSON_AS4CPP_IsArray(m_json) != 0;
}

DocumentView DocumentView::GetObject(const Aws::String& key) const
{
    assert(m_json);
    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    return item;
}

DocumentView DocumentView::AsObject() const
{
    assert(cJSON_AS4CPP_IsObject(m_json) || cJSON_AS4CPP_IsNull(m_json));
    return m_json;
}

bool DocumentView::IsObject() const
{
    return cJSON_AS4CPP_IsObject(m_json) != 0;
}

bool DocumentView::IsNull() const
{
    return cJSON_AS4CPP_IsNull(m_json) != 0;
}

Aws::Map<Aws::String, DocumentView> DocumentView::GetAllObjects() const
{
    Aws::Map<Aws::String, DocumentView> valueMap;
    if (!m_json)
    {
        return valueMap;
    }

    for (auto iter = m_json->child; iter; iter = iter->next)
    {
        valueMap.emplace(std::make_pair(Aws::String(iter->string), DocumentView(iter)));
    }

    return valueMap;
}

bool DocumentView::ValueExists(const Aws::String& key) const
{
    if (!cJSON_AS4CPP_IsObject(m_json))
    {
        return false;
    }

    auto item = cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str());
    return !(item == nullptr || cJSON_AS4CPP_IsNull(item));
}

bool DocumentView::KeyExists(const Aws::String& key) const
{
    if (!cJSON_AS4CPP_IsObject(m_json))
    {
        return false;
    }

    return cJSON_AS4CPP_GetObjectItemCaseSensitive(m_json, key.c_str()) != nullptr;;
}

Aws::String DocumentView::WriteCompact() const
{
    if (!m_json)
    {
        return "null";
    }

    auto temp = cJSON_AS4CPP_PrintUnformatted(m_json);
    Aws::String out(temp);
    cJSON_AS4CPP_free(temp);
    return out;
}

Aws::String DocumentView::WriteReadable() const
{
    if (!m_json)
    {
        return "null";
    }

    auto temp = cJSON_AS4CPP_Print(m_json);
    Aws::String out(temp);
    cJSON_AS4CPP_free(temp);
    return out;
}

Document DocumentView::Materialize() const
{
    return m_json;
}
