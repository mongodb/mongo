# Building Bazel from Source to target the S390X Architecture

Bazel doesn't release to the S390X architecture. To address this, MongoDB maintains our own Bazel build that we perform on our S390X development systems.

# JDK?

Bazel usually comes with a built-in JDK. However, the tooling used to build the built-in JDK doesn't support S390X. To get around this, an external JDK must be present on both the system compiling the Bazel executable itself as well as the host running Bazel as a build system.

On the MongoDB S390X Evergreen static hosts and dev hosts, the OpenJDK 11 installation exists at:

/usr/lib/jvm/java-11-openjdk-11.0.11.0.9-0.el8_3.s390x

To compile with on these platforms, the developer must set JAVA_HOME before invoking Bazel.

# Bazel v6.4.0 Compilation Steps

    curl -O -L https://github.com/bazelbuild/bazel/releases/download/6.4.0/bazel-6.4.0-dist.zip
    unzip bazel-6.4.0-dist.zip
    JAVA_HOME=/usr/lib/jvm/java-11-openjdk-11.0.11.0.9-0.el8_3.s390x ./compile.sh
