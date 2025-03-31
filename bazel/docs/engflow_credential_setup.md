# EngFlow Certification Installation

MongoDB uses EngFlow to enable remote execution with Bazel. This dramatically speeds up the build process, but is only available to internal MongoDB employees.

Bazel uses a wrapper script to check the credentials on each invocation, if for some reason thats not working, you can also manually perform this process with this command alternatively:

    python buildscripts/engflow_auth.py
