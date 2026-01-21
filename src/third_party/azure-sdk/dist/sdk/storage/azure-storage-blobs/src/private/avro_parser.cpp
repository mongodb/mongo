// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "avro_parser.hpp"

#include <azure/core/azure_assert.hpp>
#include <azure/core/internal/json/json.hpp>

#include <algorithm>
#include <cstring>

namespace Azure { namespace Storage { namespace Blobs { namespace _detail {

  namespace {
    int64_t parseInt(AvroStreamReader::ReaderPos& data)
    {
      uint64_t r = 0;
      int nb = 0;
      while (true)
      {
        uint8_t c = (*data.BufferPtr)[data.Offset++];
        r = r | ((static_cast<uint64_t>(c) & 0x7f) << (nb * 7));
        if (c & 0x80)
        {
          ++nb;
          continue;
        }
        break;
      }
      return static_cast<int64_t>(r >> 1) ^ -static_cast<int64_t>(r & 0x01);
    }

    AvroSchema ParseSchemaFromJsonString(const std::string& jsonSchema)
    {
      const static std::map<std::string, AvroSchema> BuiltinNameSchemaMap = {
          {"string", AvroSchema::StringSchema},
          {"bytes", AvroSchema::BytesSchema},
          {"int", AvroSchema::IntSchema},
          {"long", AvroSchema::LongSchema},
          {"float", AvroSchema::FloatSchema},
          {"double", AvroSchema::DoubleSchema},
          {"boolean", AvroSchema::BoolSchema},
          {"null", AvroSchema::NullSchema},
          {"string", AvroSchema::StringSchema},
      };
      std::map<std::string, AvroSchema> nameSchemaMap = BuiltinNameSchemaMap;

      std::function<AvroSchema(const Core::Json::_internal::json& obj)> parseSchemaFromJsonObject;
      parseSchemaFromJsonObject = [&](const Core::Json::_internal::json& obj) -> AvroSchema {
        if (obj.is_string())
        {
          auto typeName = obj.get<std::string>();
          return nameSchemaMap.find(typeName)->second;
        }
        else if (obj.is_array())
        {
          std::vector<AvroSchema> unionSchemas;
          for (const auto& s : obj)
          {
            unionSchemas.push_back(parseSchemaFromJsonObject(s));
          }
          return AvroSchema::UnionSchema(std::move(unionSchemas));
        }
        else if (obj.is_object())
        {
          if (obj.count("namespace") != 0)
          {
            throw std::runtime_error("Namespace isn't supported yet in Avro schema.");
          }
          if (obj.count("aliases") != 0)
          {
            throw std::runtime_error("Alias isn't supported yet in Avro schema.");
          }
          auto typeName = obj["type"].get<std::string>();
          auto i = nameSchemaMap.find(typeName);
          if (i != nameSchemaMap.end())
          {
            return i->second;
          }
          if (typeName == "record")
          {
            std::vector<std::pair<std::string, AvroSchema>> fieldsSchema;
            for (const auto& field : obj["fields"])
            {
              fieldsSchema.push_back(std::make_pair(
                  field["name"].get<std::string>(), parseSchemaFromJsonObject(field["type"])));
            }

            const std::string recordName = obj["name"].get<std::string>();
            auto recordSchema = AvroSchema::RecordSchema(recordName, std::move(fieldsSchema));
            nameSchemaMap.insert(std::make_pair(recordName, recordSchema));
            return recordSchema;
          }
          else if (typeName == "enum")
          {
            throw std::runtime_error("Enum type isn't supported yet in Avro schema.");
          }
          else if (typeName == "array")
          {
            return AvroSchema::ArraySchema(parseSchemaFromJsonObject(obj["items"]));
          }
          else if (typeName == "map")
          {
            return AvroSchema::MapSchema(parseSchemaFromJsonObject(obj["items"]));
          }
          else if (typeName == "fixed")
          {
            const std::string fixedName = obj["name"].get<std::string>();
            auto fixedSchema = AvroSchema::FixedSchema(fixedName, obj["size"].get<int64_t>());
            nameSchemaMap.insert(std::make_pair(fixedName, fixedSchema));
            return fixedSchema;
          }
          else
          {
            throw std::runtime_error("Unrecognized type " + typeName + " in Avro schema.");
          }
        }
        AZURE_UNREACHABLE_CODE();
      };

      auto jsonRoot = Core::Json::_internal::json::parse(jsonSchema.begin(), jsonSchema.end());
      return parseSchemaFromJsonObject(jsonRoot);
    }
  } // namespace

  int64_t AvroStreamReader::ParseInt(const Core::Context& context)
  {
    uint64_t r = 0;
    int nb = 0;
    while (true)
    {
      Preload(1, context);
      uint8_t c = m_streambuffer[m_pos.Offset++];

      r = r | ((static_cast<uint64_t>(c) & 0x7f) << (nb * 7));
      if (c & 0x80)
      {
        ++nb;
        continue;
      }
      break;
    }
    return static_cast<int64_t>(r >> 1) ^ -static_cast<int64_t>(r & 0x01);
  }

  void AvroStreamReader::Advance(size_t n, const Core::Context& context)
  {
    Preload(n, context);
    m_pos.Offset += n;
  }

  size_t AvroStreamReader::Preload(size_t n, const Core::Context& context)
  {
    size_t oldAvailable = AvailableBytes();
    while (true)
    {
      size_t newAvailable = TryPreload(n, context);
      if (newAvailable >= n)
      {
        return newAvailable;
      }
      if (oldAvailable == newAvailable)
      {
        throw std::runtime_error("Unexpected EOF of Avro stream.");
      }
      oldAvailable = newAvailable;
    }
    AZURE_UNREACHABLE_CODE();
  }

  size_t AvroStreamReader::TryPreload(size_t n, const Core::Context& context)
  {
    size_t availableBytes = AvailableBytes();
    if (availableBytes >= n)
    {
      return availableBytes;
    }
    const size_t MinRead = 4096;
    size_t tryReadSize = (std::max)(n, MinRead);
    size_t currSize = m_streambuffer.size();
    m_streambuffer.resize(m_streambuffer.size() + tryReadSize);
    size_t actualReadSize = m_stream->Read(m_streambuffer.data() + currSize, tryReadSize, context);
    m_streambuffer.resize(currSize + actualReadSize);
    return AvailableBytes();
  }

  void AvroStreamReader::Discard()
  {
    constexpr size_t MinimumReleaseMemory = 128 * 1024;
    if (m_pos.Offset < MinimumReleaseMemory)
    {
      return;
    }
    const size_t availableBytes = AvailableBytes();
    std::memmove(&m_streambuffer[0], &m_streambuffer[m_pos.Offset], availableBytes);
    m_streambuffer.resize(availableBytes);
    m_pos.Offset = 0;
  }

  const AvroSchema AvroSchema::StringSchema(AvroDatumType::String);
  const AvroSchema AvroSchema::BytesSchema(AvroDatumType::Bytes);
  const AvroSchema AvroSchema::IntSchema(AvroDatumType::Int);
  const AvroSchema AvroSchema::LongSchema(AvroDatumType::Long);
  const AvroSchema AvroSchema::FloatSchema(AvroDatumType::Float);
  const AvroSchema AvroSchema::DoubleSchema(AvroDatumType::Double);
  const AvroSchema AvroSchema::BoolSchema(AvroDatumType::Bool);
  const AvroSchema AvroSchema::NullSchema(AvroDatumType::Null);

  AvroSchema AvroSchema::RecordSchema(
      std::string name,
      const std::vector<std::pair<std::string, AvroSchema>>& fieldsSchema)
  {
    AvroSchema recordSchema(AvroDatumType::Record);
    recordSchema.m_name = std::move(name);
    recordSchema.m_status = std::make_shared<SharedStatus>();
    for (auto& i : fieldsSchema)
    {
      recordSchema.m_status->m_keys.push_back(i.first);
      recordSchema.m_status->m_schemas.push_back(i.second);
    }
    return recordSchema;
  }

  AvroSchema AvroSchema::ArraySchema(AvroSchema elementSchema)
  {
    AvroSchema arraySchema(AvroDatumType::Array);
    arraySchema.m_status = std::make_shared<SharedStatus>();
    arraySchema.m_status->m_schemas.push_back(std::move(elementSchema));
    return arraySchema;
  }

  AvroSchema AvroSchema::MapSchema(AvroSchema elementSchema)
  {
    AvroSchema mapSchema(AvroDatumType::Map);
    mapSchema.m_status = std::make_shared<SharedStatus>();
    mapSchema.m_status->m_schemas.push_back(std::move(elementSchema));
    return mapSchema;
  }

  AvroSchema AvroSchema::UnionSchema(std::vector<AvroSchema> schemas)
  {
    AvroSchema unionSchema(AvroDatumType::Union);
    unionSchema.m_status = std::make_shared<SharedStatus>();
    unionSchema.m_status->m_schemas = std::move(schemas);
    return unionSchema;
  }

  AvroSchema AvroSchema::FixedSchema(std::string name, int64_t size)
  {
    AvroSchema fixedSchema(AvroDatumType::Fixed);
    fixedSchema.m_name = std::move(name);
    fixedSchema.m_status = std::make_shared<SharedStatus>();
    fixedSchema.m_status->m_size = size;
    return fixedSchema;
  }

  void AvroDatum::Fill(AvroStreamReader& reader, const Core::Context& context)
  {
    m_data = reader.m_pos;
    if (m_schema.Type() == AvroDatumType::String || m_schema.Type() == AvroDatumType::Bytes)
    {
      int64_t stringSize = reader.ParseInt(context);
      reader.Advance(static_cast<size_t>(stringSize), context);
    }
    else if (
        m_schema.Type() == AvroDatumType::Int || m_schema.Type() == AvroDatumType::Long
        || m_schema.Type() == AvroDatumType::Enum)
    {
      reader.ParseInt(context);
    }
    else if (m_schema.Type() == AvroDatumType::Float)
    {
      reader.Advance(4, context);
    }
    else if (m_schema.Type() == AvroDatumType::Double)
    {
      reader.Advance(8, context);
    }
    else if (m_schema.Type() == AvroDatumType::Bool)
    {
      reader.Advance(1, context);
    }
    else if (m_schema.Type() == AvroDatumType::Null)
    {
      reader.Advance(0, context);
    }
    else if (m_schema.Type() == AvroDatumType::Record)
    {
      for (const auto& s : m_schema.FieldSchemas())
      {
        AvroDatum(s).Fill(reader, context);
      }
    }
    else if (m_schema.Type() == AvroDatumType::Array)
    {
      while (true)
      {
        int64_t numElementsInBlock = reader.ParseInt(context);
        if (numElementsInBlock == 0)
        {
          break;
        }
        else if (numElementsInBlock < 0)
        {
          int64_t blockSize = reader.ParseInt(context);
          reader.Advance(static_cast<size_t>(blockSize), context);
        }
        else
        {
          for (auto i = 0; i < numElementsInBlock; ++i)
          {
            AvroDatum(m_schema.ItemSchema()).Fill(reader, context);
          }
        }
      }
    }
    else if (m_schema.Type() == AvroDatumType::Map)
    {
      while (true)
      {
        int64_t numElementsInBlock = reader.ParseInt(context);
        if (numElementsInBlock == 0)
        {
          break;
        }
        else if (numElementsInBlock < 0)
        {
          int64_t blockSize = reader.ParseInt(context);
          reader.Advance(static_cast<size_t>(blockSize), context);
        }
        else
        {
          for (int64_t i = 0; i < numElementsInBlock; ++i)
          {
            AvroDatum(AvroSchema::StringSchema).Fill(reader, context);
            AvroDatum(m_schema.ItemSchema()).Fill(reader, context);
          }
        }
      }
    }
    else if (m_schema.Type() == AvroDatumType::Union)
    {
      int64_t i = reader.ParseInt(context);
      AvroDatum(m_schema.FieldSchemas()[static_cast<size_t>(i)]).Fill(reader, context);
    }
    else if (m_schema.Type() == AvroDatumType::Fixed)
    {
      reader.Advance(m_schema.Size(), context);
    }
    else
    {
      AZURE_UNREACHABLE_CODE();
    }
  }

  void AvroDatum::Fill(AvroStreamReader::ReaderPos& data)
  {
    m_data = data;
    if (m_schema.Type() == AvroDatumType::String || m_schema.Type() == AvroDatumType::Bytes)
    {
      int64_t stringSize = parseInt(data);
      data.Offset += static_cast<size_t>(stringSize);
    }
    else if (
        m_schema.Type() == AvroDatumType::Int || m_schema.Type() == AvroDatumType::Long
        || m_schema.Type() == AvroDatumType::Enum)
    {
      parseInt(data);
    }
    else if (m_schema.Type() == AvroDatumType::Float)
    {
      data.Offset += 4;
    }
    else if (m_schema.Type() == AvroDatumType::Double)
    {
      data.Offset += 8;
    }
    else if (m_schema.Type() == AvroDatumType::Bool)
    {
      data.Offset += 1;
    }
    else if (m_schema.Type() == AvroDatumType::Null)
    {
      data.Offset += 0;
    }
    else if (m_schema.Type() == AvroDatumType::Record)
    {
      for (const auto& s : m_schema.FieldSchemas())
      {
        AvroDatum(s).Fill(data);
      }
    }
    else if (m_schema.Type() == AvroDatumType::Array)
    {
      while (true)
      {
        int64_t numElementsInBlock = parseInt(data);
        if (numElementsInBlock == 0)
        {
          break;
        }
        else if (numElementsInBlock < 0)
        {
          int64_t blockSize = parseInt(data);
          data.Offset += static_cast<size_t>(blockSize);
        }
        else
        {
          for (auto i = 0; i < numElementsInBlock; ++i)
          {
            AvroDatum(m_schema.ItemSchema()).Fill(data);
          }
        }
      }
    }
    else if (m_schema.Type() == AvroDatumType::Map)
    {
      while (true)
      {
        int64_t numElementsInBlock = parseInt(data);
        if (numElementsInBlock == 0)
        {
          break;
        }
        else if (numElementsInBlock < 0)
        {
          int64_t blockSize = parseInt(data);
          data.Offset += static_cast<size_t>(blockSize);
        }
        else
        {
          for (int64_t i = 0; i < numElementsInBlock; ++i)
          {
            AvroDatum(AvroSchema::StringSchema).Fill(data);
            AvroDatum(m_schema.ItemSchema()).Fill(data);
          }
        }
      }
    }
    else if (m_schema.Type() == AvroDatumType::Union)
    {
      int64_t i = parseInt(data);
      AvroDatum(m_schema.FieldSchemas()[static_cast<size_t>(i)]).Fill(data);
    }
    else if (m_schema.Type() == AvroDatumType::Fixed)
    {
      data.Offset += m_schema.Size();
    }
    else
    {
      AZURE_UNREACHABLE_CODE();
    }
  }

  template <> AvroDatum::StringView AvroDatum::Value() const
  {
    auto data = m_data;
    if (m_schema.Type() == AvroDatumType::String || m_schema.Type() == AvroDatumType::Bytes)
    {
      const int64_t length = parseInt(data);
      const uint8_t* start = &(*data.BufferPtr)[data.Offset];
      StringView ret{start, static_cast<size_t>(length)};
      data.Offset += static_cast<size_t>(length);
      return ret;
    }
    if (m_schema.Type() == AvroDatumType::Fixed)
    {
      const size_t fixedSize = m_schema.Size();
      const uint8_t* start = &(*data.BufferPtr)[data.Offset];
      StringView ret{start, fixedSize};
      data.Offset += fixedSize;
      return ret;
    }
    AZURE_UNREACHABLE_CODE();
  }

  template <> std::string AvroDatum::Value() const
  {
    auto stringView = Value<StringView>();
    return std::string(stringView.Data, stringView.Data + stringView.Length);
  }

  template <> std::vector<uint8_t> AvroDatum::Value() const
  {
    auto stringView = Value<StringView>();
    return std::vector<uint8_t>(stringView.Data, stringView.Data + stringView.Length);
  }

  template <> int64_t AvroDatum::Value() const
  {
    auto data = m_data;
    return parseInt(data);
  }

  template <> int32_t AvroDatum::Value() const { return static_cast<int32_t>(Value<int64_t>()); }

  template <> bool AvroDatum::Value() const { return Value<int64_t>(); }

  template <> std::nullptr_t AvroDatum::Value() const { return nullptr; }

  template <> AvroRecord AvroDatum::Value() const
  {
    auto data = m_data;

    AvroRecord r;
    r.m_keys = &m_schema.FieldNames();
    for (const auto& schema : m_schema.FieldSchemas())
    {
      auto datum = AvroDatum(schema);
      datum.Fill(data);
      r.m_values.push_back(std::move(datum));
    }

    return r;
  }

  template <> AvroMap AvroDatum::Value() const
  {
    auto data = m_data;

    AvroMap m;
    while (true)
    {
      int64_t numElementsInBlock = parseInt(data);
      if (numElementsInBlock == 0)
      {
        break;
      }
      if (numElementsInBlock < 0)
      {
        numElementsInBlock = -numElementsInBlock;
        parseInt(data);
      }
      for (int64_t i = 0; i < numElementsInBlock; ++i)
      {
        auto keyDatum = AvroDatum(AvroSchema::StringSchema);
        keyDatum.Fill(data);
        auto valueDatum = AvroDatum(m_schema.ItemSchema());
        valueDatum.Fill(data);
        m[keyDatum.Value<std::string>()] = valueDatum;
      }
    }
    return m;
  }

  template <> AvroDatum AvroDatum::Value() const
  {
    auto data = m_data;
    if (m_schema.Type() == AvroDatumType::Union)
    {
      int64_t i = parseInt(data);
      auto datum = AvroDatum(m_schema.FieldSchemas()[static_cast<size_t>(i)]);
      datum.Fill(data);
      return datum;
    }
    AZURE_UNREACHABLE_CODE();
  }

  AvroObjectContainerReader::AvroObjectContainerReader(Core::IO::BodyStream& stream)
      : m_reader(std::make_unique<AvroStreamReader>(stream))
  {
  }

  AvroDatum AvroObjectContainerReader::NextImpl(
      const AvroSchema* schema,
      const Core::Context& context)
  {
    AZURE_ASSERT_FALSE(m_eof);
    static const auto SyncMarkerSchema = AvroSchema::FixedSchema("Sync", 16);
    if (!schema)
    {
      static AvroSchema FileHeaderSchema = []() {
        std::vector<std::pair<std::string, AvroSchema>> fieldsSchema;
        fieldsSchema.push_back(std::make_pair("magic", AvroSchema::FixedSchema("Magic", 4)));
        fieldsSchema.push_back(
            std::make_pair("meta", AvroSchema::MapSchema(AvroSchema::BytesSchema)));
        fieldsSchema.push_back(std::make_pair("sync", SyncMarkerSchema));
        return AvroSchema::RecordSchema("org.apache.avro.file.Header", std::move(fieldsSchema));
      }();
      auto fileHeaderDatum = AvroDatum(FileHeaderSchema);
      fileHeaderDatum.Fill(*m_reader, context);
      auto fileHeader = fileHeaderDatum.Value<AvroRecord>();
      if (fileHeader.Field("magic").Value<std::string>() != "Obj\01")
      {
        throw std::runtime_error("Invalid Avro object container magic.");
      }
      AvroMap meta = fileHeader.Field("meta").Value<AvroMap>();
      std::string objectSchemaJson = meta["avro.schema"].Value<std::string>();
      std::string codec = "null";
      if (meta.count("avro.codec") != 0)
      {
        codec = meta["avro.codec"].Value<std::string>();
      }
      if (codec != "null")
      {
        throw std::runtime_error("Unsupported Avro codec: " + codec);
      }
      m_syncMarker = fileHeader.Field("sync").Value<std::string>();
      m_objectSchema = std::make_unique<AvroSchema>(ParseSchemaFromJsonString(objectSchemaJson));
      schema = m_objectSchema.get();
    }

    if (m_remainingObjectInCurrentBlock == 0)
    {
      m_reader->Discard();
      m_remainingObjectInCurrentBlock = m_reader->ParseInt(context);
      int64_t ObjectsSize = m_reader->ParseInt(context);
      m_reader->Preload(static_cast<size_t>(ObjectsSize), context);
    }

    auto objectDatum = AvroDatum(*m_objectSchema);
    objectDatum.Fill(*m_reader, context);
    if (--m_remainingObjectInCurrentBlock == 0)
    {
      auto markerDatum = AvroDatum(SyncMarkerSchema);
      markerDatum.Fill(*m_reader, context);
      auto marker = markerDatum.Value<std::string>();
      if (marker != m_syncMarker)
      {
        throw std::runtime_error("Sync marker doesn't match.");
      }
      m_eof = m_reader->TryPreload(1, context) == 0;
    }
    return objectDatum;
  }

  size_t AvroStreamParser::OnRead(
      uint8_t* buffer,
      size_t count,
      Azure::Core::Context const& context)
  {
    if (m_parserBuffer.Length != 0)
    {
      size_t bytesToCopy = (std::min)(m_parserBuffer.Length, count);
      std::memcpy(buffer, m_parserBuffer.Data, bytesToCopy);
      m_parserBuffer.Data += bytesToCopy;
      m_parserBuffer.Length -= bytesToCopy;
      return bytesToCopy;
    }
    while (!m_parser.End())
    {
      auto datum = m_parser.Next(context);
      if (datum.Schema().Type() == AvroDatumType::Union)
      {
        datum = datum.Value<AvroDatum>();
      }
      if (datum.Schema().Type() != AvroDatumType::Record)
      {
        continue;
      }
      if (datum.Schema().Name() == "com.microsoft.azure.storage.queryBlobContents.resultData")
      {
        auto record = datum.Value<AvroRecord>();
        auto dataDatum = record.Field("data");
        m_parserBuffer = dataDatum.Value<AvroDatum::StringView>();
        return OnRead(buffer, count, context);
      }
      if (datum.Schema().Name() == "com.microsoft.azure.storage.queryBlobContents.progress"
          && m_progressCallback)
      {
        auto record = datum.Value<AvroRecord>();
        auto bytesScanned = record.Field("bytesScanned").Value<int64_t>();
        auto totalBytes = record.Field("totalBytes").Value<int64_t>();
        m_progressCallback(bytesScanned, totalBytes);
      }
      if (datum.Schema().Name() == "com.microsoft.azure.storage.queryBlobContents.error"
          && m_errorCallback)
      {
        auto record = datum.Value<AvroRecord>();
        BlobQueryError e;
        e.Name = record.Field("name").Value<std::string>();
        e.Description = record.Field("description").Value<std::string>();
        e.IsFatal = record.Field("fatal").Value<bool>();
        e.Position = record.Field("position").Value<int64_t>();
        m_errorCallback(std::move(e));
      }
    }
    return 0;
  }
}}}} // namespace Azure::Storage::Blobs::_detail
