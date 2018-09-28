#!/bin/bash

# this is the Linux build that runs all the tests. And Trusty is too old for
# this and the newer Qt packages for Trusty appear to be missing some components

# run Ubuntu 18.04 in a container and build / test everything from there.

set -x

# TestPreferences uses gui calls, so run a xvfb so it has something to talk to
export DISPLAY=:99.0
sh -e /etc/init.d/xvfb start

# Ugly, but keeps it running during the build
docker run -v $PWD:/workspace/subsurface --name=builder -w /workspace -d ubuntu:bionic /bin/sleep 60m

# now update things and install our dependencies
docker exec -t builder apt-get update
docker exec -t builder apt install -y \
        autoconf automake cmake g++ git libcrypto++-dev libcurl4-gnutls-dev \
        libgit2-dev libqt5qml5 libqt5quick5 libqt5svg5-dev \
        libqt5webkit5-dev libsqlite3-dev libssh2-1-dev libssl-dev libssl-dev \
        libtool libusb-1.0-0-dev libxml2-dev libxslt1-dev libzip-dev make \
        pkg-config qml-module-qtlocation qml-module-qtpositioning \
        qml-module-qtquick2 qt5-default qt5-qmake qtchooser qtconnectivity5-dev \
        qtdeclarative5-dev qtdeclarative5-private-dev qtlocation5-dev \
        qtpositioning5-dev qtscript5-dev qttools5-dev qttools5-dev-tools \
	qtquickcontrols2-5-dev
