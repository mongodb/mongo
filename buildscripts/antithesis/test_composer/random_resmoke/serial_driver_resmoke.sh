#!/bin/bash

cd /mongo
/scripts/run_resmoke.sh --seed $(od -vAn -N4 -tu4 </dev/urandom) --shuffle --sanityCheck
