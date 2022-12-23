#!/bin/sh -ex
wget https://github.com/macports/macports-base/releases/download/v2.8.0/MacPorts-2.8.0-12-Monterey.pkg
sudo installer -pkg ./MacPorts-2.8.0-12-Monterey.pkg -target /

export PATH=$PATH:/opt/local/bin
sudo port install cmake +universal ninja ccache p7zip
wget https://github.com/ColorsWind/FFmpeg-macOS/releases/download/n5.0.1-patch3/FFmpeg-shared-n5.0.1-OSX-universal.zip
unzip FFmpeg-shared-n5.0.1-OSX-universal.zip -d FFmpeg-shared-n5.0.1-OSX-universal
# copy ffmpeg to /usr/local
cp -rv FFmpeg-shared-n5.0.1-OSX-universal/* /usr/local
sudo port install libsdl2 +universal openssl +universal openssl3 +universal
sudo port install moltenvk 
sudo port install pcre2 +universal harfbuzz +universal freetype +universal
sudo port install llvm-11 +universal
# compile qt5 from source
git clone git://code.qt.io/qt/qt5.git
cd qt5
git checkout 5.15.2
perl init-repository
cd ..
mkdir qt5-build-arm64
cd qt5-build-arm64
../qt5/configure -device-option QMAKE_APPLE_DEVICE_ARCHS=arm64 -opensource -confirm-license -nomake examples -nomake tests -no-openssl -securetransport -prefix /usr/local/Qt-5.15.2-arm
make -j8
sudo make install
cd ..
mkdir qt5-build-x86_64
cd qt5-build-x86_64
../qt5/configure -opensource -confirm-license -nomake examples -nomake tests -no-openssl -securetransport
make -j8
sudo make install
cd ..
mkdir qt5-build-universal
git clone https://github.com/nedrysoft/makeuniversal
cd makeuniversal
wget https://raw.githubusercontent.com/crystalidea/macdeployqt-universal/main/bin/makeuniversal
chmod +x makeuniversal
./makeuniversal "../qt5-build-universal" "/usr/local/Qt-5.15.2" "/usr/local/Qt-5.15.2-arm"


# compile vulkan loader
git clone https://github.com/KhronosGroup/Vulkan-Loader
cd Vulkan-Loader
mkdir build
cd build
cmake -DUPDATE_DEPS=ON -DVULKAN_HEADERS_INSTALL_DIR=/opt/local -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64" ..
make -j8
sudo make install
cd ../..
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
