#!/usr/bin/env bash
set -e

# ==============================================================================
# build-android.sh - Unified NCNN + CLI Tools Builder for Android (arm64-v8a)
# ==============================================================================

ABI=${1:-"arm64-v8a"}
API_LEVEL=${2:-"24"}
BUILD_TYPE=${BUILD_TYPE:-"Release"}
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${WORKSPACE_DIR}/build-${ABI}"
OUTPUT_DIR="${WORKSPACE_DIR}/dist"
AAR_DIR="${WORKSPACE_DIR}/aar-staging"

echo "=== Building ncnn-android-shaft ==="
echo "ABI: ${ABI}"
echo "API Level: ${API_LEVEL}"
echo "Workspace: ${WORKSPACE_DIR}"
echo "Build Dir: ${BUILD_ROOT}"

if [ -z "$ANDROID_NDK_ROOT" ] && [ -n "$ANDROID_NDK_HOME" ]; then
    export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
fi
if [ -z "$ANDROID_NDK_ROOT" ]; then
    echo "Error: ANDROID_NDK_ROOT is not set!"
    exit 1
fi
echo "Using NDK: ${ANDROID_NDK_ROOT}"

TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake"
COMMON_CMAKE_FLAGS=(
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
    "-DANDROID_ABI=${ABI}"
    "-DANDROID_PLATFORM=android-${API_LEVEL}"
    "-DANDROID_STL=c++_static"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${BUILD_ROOT}/install"
)

mkdir -p "${BUILD_ROOT}"
cd "${BUILD_ROOT}"

# 1. 下载 stb 头文件供 rembg 使用
mkdir -p "${WORKSPACE_DIR}/src/rembg"
if [ ! -f "${WORKSPACE_DIR}/src/rembg/stb_image.h" ]; then
    echo "Downloading stb_image.h..."
    curl -sSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" -o "${WORKSPACE_DIR}/src/rembg/stb_image.h"
fi
if [ ! -f "${WORKSPACE_DIR}/src/rembg/stb_image_write.h" ]; then
    echo "Downloading stb_image_write.h..."
    curl -sSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h" -o "${WORKSPACE_DIR}/src/rembg/stb_image_write.h"
fi

# 2. Clone Tencent/ncnn
if [ ! -d "ncnn" ]; then
    echo "Cloning Tencent/ncnn with submodules..."
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/Tencent/ncnn.git ncnn
fi

# 3. 编译 libncnn.so
echo "=== Compiling NCNN Shared Library ==="
mkdir -p ncnn/build
cd ncnn/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    -DNCNN_VULKAN=ON \
    -DNCNN_OPENMP=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    ..
make -j$(nproc)
make install
cd "${BUILD_ROOT}"

NCNN_DIR="${BUILD_ROOT}/install/lib/cmake/ncnn"
GLSLANG_TARGET_DIR=$(find "${BUILD_ROOT}/install" -name "glslangTargets.cmake" -exec dirname {} \; | head -n 1)
echo "Found NCNN CMake: ${NCNN_DIR}"
echo "Found Glslang CMake: ${GLSLANG_TARGET_DIR}"

CLI_CMAKE_ARGS=(
    "${COMMON_CMAKE_FLAGS[@]}"
    "-DUSE_SYSTEM_NCNN=ON"
    "-Dncnn_DIR=${NCNN_DIR}"
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384"
)
if [ -n "${GLSLANG_TARGET_DIR}" ]; then
    CLI_CMAKE_ARGS+=("-DGLSLANG_TARGET_DIR=${GLSLANG_TARGET_DIR}")
fi

# 4. Clone & 编译 realcugan-ncnn-vulkan
echo "=== Compiling realcugan-ncnn-vulkan ==="
if [ ! -d "realcugan-ncnn-vulkan" ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/realcugan-ncnn-vulkan.git realcugan-ncnn-vulkan
fi
cd realcugan-ncnn-vulkan
# 注入 TILE %d 进度补丁 (保证分块进度输出)
if ! grep -q 'TILE %d' src/realcugan.cpp; then
    echo "Injecting TILE progress into realcugan.cpp..."
    sed -i '/for (int yi = 0; yi < ytiles; yi++)/a\        for (int xi = 0; xi < xtiles; xi++) { fprintf(stderr, "TILE %d\\n", yi * xtiles + xi); fflush(stderr); }' src/realcugan.cpp || true
fi
mkdir -p build && cd build
cmake "${CLI_CMAKE_ARGS[@]}" ..
make -j$(nproc)
cd "${BUILD_ROOT}"

# 5. Clone & 编译 rife-ncnn-vulkan
echo "=== Compiling rife-ncnn-vulkan ==="
if [ ! -d "rife-ncnn-vulkan" ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/rife-ncnn-vulkan.git rife-ncnn-vulkan
fi
mkdir -p rife-ncnn-vulkan/build && cd rife-ncnn-vulkan/build
cmake "${CLI_CMAKE_ARGS[@]}" ..
make -j$(nproc)
cd "${BUILD_ROOT}"

# 6. Clone & 编译 realsr-ncnn-vulkan
echo "=== Compiling realsr-ncnn-vulkan ==="
if [ ! -d "realsr-ncnn-vulkan" ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/realsr-ncnn-vulkan.git realsr-ncnn-vulkan
fi
mkdir -p realsr-ncnn-vulkan/build && cd realsr-ncnn-vulkan/build
cmake "${CLI_CMAKE_ARGS[@]}" ..
make -j$(nproc)
cd "${BUILD_ROOT}"

# 7. 编译 rembg-ncnn
echo "=== Compiling rembg-ncnn ==="
mkdir -p rembg-ncnn/build && cd rembg-ncnn/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    -Dncnn_DIR="${NCNN_DIR}" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    "${WORKSPACE_DIR}/src/rembg"
make -j$(nproc)
cd "${BUILD_ROOT}"

# 8. 收集产物并 Strip
echo "=== Packaging JNI Libs ==="
JNI_LIBS_DIR="${AAR_DIR}/jni/${ABI}"
mkdir -p "${JNI_LIBS_DIR}"

cp "${BUILD_ROOT}/install/lib/libncnn.so" "${JNI_LIBS_DIR}/"
cp "${BUILD_ROOT}/realcugan-ncnn-vulkan/build/realcugan-ncnn-vulkan" "${JNI_LIBS_DIR}/librealcugan_ncnn.so"
cp "${BUILD_ROOT}/rife-ncnn-vulkan/build/rife-ncnn-vulkan" "${JNI_LIBS_DIR}/librife_ncnn.so"
cp "${BUILD_ROOT}/realsr-ncnn-vulkan/build/realsr-ncnn-vulkan" "${JNI_LIBS_DIR}/librealsr_ncnn.so"
cp "${BUILD_ROOT}/rembg-ncnn/build/rembg-ncnn" "${JNI_LIBS_DIR}/librembg_ncnn.so"

# 查找 llvm-strip 剥除调试符号
STRIP_TOOL=$(find "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt" -name "llvm-strip*" | head -n 1)
if [ -n "${STRIP_TOOL}" ]; then
    echo "Stripping binaries with ${STRIP_TOOL}..."
    "${STRIP_TOOL}" --strip-unneeded "${JNI_LIBS_DIR}"/*.so
fi

echo "=== Resulting Binaries ==="
ls -lh "${JNI_LIBS_DIR}"

# 9. 生成 Android AAR
mkdir -p "${OUTPUT_DIR}"
cd "${AAR_DIR}"
cat << 'EOF' > AndroidManifest.xml
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="io.github.wangwang_code.ncnn">
</manifest>
EOF

AAR_OUTPUT="${OUTPUT_DIR}/ncnn-android-shaft.aar"
echo "Creating AAR: ${AAR_OUTPUT}..."
zip -r "${AAR_OUTPUT}" AndroidManifest.xml jni/

echo "=== Build Complete! AAR Size: $(stat -c%s "${AAR_OUTPUT}") bytes ==="
