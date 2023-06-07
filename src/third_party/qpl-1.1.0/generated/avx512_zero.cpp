#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
zero_table_t avx512_zero_table = {
	avx512_qplc_zero_8u};
}
