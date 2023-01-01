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
BUNDLE_FRAMEWORK_PATH="$BUNDLE_CONTENTS_PATH/Frameworks"
BUNDLE_RESOURCES_PATH="$BUNDLE_CONTENTS_PATH/Resources"

CITRA_STANDALONE_PATH="$REV_NAME/citra"

# move libs into folder for deployment
macpack $BUNDLE_EXECUTABLE_PATH -d "../Frameworks"
# move qt frameworks into app bundle for deployment
$(pwd)/Qt-5.15.2-universal/bin/macdeployqt $BUNDLE_PATH -executable=$BUNDLE_EXECUTABLE_PATH

# move libs into folder for deployment
macpack $CITRA_STANDALONE_PATH -d "libs"

# bundle MoltenVK
VULKAN_LOADER_PATH=/opt/local
MOLTENVK_PATH=/opt/local
mkdir $BUNDLE_LIB_PATH
# replace SDL2 with the universal2 version , copy to frameworks directory
cp /opt/local/lib/libSDL2-2.0.0.dylib $BUNDLE_FRAMEWORK_PATH
#rename it to libSDL2-2.0
rm $BUNDLE_FRAMEWORK_PATH/libSDL2-2.0.dylib
mv $BUNDLE_FRAMEWORK_PATH/libSDL2-2.0.0.dylib $BUNDLE_FRAMEWORK_PATH/libSDL2-2.0.dylib
cp $VULKAN_LOADER_PATH/lib/libvulkan.dylib $BUNDLE_LIB_PATH
cp $MOLTENVK_PATH/lib/libMoltenVK.dylib $BUNDLE_LIB_PATH
cp -r $VULKAN_LOADER_PATH/share/vulkan $BUNDLE_RESOURCES_PATH
install_name_tool -add_rpath "@loader_path/../lib/" $BUNDLE_EXECUTABLE_PATH
# workaround for libc++
install_name_tool -change @loader_path/../Frameworks/libc++.1.0.dylib /usr/lib/libc++.1.dylib $BUNDLE_EXECUTABLE_PATH
install_name_tool -change @loader_path/libs/libc++.1.0.dylib /usr/lib/libc++.1.dylib $CITRA_STANDALONE_PATH

# Make the launching script executable
chmod +x $BUNDLE_EXECUTABLE_PATH

# Verify loader instructions
find "$REV_NAME" -type f -exec otool -L {} \;

. .ci/common/post-upload.sh
