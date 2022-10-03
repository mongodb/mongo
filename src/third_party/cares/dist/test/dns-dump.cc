#include <sys/types.h>
#include <fcntl.h>
#ifdef _MSC_VER
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <iostream>
#include <vector>

#include "dns-proto.h"

namespace ares {

static void ShowFile(const char* filename) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    std::cerr << "Failed to open '" << filename << "'" << std::endl;
    return;
  }
  std::vector<unsigned char> contents;
  while (true) {
    unsigned char buffer[1024];
    int len = read(fd, buffer, sizeof(buffer));
    if (len <= 0) break;
    contents.insert(contents.end(), buffer, buffer + len);
  }
  std::cout << PacketToString(contents) << std::endl;
}

}  // namespace ares

int main(int argc, char* argv[]) {
  for (int ii = 1; ii < argc; ++ii) {
    ares::ShowFile(argv[ii]);
  }
  return 0;
}

