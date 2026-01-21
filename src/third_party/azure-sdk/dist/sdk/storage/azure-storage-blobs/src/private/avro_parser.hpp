// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_options.hpp"

#include <azure/core/io/body_stream.hpp>

#include <map>
#include <memory>
#include <type_traits>

namespace Azure { namespace Storage { namespace Blobs { namespace _detail {
  enum class AvroDatumType
  {
    String,
    Bytes,
    Int,
    Long,
    Float,
    Double,
    Bool,
    Null,
    Record,
    Enum,
    Array,
    Map,
    Union,
    Fixed,
  };

  class AvroStreamReader final {
  public:
    // position of a vector that lives through vector resizing
    struct ReaderPos final
    {
      const std::vector<uint8_t>* BufferPtr = nullptr;
      size_t Offset = 0;
    };
    explicit AvroStreamReader(Core::IO::BodyStream& stream)
        : m_stream(&stream), m_pos{&m_streambuffer, 0}
    {
    }
    AvroStreamReader(const AvroStreamReader&) = delete;
    AvroStreamReader& operator=(const AvroStreamReader&) = delete;

    int64_t ParseInt(const Core::Context& context);
    void Advance(size_t n, const Core::Context& context);
    // Read at least n bytes from m_stream and append data to m_streambuffer. Return number of bytes
    // available in m_streambuffer;
    size_t Preload(size_t n, const Core::Context& context);
    size_t TryPreload(size_t n, const Core::Context& context);
    // discards data that's before m_pos
    void Discard();

  private:
    size_t AvailableBytes() const { return m_streambuffer.size() - m_pos.Offset; }

  private:
    Core::IO::BodyStream* m_stream;
    std::vector<uint8_t> m_streambuffer;
    ReaderPos m_pos;

    friend class AvroDatum;
  };

  class AvroSchema final {
  public:
    static const AvroSchema StringSchema;
    static const AvroSchema BytesSchema;
    static const AvroSchema IntSchema;
    static const AvroSchema LongSchema;
    static const AvroSchema FloatSchema;
    static const AvroSchema DoubleSchema;
    static const AvroSchema BoolSchema;
    static const AvroSchema NullSchema;
    static AvroSchema RecordSchema(
        std::string name,
        const std::vector<std::pair<std::string, AvroSchema>>& fieldsSchema);
    static AvroSchema ArraySchema(AvroSchema elementSchema);
    static AvroSchema MapSchema(AvroSchema elementSchema);
    static AvroSchema UnionSchema(std::vector<AvroSchema> schemas);
    static AvroSchema FixedSchema(std::string name, int64_t size);

    const std::string& Name() const { return m_name; }
    AvroDatumType Type() const { return m_type; }
    const std::vector<std::string>& FieldNames() const { return m_status->m_keys; }
    AvroSchema ItemSchema() const { return m_status->m_schemas[0]; }
    const std::vector<AvroSchema>& FieldSchemas() const { return m_status->m_schemas; }
    size_t Size() const { return static_cast<size_t>(m_status->m_size); }

  private:
    explicit AvroSchema(AvroDatumType type) : m_type(type) {}

  private:
    AvroDatumType m_type;
    std::string m_name;

    struct SharedStatus
    {
      std::vector<std::string> m_keys;
      std::vector<AvroSchema> m_schemas;
      int64_t m_size = 0;
    };
    std::shared_ptr<SharedStatus> m_status;
  };

  class AvroDatum final {
  public:
    AvroDatum() : m_schema(AvroSchema::NullSchema) {}
    explicit AvroDatum(AvroSchema schema) : m_schema(std::move(schema)) {}

    void Fill(AvroStreamReader& reader, const Core::Context& context);
    void Fill(AvroStreamReader::ReaderPos& data);

    const AvroSchema& Schema() const { return m_schema; }

    template <class T> T Value() const;
    struct StringView
    {
      const uint8_t* Data = nullptr;
      size_t Length = 0;
    };

  private:
    AvroSchema m_schema;
    AvroStreamReader::ReaderPos m_data;
  };

  using AvroMap = std::map<std::string, AvroDatum>;

  class AvroRecord final {
  public:
    bool HasField(const std::string& key) const { return FindField(key) != m_keys->size(); }
    const AvroDatum& Field(const std::string& key) const { return m_values.at(FindField(key)); }
    AvroDatum& Field(const std::string& key) { return m_values.at(FindField(key)); }
    const AvroDatum& FieldAt(size_t i) const { return m_values.at(i); }
    AvroDatum& FieldAt(size_t i) { return m_values.at(i); }

  private:
    size_t FindField(const std::string& key) const
    {
      auto i = find(m_keys->begin(), m_keys->end(), key);
      return i - m_keys->begin();
    }
    const std::vector<std::string>* m_keys = nullptr;
    std::vector<AvroDatum> m_values;

    friend class AvroDatum;
  };

  class AvroObjectContainerReader final {
  public:
    explicit AvroObjectContainerReader(Core::IO::BodyStream& stream);

    bool End() const { return m_eof; }
    // Calling Next() will invalidates the previous AvroDatum returned by this function and all
    // AvroDatums propagated from there.
    AvroDatum Next(const Core::Context& context) { return NextImpl(m_objectSchema.get(), context); }

  private:
    AvroDatum NextImpl(const AvroSchema* schema, const Core::Context& context);

  private:
    std::unique_ptr<AvroStreamReader> m_reader;
    std::unique_ptr<AvroSchema> m_objectSchema;
    std::string m_syncMarker;
    int64_t m_remainingObjectInCurrentBlock = 0;
    bool m_eof = false;
  };

  class AvroStreamParser final : public Core::IO::BodyStream {
  public:
    explicit AvroStreamParser(
        std::unique_ptr<Azure::Core::IO::BodyStream> inner,
        std::function<void(int64_t, int64_t)> progressCallback,
        std::function<void(BlobQueryError)> errorCallback)
        : m_inner(std::move(inner)), m_parser(*m_inner),
          m_progressCallback(std::move(progressCallback)), m_errorCallback(std::move(errorCallback))
    {
    }

    int64_t Length() const override { return -1; }
    void Rewind() override { this->m_inner->Rewind(); }

  private:
    size_t OnRead(uint8_t* buffer, size_t count, const Azure::Core::Context& context) override;

  private:
    std::unique_ptr<Azure::Core::IO::BodyStream> m_inner;
    AvroObjectContainerReader m_parser;
    std::function<void(int64_t, int64_t)> m_progressCallback;
    std::function<void(BlobQueryError)> m_errorCallback;
    AvroDatum::StringView m_parserBuffer;
  };

}}}} // namespace Azure::Storage::Blobs::_detail
