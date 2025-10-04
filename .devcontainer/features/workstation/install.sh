#!/usr/bin/env bash

USER=$(_CONTAINER_USER)

cat ./setup.sh >/workspace-setup.sh
sudo chmod a+x /workspace-setup.sh
