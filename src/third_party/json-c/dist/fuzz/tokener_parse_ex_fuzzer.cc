#include <stdint.h>

#include <json.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *data1 = reinterpret_cast<const char *>(data);
	json_tokener *tok = json_tokener_new();
	json_object *obj = json_tokener_parse_ex(tok, data1, size);
	
	json_object_object_foreach(jobj, key, val) {
		(void)json_object_get_type(val);
		(void)json_object_get_string(val);
	}
	(void)json_object_to_json_string(obj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
	
	json_object_put(obj);
	json_tokener_free(tok);
	return 0;
}
