// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace _internal {

  enum class XmlNodeType
  {
    StartTag,
    EndTag,
    Text,
    Attribute,
    End,
  };

  struct XmlNode final
  {
    explicit XmlNode(XmlNodeType type, std::string name = std::string())
        : Type(type), Name(std::move(name))
    {
    }

    explicit XmlNode(XmlNodeType type, std::string name, std::string value)
        : Type(type), Name(std::move(name)), Value(std::move(value)), HasValue(true)
    {
    }

    XmlNodeType Type;
    std::string Name;
    std::string Value;
    bool HasValue = false;
  };

  class XmlReader final {
  public:
    explicit XmlReader(const char* data, size_t length);
    XmlReader(const XmlReader& other) = delete;
    XmlReader& operator=(const XmlReader& other) = delete;
    XmlReader(XmlReader&& other) noexcept;
    XmlReader& operator=(XmlReader&& other) noexcept;
    ~XmlReader();

    XmlNode Read();

  private:
    struct XmlReaderContext;
    std::unique_ptr<XmlReaderContext> m_context;
  };

  class XmlWriter final {
  public:
    explicit XmlWriter();
    XmlWriter(const XmlWriter& other) = delete;
    XmlWriter& operator=(const XmlWriter& other) = delete;
    XmlWriter(XmlWriter&& other) noexcept;
    XmlWriter& operator=(XmlWriter&& other) noexcept;
    ~XmlWriter();

    void Write(XmlNode node);

    std::string GetDocument();

  private:
    struct XmlWriterContext;
    std::unique_ptr<XmlWriterContext> m_context;
  };

}}} // namespace Azure::Storage::_internal
