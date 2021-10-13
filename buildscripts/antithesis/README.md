# Antithesis Docker Image Building

This directory contains `Dockerfile`s and other resources for constructing
the workload, SUT, and config docker images for use with Antithesis.

- workload: the workload image that runs resmoke
- database: the System Under Test, spawned multiple times to form the
  appropriate topology
- config: this directory, contains the above and a docker-compose.yml file for
  spawning the system
- logs: the log folders of each database node will be mounted as a subdirectory
  in logs.

Please also see evergreen/antithesis_image_build.sh, which prepares the build
contexts for building these images, and uploads them to the docker registry
