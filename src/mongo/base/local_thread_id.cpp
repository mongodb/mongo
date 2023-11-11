#include <stdint.h>

namespace mongo {
thread_local uint16_t localThreadId = 0;
}
