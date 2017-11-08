#!/bin/bash

# this gets executed by Travis when building an installer for Windows
# it gets started from inside the subsurface directory

bash -ex subsurface/packaging/windows/mxe-based-build.sh
