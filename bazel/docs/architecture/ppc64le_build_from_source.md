# Building Bazel from Source to target the PPC64LE Architecture

Bazel doesn't release to the PPC64LE architecture. To address this, MongoDB maintains our own Bazel build that we perform on our PPC64LE development systems.

# JDK?

Bazel usually comes with a built-in JDK. However, the tooling used to build the built-in JDK doesn't support PPC64LE. To get around this, an external JDK must be present on both the system compiling the Bazel executable itself as well as the host running Bazel as a build system.

On the MongoDB PPC64LE Evergreen static hosts and dev hosts, the OpenJDK 21 installation exists at:

/usr/lib/jvm/java-21-openjdk

To compile with on these platforms, the developer must set JAVA_HOME before invoking Bazel.

# Bazel v7.2.1 Compilation Steps

    curl -O -L https://github.com/bazelbuild/bazel/releases/download/7.2.1/bazel-7.2.1-dist.zip
    unzip bazel-7.2.1-dist.zip
    JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./compile.sh
