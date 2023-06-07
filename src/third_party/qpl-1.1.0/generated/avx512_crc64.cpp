#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
crc64_table_t avx512_crc64_table = {
	avx512_qplc_crc64};
}
