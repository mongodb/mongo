#include "ares-test.h"

#ifdef HAVE_CONTAINER

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <iostream>
#include <functional>
#include <string>
#include <sstream>
#include <vector>

namespace ares {
namespace test {

namespace {

struct ContainerInfo {
  ContainerFilesystem* fs_;
  std::string hostname_;
  std::string domainname_;
  VoidToIntFn fn_;
};

int EnterContainer(void *data) {
  ContainerInfo *container = (ContainerInfo*)data;

  if (verbose) {
    std::cerr << "Running function in container {chroot='"
              << container->fs_->root() << "', hostname='" << container->hostname_
              << "', domainname='" << container->domainname_ << "'}"
              << std::endl;
  }

  // Ensure we are apparently root before continuing.
  int count = 10;
  while (getuid() != 0 && count > 0) {
    usleep(100000);
    count--;
  }
  if (getuid() != 0) {
    std::cerr << "Child in user namespace has uid " << getuid() << std::endl;
    return -1;
  }
  if (!container->fs_->mountpt().empty()) {
    // We want to bind mount this inside the specified directory.
    std::string innerdir = container->fs_->root() + container->fs_->mountpt();
    if (verbose) std::cerr << " mount --bind " << container->fs_->mountpt()
                           << " " << innerdir << std::endl;
    int rc = mount(container->fs_->mountpt().c_str(), innerdir.c_str(),
                   "none", MS_BIND, 0);
    if (rc != 0) {
      std::cerr << "Warning: failed to bind mount " << container->fs_->mountpt() << " at "
                << innerdir << ", errno=" << errno << std::endl;
    }
  }

  // Move into the specified directory.
  if (chdir(container->fs_->root().c_str()) != 0) {
    std::cerr << "Failed to chdir('" << container->fs_->root()
              << "'), errno=" << errno << std::endl;
    return -1;
  }
  // And make it the new root directory;
  char buffer[PATH_MAX + 1];
  if (getcwd(buffer, PATH_MAX) == NULL) {
    std::cerr << "failed to retrieve cwd, errno=" << errno << std::endl;
    return -1;
  }
  buffer[PATH_MAX] = '\0';
  if (chroot(buffer) != 0) {
    std::cerr << "chroot('" << buffer << "') failed, errno=" << errno << std::endl;
    return -1;
  }

  // Set host/domainnames if specified
  if (!container->hostname_.empty()) {
    if (sethostname(container->hostname_.c_str(),
                    container->hostname_.size()) != 0) {
      std::cerr << "Failed to sethostname('" << container->hostname_
                << "'), errno=" << errno << std::endl;
      return -1;
    }
  }
  if (!container->domainname_.empty()) {
    if (setdomainname(container->domainname_.c_str(),
                      container->domainname_.size()) != 0) {
      std::cerr << "Failed to setdomainname('" << container->domainname_
                << "'), errno=" << errno << std::endl;
      return -1;
    }
  }

  return container->fn_();
}

}  // namespace

// Run a function while:
//  - chroot()ed into a particular directory
//  - having a specified hostname/domainname

int RunInContainer(ContainerFilesystem* fs, const std::string& hostname,
                   const std::string& domainname, VoidToIntFn fn) {
  const int stack_size = 1024 * 1024;
  std::vector<byte> stack(stack_size, 0);
  ContainerInfo container = {fs, hostname, domainname, fn};

  // Start a child process in a new user and UTS namespace
  pid_t child = clone(EnterContainer, stack.data() + stack_size,
                      CLONE_VM|CLONE_NEWNS|CLONE_NEWUSER|CLONE_NEWUTS|SIGCHLD,
                      (void *)&container);
  if (child < 0) {
    std::cerr << "Failed to clone(), errno=" << errno << std::endl;
    return -1;
  }

  // Build the UID map that makes us look like root inside the namespace.
  std::stringstream mapfiless;
  mapfiless << "/proc/" << child << "/uid_map";
  std::string mapfile = mapfiless.str();
  int fd = open(mapfile.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
  if (fd < 0) {
    std::cerr << "Failed to create '" << mapfile << "'" << std::endl;
    return -1;
  }
  std::stringstream contentss;
  contentss << "0 " << getuid() << " 1" << std::endl;
  std::string content = contentss.str();
  int rc = write(fd, content.c_str(), content.size());
  if (rc != (int)content.size()) {
    std::cerr << "Failed to write uid map to '" << mapfile << "'" << std::endl;
  }
  close(fd);

  // Wait for the child process and retrieve its status.
  int status;
  waitpid(child, &status, 0);
  if (rc <= 0) {
    std::cerr << "Failed to waitpid(" << child << ")" << std::endl;
    return -1;
  }
  if (!WIFEXITED(status)) {
    std::cerr << "Child " << child << " did not exit normally" << std::endl;
    return -1;
  }
  return status;
}

ContainerFilesystem::ContainerFilesystem(NameContentList files, const std::string& mountpt) {
  rootdir_ = TempNam(nullptr, "ares-chroot");
  mkdir(rootdir_.c_str(), 0755);
  dirs_.push_front(rootdir_);
  for (const auto& nc : files) {
    std::string fullpath = rootdir_ + nc.first;
    int idx = fullpath.rfind('/');
    std::string dir = fullpath.substr(0, idx);
    EnsureDirExists(dir);
    files_.push_back(std::unique_ptr<TransientFile>(
        new TransientFile(fullpath, nc.second)));
  }
  if (!mountpt.empty()) {
    char buffer[PATH_MAX + 1];
    if (realpath(mountpt.c_str(), buffer)) {
      mountpt_ = buffer;
      std::string fullpath = rootdir_ + mountpt_;
      EnsureDirExists(fullpath);
    }
  }
}

ContainerFilesystem::~ContainerFilesystem() {
  files_.clear();
  for (const std::string& dir : dirs_) {
    rmdir(dir.c_str());
  }
}

void ContainerFilesystem::EnsureDirExists(const std::string& dir) {
  if (std::find(dirs_.begin(), dirs_.end(), dir) != dirs_.end()) {
    return;
  }
  size_t idx = dir.rfind('/');
  if (idx != std::string::npos) {
    std::string prevdir = dir.substr(0, idx);
    EnsureDirExists(prevdir);
  }
  // Ensure this directory is in the list before its ancestors.
  mkdir(dir.c_str(), 0755);
  dirs_.push_front(dir);
}

}  // namespace test
}  // namespace ares

#endif
