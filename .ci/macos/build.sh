#!/bin/bash -ex

set -o pipefail

export Qt5_DIR=$(pwd)/Qt-5.15.2-universal
# export PATH="/opt/local/libexec:$PATH"
export PATH=$PATH:/opt/local/bin
# ccache configurations
export CCACHE_CPP2=yes
export CCACHE_SLOPPINESS=time_macros

export CC="ccache clang"
export CXX="ccache clang++"
export OBJC="clang"
export ASM="clang"
ccache -s

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QT_TRANSLATION=ON \
    -DCITRA_ENABLE_COMPATIBILITY_REPORTING=${ENABLE_COMPATIBILITY_REPORTING:-"OFF"} \
    -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON \
    -DUSE_DISCORD_PRESENCE=ON \
    -DENABLE_FFMPEG_AUDIO_DECODER=ON \
    -DENABLE_FFMPEG_VIDEO_DUMPER=ON \
    -DCMAKE_OSX_ARCHITECTURES="x86_64" \
    -GNinja
ninja  
# copy to build-64
cd ..
mkdir build-64 && cd build-64
cp -r ../build/* .
# build arm64 
cd ..
mkdir build-arm64 && cd build-arm64
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QT_TRANSLATION=ON \
    -DCITRA_ENABLE_COMPATIBILITY_REPORTING=${ENABLE_COMPATIBILITY_REPORTING:-"OFF"} \
    -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON \
    -DUSE_DISCORD_PRESENCE=ON \
    -DENABLE_FFMPEG_AUDIO_DECODER=ON \
    -DENABLE_FFMPEG_VIDEO_DUMPER=ON \
    -DCMAKE_OSX_ARCHITECTURES="arm64" \
    -GNinja
ninja
cd ..
lipo -create build-64/bin/Release/citra-qt.app/Contents/MacOS/citra-qt build-arm64/bin/Release/citra-qt.app/Contents/MacOS/citra-qt -output build/bin/Release/citra-qt.app/Contents/MacOS/citra-qt

ccache -s

ctest -VV -C Release
