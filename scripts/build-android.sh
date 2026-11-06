#!/usr/bin/env bash
set -e

# ==============================================================================
# build-android.sh - Dual-Runtime NCNN & CLI Tools Builder for Android (arm64-v8a)
# ==============================================================================
#
# 【架构重点说明 / Architecture Notes】
# 本工程采取自闭环、完全本地构建的双 NCNN 运行时架构：
# 1. libncnn.so (通用库，基于 Tencent/ncnn v20250503 稳定基线):
#    - 专职服务于 realsr-ncnn (超分) 与 rembg-ncnn (智能抠图)；
#    - 精准裁剪算子，杜绝体积膨胀。
#
# 2. libncnn_anime.so (二次元特化库，基于 nihui 2022-07-28 官方稳定基线):
#    - 专职服务于 realcugan-ncnn 与 rife-ncnn；
#    - 100% 原生保留 opt.use_shader_pack8 通道对齐特性，杜绝视频补帧拉丝花屏；
#    - 100% 原生保留 Winograd 卷积与 Tile 切片内存跨度，杜绝 Tile 段错误崩溃；
#    - 关闭在线 SPIR-V 编译，彻底剔除 glslang 编译器。
#
# 3. 所有 CLI（realcugan / rife / realsr / rembg）源码已全部内置于 src/：
#    - 彻底告别 1.5GB 巨型 PC 模型网络克隆，0 网络下载；
#    - 全面激活 CCache，实现高速缓存命中与秒级增量编译。
# ==============================================================================

ABI=${1:-"arm64-v8a"}
API_LEVEL=${2:-"24"}
BUILD_TYPE=${BUILD_TYPE:-"Release"}
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${WORKSPACE_DIR}/build-${ABI}"
OUTPUT_DIR="${WORKSPACE_DIR}/dist"
AAR_DIR="${WORKSPACE_DIR}/aar-staging"

echo "=== Building ncnn-android-shaft (Dual-Runtime Architecture) ==="
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
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    "-DCMAKE_CXX_FLAGS=-fno-rtti -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fvisibility=hidden -fvisibility-inlines-hidden -flto=thin"
    "-DCMAKE_C_FLAGS=-fno-unwind-tables -fno-asynchronous-unwind-tables -fvisibility=hidden -flto=thin"
    "-DCMAKE_CXX_VISIBILITY_PRESET=hidden"
    "-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON"
    "-DCMAKE_C_VISIBILITY_PRESET=hidden"
)

# 激活 CCache 并配置路径无关哈希
if command -v ccache &> /dev/null; then
    echo "Enabling ccache compiler launcher..."
    export CCACHE_COMPILERCHECK="content"
    export CCACHE_BASEDIR="${WORKSPACE_DIR}"
    export CCACHE_NOHASHDIR="true"
    COMMON_CMAKE_FLAGS+=(
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    )
fi

# 精准裁剪算子（针对 Pixiv-Shaft 全量模型: Real-CUGAN, Real-ESRGAN, RIFE, rembg）
# 禁用 70+ 个无用重型算子 (3D卷积/RNN/LSTM/Transformer多头注意力/YOLO检测/量化等)，包体断崖式缩减
PRUNED_LAYER_FLAGS=(
    -DWITH_LAYER_absval=OFF
    -DWITH_LAYER_argmax=OFF
    -DWITH_LAYER_batchnorm=OFF
    -DWITH_LAYER_bias=OFF
    -DWITH_LAYER_bnll=OFF
    -DWITH_LAYER_concat=ON
    -DWITH_LAYER_convolution=ON
    -DWITH_LAYER_crop=ON
    -DWITH_LAYER_deconvolution=ON
    -DWITH_LAYER_dropout=OFF
    -DWITH_LAYER_eltwise=ON
    -DWITH_LAYER_elu=OFF
    -DWITH_LAYER_embed=OFF
    -DWITH_LAYER_exp=OFF
    -DWITH_LAYER_flatten=ON
    -DWITH_LAYER_innerproduct=ON
    -DWITH_LAYER_input=ON
    -DWITH_LAYER_log=OFF
    -DWITH_LAYER_lrn=OFF
    -DWITH_LAYER_memorydata=ON
    -DWITH_LAYER_mvn=OFF
    -DWITH_LAYER_pooling=ON
    -DWITH_LAYER_power=OFF
    -DWITH_LAYER_prelu=ON
    -DWITH_LAYER_proposal=OFF
    -DWITH_LAYER_reduction=ON
    -DWITH_LAYER_relu=ON
    -DWITH_LAYER_reshape=ON
    -DWITH_LAYER_roipooling=OFF
    -DWITH_LAYER_scale=ON
    -DWITH_LAYER_sigmoid=ON
    -DWITH_LAYER_slice=ON
    -DWITH_LAYER_softmax=OFF
    -DWITH_LAYER_split=ON
    -DWITH_LAYER_spp=OFF
    -DWITH_LAYER_tanh=OFF
    -DWITH_LAYER_threshold=OFF
    -DWITH_LAYER_tile=OFF
    -DWITH_LAYER_rnn=OFF
    -DWITH_LAYER_lstm=OFF
    -DWITH_LAYER_binaryop=ON
    -DWITH_LAYER_unaryop=ON
    -DWITH_LAYER_convolutiondepthwise=OFF
    -DWITH_LAYER_padding=ON
    -DWITH_LAYER_squeeze=OFF
    -DWITH_LAYER_expanddims=OFF
    -DWITH_LAYER_normalize=OFF
    -DWITH_LAYER_permute=OFF
    -DWITH_LAYER_priorbox=OFF
    -DWITH_LAYER_detectionoutput=OFF
    -DWITH_LAYER_interp=ON
    -DWITH_LAYER_deconvolutiondepthwise=OFF
    -DWITH_LAYER_shufflechannel=OFF
    -DWITH_LAYER_instancenorm=OFF
    -DWITH_LAYER_clip=ON
    -DWITH_LAYER_reorg=OFF
    -DWITH_LAYER_yolodetectionoutput=OFF
    -DWITH_LAYER_quantize=OFF
    -DWITH_LAYER_dequantize=OFF
    -DWITH_LAYER_yolov3detectionoutput=OFF
    -DWITH_LAYER_psroipooling=OFF
    -DWITH_LAYER_roialign=OFF
    -DWITH_LAYER_packing=ON
    -DWITH_LAYER_requantize=OFF
    -DWITH_LAYER_cast=ON
    -DWITH_LAYER_hardsigmoid=OFF
    -DWITH_LAYER_selu=OFF
    -DWITH_LAYER_hardswish=OFF
    -DWITH_LAYER_noop=ON
    -DWITH_LAYER_pixelshuffle=ON
    -DWITH_LAYER_deepcopy=ON
    -DWITH_LAYER_mish=OFF
    -DWITH_LAYER_statisticspooling=OFF
    -DWITH_LAYER_swish=OFF
    -DWITH_LAYER_gemm=OFF
    -DWITH_LAYER_groupnorm=OFF
    -DWITH_LAYER_layernorm=OFF
    -DWITH_LAYER_softplus=OFF
    -DWITH_LAYER_gru=OFF
    -DWITH_LAYER_multiheadattention=OFF
    -DWITH_LAYER_gelu=OFF
    -DWITH_LAYER_convolution1d=OFF
    -DWITH_LAYER_pooling1d=OFF
    -DWITH_LAYER_convolutiondepthwise1d=OFF
    -DWITH_LAYER_convolution3d=OFF
    -DWITH_LAYER_convolutiondepthwise3d=OFF
    -DWITH_LAYER_pooling3d=OFF
    -DWITH_LAYER_matmul=OFF
    -DWITH_LAYER_deconvolution1d=OFF
    -DWITH_LAYER_deconvolutiondepthwise1d=OFF
    -DWITH_LAYER_deconvolution3d=OFF
    -DWITH_LAYER_deconvolutiondepthwise3d=OFF
    -DWITH_LAYER_einsum=OFF
    -DWITH_LAYER_deformableconv2d=OFF
)

mkdir -p "${BUILD_ROOT}"
cd "${BUILD_ROOT}"

# 1. 链接共享 glslang 编译器源码至通用版与特例版 NCNN
if [ ! -f "${WORKSPACE_DIR}/external/ncnn-general/glslang/CMakeLists.txt" ]; then
    rm -rf "${WORKSPACE_DIR}/external/ncnn-general/glslang"
    ln -sfn "${WORKSPACE_DIR}/external/glslang" "${WORKSPACE_DIR}/external/ncnn-general/glslang" 2>/dev/null || cp -r "${WORKSPACE_DIR}/external/glslang" "${WORKSPACE_DIR}/external/ncnn-general/glslang"
fi
if [ ! -f "${WORKSPACE_DIR}/external/ncnn-anime/glslang/CMakeLists.txt" ]; then
    rm -rf "${WORKSPACE_DIR}/external/ncnn-anime/glslang"
    ln -sfn "${WORKSPACE_DIR}/external/glslang" "${WORKSPACE_DIR}/external/ncnn-anime/glslang" 2>/dev/null || cp -r "${WORKSPACE_DIR}/external/glslang" "${WORKSPACE_DIR}/external/ncnn-anime/glslang"
fi

# 2. 链接共享 libwebp 源码至各 CLI
for mod in realcugan rife realsr; do
    if [ ! -f "${WORKSPACE_DIR}/src/${mod}/libwebp/CMakeLists.txt" ]; then
        rm -rf "${WORKSPACE_DIR}/src/${mod}/libwebp"
        ln -sfn "${WORKSPACE_DIR}/external/libwebp" "${WORKSPACE_DIR}/src/${mod}/libwebp" 2>/dev/null || cp -r "${WORKSPACE_DIR}/external/libwebp" "${WORKSPACE_DIR}/src/${mod}/libwebp"
    fi
done

# ==============================================================================
# 2. 构建通用 NCNN 共享库: libncnn.so (基于 external/ncnn-general)
# ==============================================================================
echo "=== [1/6] Compiling General libncnn.so ==="
mkdir -p ncnn-general/build && cd ncnn-general/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "${PRUNED_LAYER_FLAGS[@]}" \
    -DNCNN_SHARED_LIB=ON \
    -DNCNN_VULKAN=ON \
    -DNCNN_OPENMP=ON \
    -DNCNN_DISABLE_RTTI=ON \
    -DNCNN_DISABLE_EXCEPTION=ON \
    -DNCNN_ENABLE_LTO=ON \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_INSTALL_SDK=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_PIXEL_DRAWING=OFF \
    -DNCNN_INT8=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -static-openmp -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10 -Wl,--version-script=${WORKSPACE_DIR}/scripts/ncnn.sym" \
    "${WORKSPACE_DIR}/external/ncnn-general"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# 组装通用 ncnn CMake 导入包
INSTALL_GEN="${BUILD_ROOT}/install_general"
mkdir -p "${INSTALL_GEN}/lib" "${INSTALL_GEN}/include/ncnn" "${INSTALL_GEN}/lib/cmake/ncnn"
cp "${BUILD_ROOT}/ncnn-general/build/src/libncnn.so" "${INSTALL_GEN}/lib/"
cp "${WORKSPACE_DIR}/external/ncnn-general/src/"*.h "${INSTALL_GEN}/include/ncnn/"
cp "${BUILD_ROOT}/ncnn-general/build/src/"*.h "${INSTALL_GEN}/include/ncnn/"
cp "${INSTALL_GEN}/include/ncnn/"*.h "${INSTALL_GEN}/include/" 2>/dev/null || true

cat << EOF > "${INSTALL_GEN}/lib/cmake/ncnn/ncnnConfig.cmake"
if(NOT TARGET ncnn)
    add_library(ncnn SHARED IMPORTED)
    set_target_properties(ncnn PROPERTIES
        IMPORTED_LOCATION "${INSTALL_GEN}/lib/libncnn.so"
        INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_GEN}/include;${INSTALL_GEN}/include/ncnn"
    )
endif()
set(ncnn_FOUND TRUE)
EOF

# ==============================================================================
# 3. 构建特例二次元 NCNN 共享库: libncnn_anime.so (基于 external/ncnn-anime)
# ==============================================================================
echo "=== [2/6] Compiling Anime-Specialized libncnn_anime.so ==="
mkdir -p ncnn-anime/build && cd ncnn-anime/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "${PRUNED_LAYER_FLAGS[@]}" \
    -DNCNN_SHARED_LIB=ON \
    -DNCNN_VULKAN=ON \
    -DNCNN_VULKAN_ONLINE_SPIRV=OFF \
    -DNCNN_OPENMP=ON \
    -DNCNN_DISABLE_RTTI=ON \
    -DNCNN_DISABLE_EXCEPTION=ON \
    -DNCNN_ENABLE_LTO=ON \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_BUILD_TESTS=OFF \
    -DNCNN_INSTALL_SDK=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_PIXEL_DRAWING=OFF \
    -DNCNN_INT8=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -static-openmp -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10 -Wl,--version-script=${WORKSPACE_DIR}/scripts/ncnn.sym" \
    "${WORKSPACE_DIR}/external/ncnn-anime"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# 组装特例 ncnn_anime CMake 导入包
INSTALL_ANIME="${BUILD_ROOT}/install_anime"
mkdir -p "${INSTALL_ANIME}/lib" "${INSTALL_ANIME}/include/ncnn" "${INSTALL_ANIME}/lib/cmake/ncnn"
cp "${BUILD_ROOT}/ncnn-anime/build/src/libncnn_anime.so" "${INSTALL_ANIME}/lib/"
cp "${WORKSPACE_DIR}/external/ncnn-anime/src/"*.h "${INSTALL_ANIME}/include/ncnn/"
cp "${BUILD_ROOT}/ncnn-anime/build/src/"*.h "${INSTALL_ANIME}/include/ncnn/"
cp "${INSTALL_ANIME}/include/ncnn/"*.h "${INSTALL_ANIME}/include/" 2>/dev/null || true

cat << EOF > "${INSTALL_ANIME}/lib/cmake/ncnn/ncnnConfig.cmake"
if(NOT TARGET ncnn)
    add_library(ncnn SHARED IMPORTED)
    set_target_properties(ncnn PROPERTIES
        IMPORTED_LOCATION "${INSTALL_ANIME}/lib/libncnn_anime.so"
        INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_ANIME}/include;${INSTALL_ANIME}/include/ncnn"
    )
endif()
set(ncnn_FOUND TRUE)
EOF

# ==============================================================================
# 4. 编译 realcugan-ncnn-vulkan (动态链接 libncnn_anime.so)
# ==============================================================================
echo "=== [3/6] Compiling realcugan-ncnn-vulkan ==="
mkdir -p realcugan/build && cd realcugan/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_ANIME}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10" \
    "${WORKSPACE_DIR}/src/realcugan"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 5. 编译 rife-ncnn-vulkan (动态链接 libncnn_anime.so，原生完美保留 pack8)
# ==============================================================================
echo "=== [4/6] Compiling rife-ncnn-vulkan ==="
mkdir -p rife/build && cd rife/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_ANIME}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10" \
    "${WORKSPACE_DIR}/src/rife"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 6. 编译 realsr-ncnn-vulkan (动态链接通用 libncnn.so)
# ==============================================================================
echo "=== [5/6] Compiling realsr-ncnn-vulkan ==="
mkdir -p realsr/build && cd realsr/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_GEN}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10" \
    "${WORKSPACE_DIR}/src/realsr"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 7. 编译 rembg-ncnn (动态链接通用 libncnn.so)
# ==============================================================================
echo "=== [6/6] Compiling rembg-ncnn ==="
mkdir -p rembg-ncnn/build && cd rembg-ncnn/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    -Dncnn_DIR="${INSTALL_GEN}/lib/cmake/ncnn" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,-mllvm,-import-instr-limit=10" \
    "${WORKSPACE_DIR}/src/rembg"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 8. 收集产物并 Strip，自动解决动态依赖闭环与 16KB 页面对齐
# ==============================================================================
echo "=== Assembling and Stripping Artifacts ==="
rm -rf "${AAR_DIR}"
JNI_LIBS_DIR="${AAR_DIR}/jni/${ABI}"
mkdir -p "${JNI_LIBS_DIR}"

# 通用 NCNN 运行时
cp "${INSTALL_GEN}/lib/libncnn.so" "${JNI_LIBS_DIR}/libncnn.so"

# 二次元特例 NCNN 运行时
cp "${INSTALL_ANIME}/lib/libncnn_anime.so" "${JNI_LIBS_DIR}/libncnn_anime.so"

# 各模型 CLI 执行体
cp "${BUILD_ROOT}/realcugan/build/realcugan-ncnn-vulkan" "${JNI_LIBS_DIR}/librealcugan_ncnn.so"
cp "${BUILD_ROOT}/rife/build/rife-ncnn-vulkan" "${JNI_LIBS_DIR}/librife_ncnn.so"
cp "${BUILD_ROOT}/realsr/build/realsr-ncnn-vulkan" "${JNI_LIBS_DIR}/librealsr_ncnn.so"
cp "${BUILD_ROOT}/rembg-ncnn/build/rembg-ncnn" "${JNI_LIBS_DIR}/librembg_ncnn.so"

# 彻底移除动态 libomp.so 校验（各 CLI 静态链接 OpenMP 桩，无外部 libomp 依赖）
READELF_TOOL=$(find "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt" -name "llvm-readelf*" | head -n 1)
if [ -n "${READELF_TOOL}" ]; then
    echo "Running ELF dynamic dependency audit..."
    for so in "${JNI_LIBS_DIR}"/*.so; do
        if "${READELF_TOOL}" -d "${so}" 2>/dev/null | grep -q "libomp.so"; then
            echo "ERROR: $(basename "${so}") still has DT_NEEDED on libomp.so!"
            exit 1
        fi
    done
    echo "Audit passed: No .so depends on libomp.so!"
fi

# 查找 llvm-strip 剥除调试符号
STRIP_TOOL=$(find "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt" -name "llvm-strip*" | head -n 1)
if [ -n "${STRIP_TOOL}" ]; then
    echo "Stripping binaries with ${STRIP_TOOL}..."
    "${STRIP_TOOL}" --strip-unneeded "${JNI_LIBS_DIR}"/*.so
fi

# ==============================================================================
# 9. 构建标准 AAR 并输出 POM
# ==============================================================================
echo "=== Packaging AAR ==="
cat << 'EOF' > "${AAR_DIR}/AndroidManifest.xml"
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="ceui.pixiv.ncnn">
</manifest>
EOF

mkdir -p "${OUTPUT_DIR}"
cd "${AAR_DIR}"
zip -r "${OUTPUT_DIR}/ncnn-android-shaft.aar" AndroidManifest.xml jni/

echo "=== Build Complete! ==="
echo "Output: ${OUTPUT_DIR}/ncnn-android-shaft.aar"
ls -lh "${OUTPUT_DIR}/ncnn-android-shaft.aar"
