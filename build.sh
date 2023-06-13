#!/usr/bin/env bash

set -euo pipefail

build_mode="${1:-release}"

cd "$(dirname "$0")"

ANDROID_NDK_HOME=./android-ndk-r23b
export PATH=${PATH}:${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin

rm -rf native/libs
mkdir -p native/libs

( cat << EOF
arm64-v8a aarch64-linux-android31-clang++
armeabi-v7a armv7a-linux-androideabi31-clang++
x86 i686-linux-android31-clang++
x86_64 x86_64-linux-android31-clang++
EOF
) | while read line; do
    ARCH="$(echo $line | awk '{ print $1 }')"
    CXX="$(echo $line | awk '{ print $2 }')"
    if [ ! -z "$ARCH" ]; then
        mkdir -p "native/libs/static/${ARCH}"
        ${CXX} \
    native/jni/main.cpp \
    native/jni/logging.cpp native/jni/utils.cpp \
    -static \
    -std=c++17 \
    -o "native/libs/static/${ARCH}/magic-mount"
    fi
done


( cat << EOF
arm64-v8a aarch64-linux-android31-clang++
armeabi-v7a armv7a-linux-androideabi31-clang++
x86 i686-linux-android31-clang++
x86_64 x86_64-linux-android31-clang++
EOF
) | while read line; do
    ARCH="$(echo $line | awk '{ print $1 }')"
    CXX="$(echo $line | awk '{ print $2 }')"
    if [ ! -z "$ARCH" ]; then
        mkdir -p "native/libs/dynamic/${ARCH}"
        ${CXX} \
    native/jni/main.cpp \
    native/jni/logging.cpp native/jni/utils.cpp \
    -stdlib=libc++ \
    -lc++abi \
    -std=c++17 \
    -o "native/libs/dynamic/${ARCH}/magic-mount"
    fi
done

