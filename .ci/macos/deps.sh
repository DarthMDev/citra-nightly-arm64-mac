#!/bin/sh -ex
brew unlink python@2 || true
rm '/usr/local/bin/2to3' || true
wget https://github.com/macports/macports-base/releases/download/v2.8.0/MacPorts-2.8.0-12-Monterey.pkg
sudo installer -pkg ./MacPorts-2.8.0-12-Monterey.pkg -target /

export PATH=$PATH:/opt/local/bin
sudo port install cmake ninja ccache p7zip
wget https://github.com/ColorsWind/FFmpeg-macOS/releases/download/n5.0.1-patch3/FFmpeg-shared-n5.0.1-OSX-universal.zip
unzip FFmpeg-shared-n5.0.1-OSX-universal.zip -d FFmpeg-shared-n5.0.1-OSX-universal
sudo cp -rv FFmpeg-shared-n5.0.1-OSX-universal/* /usr/local/
# copy to /Users/runner/work/FFmpeg-macOS/FFmpeg-macOS/ffmpeg/install_universal
mkdir -p /Users/runner/work/FFmpeg-macOS/FFmpeg-macOS/ffmpeg/install_universal
sudo cp -rv FFmpeg-shared-n5.0.1-OSX-universal/* /Users/runner/work/FFmpeg-macOS/FFmpeg-macOS/ffmpeg/install_universal
git clone https://github.com/MichaelGDev48/citra-dependencies-universal2
cd citra-dependencies-universal2
# install all .pkg dependencies
for i in *.pkg; do sudo installer -pkg $i -target /opt/local; done
cd ..
# sudo port install openssl3 +universal glslang +universal moltenvk +universal vulkan-loader +universal libsdl2 +universal
# grab qt5 universal2 binaries
wget https://github.com/MichaelGDev48/qt5.15.2-universal-binaries/releases/download/1.0/Qt-5.15.2-universal.zip 
unzip Qt-5.15.2-universal
chmod +x Qt-5.15.2-universal/bin/*
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
