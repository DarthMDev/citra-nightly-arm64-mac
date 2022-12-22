#!/bin/sh -ex
wget https://github.com/macports/macports-base/releases/download/v2.8.0/MacPorts-2.8.0-12-Monterey.pkg
sudo installer -pkg ./MacPorts-2.8.0-12-Monterey.pkg -target /

export PATH=$PATH:/opt/local/bin
sudo port install cmake ninja qt5 ccache p7zip
git clone https://github.com/FFmpeg/FFmpeg
cd FFmpeg
git checkout release/4.4
./configure --arch=arm64
make -j8
sudo make install
sudo port install libsdl2 +universal 
sudo port install moltenvk 
git clone https://github.com/KhronosGroup/Vulkan-Loader
cd Vulkan-Loader
mkdir build
cd build
cmake -DUPDATE_DEPS=ON -DVULKAN_HEADERS_INSTALL_DIR=/opt/local -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64" ..
make -j8
sudo make install
#sudo port install vulkan-loader +arm64
#sudo port install ffmpeg +arm64
# sudo port install moltenvk +arm64
pip3 install macpack

export SDL_VER=2.0.16
export FFMPEG_VER=4.4

mkdir tmp
cd tmp/

# install SDL
# wget https://github.com/SachinVin/ext-macos-bin/raw/main/sdl2/sdl-${SDL_VER}.7z
# 7z x sdl-${SDL_VER}.7z
# cp -rv $(pwd)/sdl-${SDL_VER}/* /

# # install FFMPEG
# wget https://github.com/SachinVin/ext-macos-bin/raw/main/ffmpeg/ffmpeg-${FFMPEG_VER}.7z
# 7z x ffmpeg-${FFMPEG_VER}.7z
# cp -rv $(pwd)/ffmpeg-${FFMPEG_VER}/* /
