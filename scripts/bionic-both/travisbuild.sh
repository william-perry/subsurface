#!/bin/bash

set -x
set -e

docker exec -t builder subsurface/scripts/build.sh -desktop -no-bt 2>&1 | tee build.log
# fail the build if we didn't create the target binary
grep /workspace/install-root/bin/subsurface build.log

docker exec -t builder subsurface/scripts/build.sh -mobile | tee build-mobile.log
# fail the build if we didn't create the target binary
grep /workspace/install-root/bin/subsurface-mobile build-mobile.log
