#!/bin/bash
set -e
SCRIPT_PATH=$( cd "$(dirname "${BASH_SOURCE[0]}")" ; pwd )

set -x
$SCRIPT_PATH/test_model_basic
$SCRIPT_PATH/test_model_checkpoint
$SCRIPT_PATH/test_model_rts
$SCRIPT_PATH/test_model_transaction
$SCRIPT_PATH/test_model_workload
