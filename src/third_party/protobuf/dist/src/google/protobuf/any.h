// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GOOGLE_PROTOBUF_ANY_H__
#define GOOGLE_PROTOBUF_ANY_H__

#include <string>

#include "google/protobuf/port.h"
#include "google/protobuf/arenastring.h"
#include "google/protobuf/message_lite.h"

// Must be included last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {

class FieldDescriptor;
class Message;

namespace internal {

// "google.protobuf.Any".
PROTOBUF_EXPORT extern const char kAnyFullTypeName[];
// "type.googleapis.com/".
PROTOBUF_EXPORT extern const char kTypeGoogleApisComPrefix[];
// "type.googleprod.com/".
PROTOBUF_EXPORT extern const char kTypeGoogleProdComPrefix[];

std::string GetTypeUrl(absl::string_view message_name,
                       absl::string_view type_url_prefix);

// Helper class used to implement google::protobuf::Any.
class PROTOBUF_EXPORT AnyMetadata {
  typedef ArenaStringPtr UrlType;
  typedef ArenaStringPtr ValueType;
 public:
  // AnyMetadata does not take ownership of "type_url" and "value".
  constexpr AnyMetadata(UrlType* type_url, ValueType* value)
      : type_url_(type_url), value_(value) {}
  AnyMetadata(const AnyMetadata&) = delete;
  AnyMetadata& operator=(const AnyMetadata&) = delete;

  // Packs a message using the default type URL prefix: "type.googleapis.com".
  // The resulted type URL will be "type.googleapis.com/<message_full_name>".
  // Returns false if serializing the message failed.
  template <typename T>
  bool PackFrom(Arena* arena, const T& message) {
    return InternalPackFrom(arena, message, kTypeGoogleApisComPrefix,
                            T::FullMessageName());
  }

  bool PackFrom(Arena* arena, const Message& message);

  // Packs a message using the given type URL prefix. The type URL will be
  // constructed by concatenating the message type's full name to the prefix
  // with an optional "/" separator if the prefix doesn't already end with "/".
  // For example, both PackFrom(message, "type.googleapis.com") and
  // PackFrom(message, "type.googleapis.com/") yield the same result type
  // URL: "type.googleapis.com/<message_full_name>".
  // Returns false if serializing the message failed.
  template <typename T>
  bool PackFrom(Arena* arena, const T& message,
                absl::string_view type_url_prefix) {
    return InternalPackFrom(arena, message, type_url_prefix,
                            T::FullMessageName());
  }

  bool PackFrom(Arena* arena, const Message& message,
                absl::string_view type_url_prefix);

  // Unpacks the payload into the given message. Returns false if the message's
  // type doesn't match the type specified in the type URL (i.e., the full
  // name after the last "/" of the type URL doesn't match the message's actual
  // full name) or parsing the payload has failed.
  template <typename T>
  bool UnpackTo(T* message) const {
    return InternalUnpackTo(T::FullMessageName(), message);
  }

  bool UnpackTo(Message* message) const;

  // Checks whether the type specified in the type URL matches the given type.
  // A type is considered matching if its full name matches the full name after
  // the last "/" in the type URL.
  template <typename T>
  bool Is() const {
    return InternalIs(T::FullMessageName());
  }

 private:
  bool InternalPackFrom(Arena* arena, const MessageLite& message,
                        absl::string_view type_url_prefix,
                        absl::string_view type_name);
  bool InternalUnpackTo(absl::string_view type_name,
                        MessageLite* message) const;
  bool InternalIs(absl::string_view type_name) const;

  UrlType* type_url_;
  ValueType* value_;
};

// Get the proto type name from Any::type_url value. For example, passing
// "type.googleapis.com/rpc.QueryOrigin" will return "rpc.QueryOrigin" in
// *full_type_name. Returns false if the type_url does not have a "/"
// in the type url separating the full type name.
//
// NOTE: this function is available publicly as a static method on the
// generated message type: google::protobuf::Any::ParseAnyTypeUrl()
bool ParseAnyTypeUrl(absl::string_view type_url, std::string* full_type_name);

// Get the proto type name and prefix from Any::type_url value. For example,
// passing "type.googleapis.com/rpc.QueryOrigin" will return
// "type.googleapis.com/" in *url_prefix and "rpc.QueryOrigin" in
// *full_type_name. Returns false if the type_url does not have a "/" in the
// type url separating the full type name.
bool ParseAnyTypeUrl(absl::string_view type_url, std::string* url_prefix,
                     std::string* full_type_name);

// See if message is of type google.protobuf.Any, if so, return the descriptors
// for "type_url" and "value" fields.
bool GetAnyFieldDescriptors(const Message& message,
                            const FieldDescriptor** type_url_field,
                            const FieldDescriptor** value_field);

}  // namespace internal
}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"

#endif  // GOOGLE_PROTOBUF_ANY_H__
