#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
extract_table_t px_extract_table = {
	px_qplc_extract_8u,
	px_qplc_extract_16u,
	px_qplc_extract_32u};
}
