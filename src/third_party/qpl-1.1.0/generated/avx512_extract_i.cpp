#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
extract_i_table_t avx512_extract_i_table = {
	avx512_qplc_extract_8u_i,
	avx512_qplc_extract_16u_i,
	avx512_qplc_extract_32u_i};
}
