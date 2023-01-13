#include "kms_message/kms_kmip_response.h"

#include "kms_message_private.h"
#include "kms_kmip_reader_writer_private.h"
#include "kms_kmip_result_reason_private.h"
#include "kms_kmip_result_status_private.h"

#include <stdlib.h>
#include <inttypes.h>
#include <limits.h> /* CHAR_BIT */

static bool
check_and_require_kmip (kms_response_t *res)
{
   if (res->provider != KMS_REQUEST_PROVIDER_KMIP) {
      KMS_ERROR (res, "Function requires KMIP request");
      return false;
   }
   return true;
}

/*
Example of an error message:
<ResponseMessage tag="0x42007b" type="Structure">
 <ResponseHeader tag="0x42007a" type="Structure">
  <ProtocolVersion tag="0x420069" type="Structure">
   <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
   <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
  </ProtocolVersion>
  <TimeStamp tag="0x420092" type="DateTime" value="2021-10-01T14:43:13-0500"/>
  <BatchCount tag="0x42000d" type="Integer" value="1"/>
 </ResponseHeader>
 <BatchItem tag="0x42000f" type="Structure">
  <Operation tag="0x42005c" type="Enumeration" value="10"/>
  <ResultStatus tag="0x42007f" type="Enumeration" value="1"/>
  <ResultReason tag="0x42007e" type="Enumeration" value="1"/>
  <ResultMessage tag="0x42007d" type="TextString"
value="ResultReasonItemNotFound"/>
 </BatchItem>
</ResponseMessage>
*/
static bool
kms_kmip_response_ok (kms_response_t *res)
{
   kmip_reader_t *reader = NULL;
   size_t pos;
   size_t len;
   uint32_t result_status;
   uint32_t result_reason = 0;
   const char *result_message = "";
   uint32_t result_message_len = 0;
   bool ok = false;

   reader = kmip_reader_new (res->kmip.data, res->kmip.len);

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_ResponseMessage)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResponseMessage));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_BatchItem)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_BatchItem));
      goto fail;
   }

   /* Look for optional Result Reason. */
   if (kmip_reader_find (reader,
                         KMIP_TAG_ResultReason,
                         KMIP_ITEM_TYPE_Enumeration,
                         &pos,
                         &len)) {
      if (!kmip_reader_read_enumeration (reader, &result_reason)) {
         KMS_ERROR (res, "unable to read result reason value");
         goto fail;
      }
   }

   /* Look for optional Result Message. */
   if (kmip_reader_find (reader,
                         KMIP_TAG_ResultMessage,
                         KMIP_ITEM_TYPE_TextString,
                         &pos,
                         &len)) {
      if (!kmip_reader_read_string (
             reader, (uint8_t **) &result_message, len)) {
         KMS_ERROR (res, "unable to read result message value");
         goto fail;
      }
      result_message_len = (uint32_t) len;
   }

   /* Look for required Result Status. */
   if (!kmip_reader_find (reader,
                          KMIP_TAG_ResultStatus,
                          KMIP_ITEM_TYPE_Enumeration,
                          &pos,
                          &len)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResultStatus));
      goto fail;
   }

   if (!kmip_reader_read_enumeration (reader, &result_status)) {
      KMS_ERROR (res, "unable to read result status value");
      goto fail;
   }

   if (result_status != KMIP_RESULT_STATUS_OperationSuccess) {
      KMS_ERROR (res,
                 "KMIP response error. Result Status (%" PRIu32
                 "): %s. Result Reason (%" PRIu32 "): %s. Result Message: %.*s",
                 result_status,
                 kmip_result_status_to_string (result_status),
                 result_reason,
                 kmip_result_reason_to_string (result_reason),
                 result_message_len,
                 result_message);
      goto fail;
   }

   ok = true;
fail:
   kmip_reader_destroy (reader);
   return ok;
}

/*
Example of a successful response to a Register request:
<ResponseMessage tag="0x42007b" type="Structure">
 <ResponseHeader tag="0x42007a" type="Structure">
  <ProtocolVersion tag="0x420069" type="Structure">
   <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
   <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
  </ProtocolVersion>
  <TimeStamp tag="0x420092" type="DateTime" value="2021-10-12T14:09:25-0500"/>
  <BatchCount tag="0x42000d" type="Integer" value="1"/>
 </ResponseHeader>
 <BatchItem tag="0x42000f" type="Structure">
  <Operation tag="0x42005c" type="Enumeration" value="3"/>
  <ResultStatus tag="0x42007f" type="Enumeration" value="0"/>
  <ResponsePayload tag="0x42007c" type="Structure">
   <UniqueIdentifier tag="0x420094" type="TextString" value="39"/>
  </ResponsePayload>
 </BatchItem>
</ResponseMessage>
 */
char *
kms_kmip_response_get_unique_identifier (kms_response_t *res)
{
   kmip_reader_t *reader = NULL;
   size_t pos;
   size_t len;
   char *uid = NULL;
   kms_request_str_t *nullterminated = NULL;

   if (!check_and_require_kmip (res)) {
      goto fail;
   }

   if (!kms_kmip_response_ok (res)) {
      goto fail;
   }

   reader = kmip_reader_new (res->kmip.data, res->kmip.len);
   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_ResponseMessage)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResponseMessage));
      goto fail;
   }
   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_BatchItem)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_BatchItem));
      goto fail;
   }
   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_ResponsePayload)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResponsePayload));
      goto fail;
   }
   if (!kmip_reader_find (reader,
                          KMIP_TAG_UniqueIdentifier,
                          KMIP_ITEM_TYPE_TextString,
                          &pos,
                          &len)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_UniqueIdentifier));
      goto fail;
   }

   if (!kmip_reader_read_string (reader, (uint8_t **) &uid, len)) {
      KMS_ERROR (res, "unable to read unique identifier");
      goto fail;
   }

   KMS_ASSERT (len <= SSIZE_MAX);
   nullterminated = kms_request_str_new_from_chars (uid, (ssize_t) len);

fail:
   kmip_reader_destroy (reader);
   return kms_request_str_detach (nullterminated);
}

/*
Example of a successful response to a Get request:
<ResponseMessage tag="0x42007b" type="Structure">
 <ResponseHeader tag="0x42007a" type="Structure">
  <ProtocolVersion tag="0x420069" type="Structure">
   <ProtocolVersionMajor tag="0x42006a" type="Integer" value="1"/>
   <ProtocolVersionMinor tag="0x42006b" type="Integer" value="0"/>
  </ProtocolVersion>
  <TimeStamp tag="0x420092" type="DateTime" value="2021-10-12T14:09:25-0500"/>
  <BatchCount tag="0x42000d" type="Integer" value="1"/>
 </ResponseHeader>
 <BatchItem tag="0x42000f" type="Structure">
  <Operation tag="0x42005c" type="Enumeration" value="10"/>
  <ResultStatus tag="0x42007f" type="Enumeration" value="0"/>
  <ResponsePayload tag="0x42007c" type="Structure">
   <ObjectType tag="0x420057" type="Enumeration" value="7"/>
   <UniqueIdentifier tag="0x420094" type="TextString" value="39"/>
   <SecretData tag="0x420085" type="Structure">
    <SecretDataType tag="0x420086" type="Enumeration" value="1"/>
    <KeyBlock tag="0x420040" type="Structure">
     <KeyFormatType tag="0x420042" type="Enumeration" value="2"/>
     <KeyValue tag="0x420045" type="Structure">
      <KeyMaterial tag="0x420043" type="ByteString" value="..."/>
     </KeyValue>
    </KeyBlock>
   </SecretData>
  </ResponsePayload>
 </BatchItem>
</ResponseMessage>
*/
uint8_t *
kms_kmip_response_get_secretdata (kms_response_t *res, size_t *secretdatalen)
{
   kmip_reader_t *reader = NULL;
   size_t pos;
   size_t len;
   uint8_t *secretdata = NULL;
   uint8_t *tmp;

   if (!check_and_require_kmip (res)) {
      goto fail;
   }

   if (!kms_kmip_response_ok (res)) {
      goto fail;
   }

   reader = kmip_reader_new (res->kmip.data, res->kmip.len);

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_ResponseMessage)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResponseMessage));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_BatchItem)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_BatchItem));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_ResponsePayload)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_ResponsePayload));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_SecretData)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_SecretData));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_KeyBlock)) {
      KMS_ERROR (
         res, "unable to find tag: %s", kmip_tag_to_string (KMIP_TAG_KeyBlock));
      goto fail;
   }

   if (!kmip_reader_find_and_recurse (reader, KMIP_TAG_KeyValue)) {
      KMS_ERROR (
         res, "unable to find tag: %s", kmip_tag_to_string (KMIP_TAG_KeyValue));
      goto fail;
   }

   if (!kmip_reader_find (reader,
                          KMIP_TAG_KeyMaterial,
                          KMIP_ITEM_TYPE_ByteString,
                          &pos,
                          &len)) {
      KMS_ERROR (res,
                 "unable to find tag: %s",
                 kmip_tag_to_string (KMIP_TAG_KeyMaterial));
      goto fail;
   }

   if (!kmip_reader_read_bytes (reader, &tmp, len)) {
      KMS_ERROR (res, "unable to read secretdata bytes");
      goto fail;
   }
   secretdata = malloc (len);
   memcpy (secretdata, tmp, len);
   *secretdatalen = len;

fail:
   kmip_reader_destroy (reader);
   return secretdata;
}
