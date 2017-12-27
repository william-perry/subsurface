#!/bin/bash

set -x
set -e

# this gets executed by Travis when building an AppImage for Linux
# it gets started from inside the subsurface directory

export PATH=$QT_ROOT/bin:$PATH # Make sure correct qmake is found on the $PATH for linuxdeployqt
export CMAKE_PREFIX_PATH=$QT_ROOT/lib/cmake

# the global build script expects to be called from the directory ABOVE subsurface
# build both desktop and mobile - first desktop without BT support and without
# webkit to make sure that still works, then with all components in order
# to create an AppImage

bash -e -x ${TRAVIS_BUILD_DIR}/scripts/build.sh -desktop -no-bt $@

if [ "$1" = "-onlydeps" ] ; then
	exit 0
fi

rm -rf subsurface/build
bash -e -x ${TRAVIS_BUILD_DIR}/scripts/build.sh -both -create-appdir -build-with-webkit $@
