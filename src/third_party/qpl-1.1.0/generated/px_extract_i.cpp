#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
extract_i_table_t px_extract_i_table = {
	px_qplc_extract_8u_i,
	px_qplc_extract_16u_i,
	px_qplc_extract_32u_i};
}
