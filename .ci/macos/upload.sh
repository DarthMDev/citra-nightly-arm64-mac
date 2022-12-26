#!/bin/bash -ex

. .ci/common/pre-upload.sh

REV_NAME="citra-osx-${GITDATE}-${GITREV}"
ARCHIVE_NAME="${REV_NAME}.tar.gz"
COMPRESSION_FLAGS="-czvf"
export PATH=$PATH:/opt/local/bin
mkdir "$REV_NAME"

cp build/bin/Release/citra "$REV_NAME"
cp -r build/bin/Release/citra-qt.app "$REV_NAME"
cp build/bin/Release/citra-room "$REV_NAME"

BUNDLE_PATH="$REV_NAME/citra-qt.app"
BUNDLE_CONTENTS_PATH="$BUNDLE_PATH/Contents"
BUNDLE_EXECUTABLE_PATH="$BUNDLE_CONTENTS_PATH/MacOS/citra-qt"
BUNDLE_LIB_PATH="$BUNDLE_CONTENTS_PATH/lib"
BUNDLE_RESOURCES_PATH="$BUNDLE_CONTENTS_PATH/Resources"

CITRA_STANDALONE_PATH="$REV_NAME/citra"

# move libs into folder for deployment
python3 -m macpack.patcher $BUNDLE_EXECUTABLE_PATH -d "../Frameworks"
# move qt frameworks into app bundle for deployment
$(brew --prefix)/opt/qt5/bin/macdeployqt $BUNDLE_PATH -executable=$BUNDLE_EXECUTABLE_PATH

# move libs into folder for deployment
python3 -m macpack.patcher $CITRA_STANDALONE_PATH -d "libs"

# bundle MoltenVK
VULKAN_LOADER_PATH=$(brew --prefix vulkan-loader)
MOLTENVK_PATH=$(brew --prefix molten-vk)
mkdir $BUNDLE_LIB_PATH
cp $VULKAN_LOADER_PATH/lib/libvulkan.dylib $BUNDLE_LIB_PATH
cp $MOLTENVK_PATH/lib/libMoltenVK.dylib $BUNDLE_LIB_PATH
cp -r $MOLTENVK_PATH/share/vulkan $BUNDLE_RESOURCES_PATH
install_name_tool -add_rpath "@loader_path/../lib/" $BUNDLE_EXECUTABLE_PATH

# bundle MoltenVK
VULKAN_LOADER_PATH=/opt/local
MOLTENVK_PATH=/opt/local
mkdir $BUNDLE_LIB_PATH
cp $VULKAN_LOADER_PATH/lib/libvulkan.dylib $BUNDLE_LIB_PATH
cp $MOLTENVK_PATH/lib/libMoltenVK.dylib $BUNDLE_LIB_PATH
cp -r /opt/local/vulkan $BUNDLE_RESOURCES_PATH
install_name_tool -add_rpath "@loader_path/../lib/" $BUNDLE_EXECUTABLE_PATH
# workaround for libc++
install_name_tool -change @loader_path/../Frameworks/libc++.1.0.dylib /usr/lib/libc++.1.dylib $BUNDLE_EXECUTABLE_PATH
install_name_tool -change @loader_path/libs/libc++.1.0.dylib /usr/lib/libc++.1.dylib $CITRA_STANDALONE_PATH

# Make the launching script executable
chmod +x $BUNDLE_EXECUTABLE_PATH

# Verify loader instructions
find "$REV_NAME" -type f -exec otool -L {} \;

. .ci/common/post-upload.sh
