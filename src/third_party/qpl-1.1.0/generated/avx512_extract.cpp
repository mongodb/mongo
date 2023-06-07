#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
extract_table_t avx512_extract_table = {
	avx512_qplc_extract_8u,
	avx512_qplc_extract_16u,
	avx512_qplc_extract_32u};
}
