#!/bin/bash

# this gets executed by Travis when building an installer for Windows
# it gets started from inside the subsurface directory

bash -ex ${TRAVIS_BUILD_DIR}/packaging/windows/mxe-based-build.sh
