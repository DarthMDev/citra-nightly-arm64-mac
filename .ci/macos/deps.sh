#!/bin/sh -ex
wget https://github.com/macports/macports-base/releases/download/v2.8.0/MacPorts-2.8.0-12-Monterey.pkg
sudo installer -pkg ./MacPorts-2.8.0-12-Monterey.pkg -target /

export PATH=$PATH:/opt/local/bin

sudo port install qt5 ffmpeg moltenvk vulkan-loader p7zip ccache ninja cmake +universal
pip3 install macpack

export SDL_VER=2.0.16
export FFMPEG_VER=4.4

mkdir tmp
cd tmp/

# install SDL
wget https://github.com/SachinVin/ext-macos-bin/raw/main/sdl2/sdl-${SDL_VER}.7z
7z x sdl-${SDL_VER}.7z
cp -rv $(pwd)/sdl-${SDL_VER}/* /

# install FFMPEG
wget https://github.com/SachinVin/ext-macos-bin/raw/main/ffmpeg/ffmpeg-${FFMPEG_VER}.7z
7z x ffmpeg-${FFMPEG_VER}.7z
cp -rv $(pwd)/ffmpeg-${FFMPEG_VER}/* /
