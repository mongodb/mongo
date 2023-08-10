/*
 * Copyright 2021-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kms_message/kms_kmip_request.h"

#include "kms_message_private.h"
#include "kms_kmip_reader_writer_private.h"

#include <inttypes.h>
#include <stdint.h>

static void
copy_writer_buffer (kms_request_t *req, kmip_writer_t *writer)
{
   const uint8_t *buf;
   size_t buflen;

   buf = kmip_writer_get_buffer (writer, &buflen);
   req->kmip.data = malloc (buflen);
   memcpy (req->kmip.data, buf, buflen);
   req->kmip.len = (uint32_t) buflen;
}

kms_request_t *
kms_kmip_request_register_secretdata_new (void *reserved,
                                          const uint8_t *data,
                                          size_t len)
{
   /*
   Create a KMIP Register request with a 96 byte SecretData of this form:

   <RequestMessage tag="0x420078" type="Structure">
    <RequestHeader tag="0x420077" type="Structure">
     <ProtocolVersion tag="0x420069" type="Structure">
      <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
      <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
     </ProtocolVersion>
     <BatchCount tag="0x42000d" type="Integer" value="1"/>
    </RequestHeader>
    <BatchItem tag="0x42000f" type="Structure">
     <Operation tag="0x42005c" type="Enumeration" value="3"/>
     <RequestPayload tag="0x420079" type="Structure">
      <ObjectType tag="0x420057" type="Enumeration" value="7"/>
      <TemplateAttribute tag="0x420091" type="Structure">
       <Attribute tag="0x420008" type="Structure">
        <AttributeName tag="0x42000a" type="TextString" value="Cryptographic
   Usage Mask"/> <AttributeValue tag="0x42000b" type="Integer" value="0"/>
       </Attribute>
      </TemplateAttribute>
      <SecretData tag="0x420085" type="Structure">
       <SecretDataType tag="0x420086" type="Enumeration" value="2"/>
       <KeyBlock tag="0x420040" type="Structure">
        <KeyFormatType tag="0x420042" type="Enumeration" value="2"/>
        <KeyValue tag="0x420045" type="Structure">
         <KeyMaterial tag="0x420043" type="ByteString" value="..."/>
        </KeyValue>
       </KeyBlock>
      </SecretData>
     </RequestPayload>
    </BatchItem>
   </RequestMessage>
   */

   kmip_writer_t *writer;
   kms_request_t *req;

   req = calloc (1, sizeof (kms_request_t));
   req->provider = KMS_REQUEST_PROVIDER_KMIP;

   if (len != KMS_KMIP_REQUEST_SECRETDATA_LENGTH) {
      KMS_ERROR (req,
                 "expected SecretData length of %d, got %" PRIu32,
                 KMS_KMIP_REQUEST_SECRETDATA_LENGTH,
                 len);
      return req;
   }

   writer = kmip_writer_new ();
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestMessage);

   kmip_writer_begin_struct (writer, KMIP_TAG_RequestHeader);
   kmip_writer_begin_struct (writer, KMIP_TAG_ProtocolVersion);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMajor, 1);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMinor, 0);
   kmip_writer_close_struct (writer); /* KMIP_TAG_ProtocolVersion */
   kmip_writer_write_integer (writer, KMIP_TAG_BatchCount, 1);
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestHeader */

   kmip_writer_begin_struct (writer, KMIP_TAG_BatchItem);
   /* 0x03 == Register */
   kmip_writer_write_enumeration (writer, KMIP_TAG_Operation, 0x03);
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestPayload);
   /* 0x07 == SecretData */
   kmip_writer_write_enumeration (writer, KMIP_TAG_ObjectType, 0x07);
   kmip_writer_begin_struct (writer, KMIP_TAG_TemplateAttribute);
   // Add required Cryptographic Usage Mask attribute.
   {
      kmip_writer_begin_struct (writer, KMIP_TAG_Attribute);
      const char *cryptographicUsageMaskStr = "Cryptographic Usage Mask";
      kmip_writer_write_string (writer,
                                KMIP_TAG_AttributeName,
                                cryptographicUsageMaskStr,
                                strlen (cryptographicUsageMaskStr));
      // Use 0 because the Secret Data object is not used in cryptographic
      // operations on the KMIP server.
      kmip_writer_write_integer (writer, KMIP_TAG_AttributeValue, 0);
      kmip_writer_close_struct (writer);
   }
   kmip_writer_close_struct (writer); /* KMIP_TAG_TemplateAttribute */
   kmip_writer_begin_struct (writer, KMIP_TAG_SecretData);
   /* 0x02 = Seed */
   kmip_writer_write_enumeration (writer, KMIP_TAG_SecretDataType, 0x02);
   kmip_writer_begin_struct (writer, KMIP_TAG_KeyBlock);
   /* 0x02 = Opaque */
   kmip_writer_write_enumeration (writer, KMIP_TAG_KeyFormatType, 0x02);
   kmip_writer_begin_struct (writer, KMIP_TAG_KeyValue);
   kmip_writer_write_bytes (
      writer, KMIP_TAG_KeyMaterial, (const char *) data, len);
   kmip_writer_close_struct (writer); /* KMIP_TAG_KeyValue */
   kmip_writer_close_struct (writer); /* KMIP_TAG_KeyBlock */
   kmip_writer_close_struct (writer); /* KMIP_TAG_SecretData */
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestPayload */
   kmip_writer_close_struct (writer); /* KMIP_TAG_BatchItem */
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestMessage */

   copy_writer_buffer (req, writer);
   kmip_writer_destroy (writer);
   return req;
}

kms_request_t *
kms_kmip_request_activate_new (void *reserved, const char *unique_identifer)
{
   /*
   Create a KMIP Activate request of this form:
   <RequestMessage tag="0x420078" type="Structure">
    <RequestHeader tag="0x420077" type="Structure">
     <ProtocolVersion tag="0x420069" type="Structure">
      <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
      <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
     </ProtocolVersion>
     <BatchCount tag="0x42000d" type="Integer" value="1"/>
    </RequestHeader>
    <BatchItem tag="0x42000f" type="Structure">
     <Operation tag="0x42005c" type="Enumeration" value="18"/>
     <RequestPayload tag="0x420079" type="Structure">
      <UniqueIdentifier tag="0x420094" type="TextString" value="..."/>
     </RequestPayload>
    </BatchItem>
   </RequestMessage>
   */

   kmip_writer_t *writer;
   kms_request_t *req;

   req = calloc (1, sizeof (kms_request_t));
   req->provider = KMS_REQUEST_PROVIDER_KMIP;

   writer = kmip_writer_new ();
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestMessage);

   kmip_writer_begin_struct (writer, KMIP_TAG_RequestHeader);
   kmip_writer_begin_struct (writer, KMIP_TAG_ProtocolVersion);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMajor, 1);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMinor, 0);
   kmip_writer_close_struct (writer); /* KMIP_TAG_ProtocolVersion */
   kmip_writer_write_integer (writer, KMIP_TAG_BatchCount, 1);
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestHeader */

   kmip_writer_begin_struct (writer, KMIP_TAG_BatchItem);
   /* 0x0A == Get */
   kmip_writer_write_enumeration (writer, KMIP_TAG_Operation, 0x12);
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestPayload);
   kmip_writer_write_string (writer,
                             KMIP_TAG_UniqueIdentifier,
                             unique_identifer,
                             strlen (unique_identifer));
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestPayload */
   kmip_writer_close_struct (writer); /* KMIP_TAG_BatchItem */
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestMessage */

   copy_writer_buffer (req, writer);
   kmip_writer_destroy (writer);
   return req;
}

kms_request_t *
kms_kmip_request_get_new (void *reserved, const char *unique_identifer)
{
   /*
   Create a KMIP Get request of this form:
   <RequestMessage tag="0x420078" type="Structure">
    <RequestHeader tag="0x420077" type="Structure">
     <ProtocolVersion tag="0x420069" type="Structure">
      <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
      <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
     </ProtocolVersion>
     <BatchCount tag="0x42000d" type="Integer" value="1"/>
    </RequestHeader>
    <BatchItem tag="0x42000f" type="Structure">
     <Operation tag="0x42005c" type="Enumeration" value="10"/>
     <RequestPayload tag="0x420079" type="Structure">
      <UniqueIdentifier tag="0x420094" type="TextString" value="..."/>
     </RequestPayload>
    </BatchItem>
   </RequestMessage>
   */

   kmip_writer_t *writer;
   kms_request_t *req;

   req = calloc (1, sizeof (kms_request_t));
   req->provider = KMS_REQUEST_PROVIDER_KMIP;

   writer = kmip_writer_new ();
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestMessage);

   kmip_writer_begin_struct (writer, KMIP_TAG_RequestHeader);
   kmip_writer_begin_struct (writer, KMIP_TAG_ProtocolVersion);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMajor, 1);
   kmip_writer_write_integer (writer, KMIP_TAG_ProtocolVersionMinor, 0);
   kmip_writer_close_struct (writer); /* KMIP_TAG_ProtocolVersion */
   kmip_writer_write_integer (writer, KMIP_TAG_BatchCount, 1);
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestHeader */

   kmip_writer_begin_struct (writer, KMIP_TAG_BatchItem);
   /* 0x0A == Get */
   kmip_writer_write_enumeration (writer, KMIP_TAG_Operation, 0x0A);
   kmip_writer_begin_struct (writer, KMIP_TAG_RequestPayload);
   kmip_writer_write_string (writer,
                             KMIP_TAG_UniqueIdentifier,
                             unique_identifer,
                             strlen (unique_identifer));
   /* 0x01 = Raw */
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestPayload */
   kmip_writer_close_struct (writer); /* KMIP_TAG_BatchItem */
   kmip_writer_close_struct (writer); /* KMIP_TAG_RequestMessage */

   /* Copy the KMIP writer buffer to a KMIP request. */
   copy_writer_buffer (req, writer);
   kmip_writer_destroy (writer);
   return req;
}
