#!/bin/bash

set -x

cd ${TRAVIS_BUILD_DIR}/..
if [ ! -d mxe/.git ] ; then
	git clone git://github.com/mxe/mxe
fi
cd mxe
git checkout 994ad   # this is the tested working version

# create our config file
/usr/bin/cat <<'EOT' > settings.mk
JOBS := 4
MXE_TARGETS := i686-w64-mingw32.shared
LOCAL_PKG_LIST := libxml2 libxslt libusb1 qt5 nsis libzip
.DEFAULT local-pkg-list:
local-pkg-list: $(LOCAL_PKG_LIST)
EOT

/usr/bin/cat settings.mk

make -j 4

