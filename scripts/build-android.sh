#!/usr/bin/env bash
set -e

# ==============================================================================
# build-android.sh - Dual-Runtime NCNN & CLI Tools Builder for Android (arm64-v8a)
# ==============================================================================
#
# 【架构重点说明 / Architecture Notes】
# 本工程采取「分而治之」的精准双 NCNN 运行时架构：
# 1. libncnn.so (通用库，基于 Tencent/ncnn v20250503 稳定基线):
#    - 专职服务于 realsr-ncnn (超分) 与 rembg-ncnn (智能抠图)；
#    - 稳定承接 4.9.9 既有生产环境的推理算子。
#
# 2. libncnn_anime.so (二次元特化库，基于 nihui 2022-07-28 官方稳定基线):
#    - 专职服务于 realcugan-ncnn 与 rife-ncnn；
#    - 100% 原生保留 opt.use_shader_pack8 通道对齐特性，杜绝视频补帧拉丝花屏；
#    - 100% 原生保留 Winograd 卷积与 Tile 切片内存跨度，杜绝 Tile 段错误崩溃；
#    - 避免两套重型工具重复静态内联，直接净省 10MB+ 原生体积！
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
)

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

# ==============================================================================
# 2. 构建通用 NCNN 共享库: libncnn.so (Tencent/ncnn v20250503)
# ==============================================================================
echo "=== [1/6] Compiling General libncnn.so (v20250503) ==="
if [ ! -d "ncnn-general" ]; then
    echo "Cloning Tencent/ncnn (tag 20250503)..."
    git clone --depth 1 --branch 20250503 --recurse-submodules --shallow-submodules https://github.com/Tencent/ncnn.git ncnn-general
fi

mkdir -p ncnn-general/build && cd ncnn-general/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "${PRUNED_LAYER_FLAGS[@]}" \
    -DNCNN_SHARED_LIB=ON \
    -DNCNN_VULKAN=ON \
    -DNCNN_OPENMP=ON \
    -DNCNN_DISABLE_RTTI=OFF \
    -DNCNN_DISABLE_EXCEPTION=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_INSTALL_SDK=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_PIXEL_DRAWING=OFF \
    -DNCNN_INT8=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -static-openmp" \
    ..
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# 组装通用 ncnn CMake 导入包
INSTALL_GEN="${BUILD_ROOT}/install_general"
mkdir -p "${INSTALL_GEN}/lib" "${INSTALL_GEN}/include/ncnn" "${INSTALL_GEN}/lib/cmake/ncnn"
cp "${BUILD_ROOT}/ncnn-general/build/src/libncnn.so" "${INSTALL_GEN}/lib/"
cp "${BUILD_ROOT}/ncnn-general/src/"*.h "${INSTALL_GEN}/include/ncnn/"
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
# 3. 构建特例二次元 NCNN 共享库: libncnn_anime.so (nihui 2022-07-28 稳定基线)
# ==============================================================================
echo "=== [2/6] Compiling Anime-Specialized libncnn_anime.so ==="
if [ ! -d "realcugan-ncnn-vulkan" ]; then
    echo "Cloning realcugan-ncnn-vulkan with submodules..."
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/realcugan-ncnn-vulkan.git realcugan-ncnn-vulkan
fi

# 在 realcugan 自带的 2022 基线 submodule 中编译共享库，输出为 libncnn_anime.so
cd "${BUILD_ROOT}/realcugan-ncnn-vulkan/src/ncnn"
sed -i 's/cmake_minimum_required(VERSION [0-9\.]*)/cmake_minimum_required(VERSION 3.10)/g' CMakeLists.txt
if [ -f "glslang/CMakeLists.txt" ]; then
    sed -i 's/cmake_minimum_required(VERSION [0-9\.]*)/cmake_minimum_required(VERSION 3.10)/g' glslang/CMakeLists.txt
fi
# 设置动态库输出名称为 ncnn_anime，防止与通用 libncnn.so 混淆
if ! grep -q "OUTPUT_NAME \"ncnn_anime\"" src/CMakeLists.txt; then
    echo 'set_target_properties(ncnn PROPERTIES OUTPUT_NAME "ncnn_anime")' >> src/CMakeLists.txt
fi

mkdir -p build && cd build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "${PRUNED_LAYER_FLAGS[@]}" \
    -DNCNN_SHARED_LIB=ON \
    -DNCNN_VULKAN=ON \
    -DNCNN_VULKAN_ONLINE_SPIRV=OFF \
    -DNCNN_OPENMP=ON \
    -DNCNN_DISABLE_RTTI=OFF \
    -DNCNN_DISABLE_EXCEPTION=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_BUILD_TESTS=OFF \
    -DNCNN_INSTALL_SDK=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_PIXEL_DRAWING=OFF \
    -DNCNN_INT8=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -static-openmp" \
    ..
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# 组装特例 ncnn_anime CMake 导入包
INSTALL_ANIME="${BUILD_ROOT}/install_anime"
mkdir -p "${INSTALL_ANIME}/lib" "${INSTALL_ANIME}/include/ncnn" "${INSTALL_ANIME}/lib/cmake/ncnn"
cp "${BUILD_ROOT}/realcugan-ncnn-vulkan/src/ncnn/build/src/libncnn_anime.so" "${INSTALL_ANIME}/lib/"
cp "${BUILD_ROOT}/realcugan-ncnn-vulkan/src/ncnn/src/"*.h "${INSTALL_ANIME}/include/ncnn/"
cp "${BUILD_ROOT}/realcugan-ncnn-vulkan/src/ncnn/build/src/"*.h "${INSTALL_ANIME}/include/ncnn/"
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
cd "${BUILD_ROOT}/realcugan-ncnn-vulkan"
sed -i 's/set(USE_SYSTEM_NCNN OFF)/# set(USE_SYSTEM_NCNN OFF)/g' src/CMakeLists.txt
sed -i '/include.*GLSLANG_TARGET_DIR/d' src/CMakeLists.txt
sed -i '/GLSLANG_TARGET_DIR must be defined/d' src/CMakeLists.txt
sed -i 's/\${OpenMP_CXX_LIBRARIES}/-static-openmp/g' src/CMakeLists.txt
sed -i 's/set(REALCUGAN_LINK_LIBRARIES ncnn/set(REALCUGAN_LINK_LIBRARIES ncnn -static-openmp/g' src/CMakeLists.txt

# 注入精准安全的 TILE %d 单调递增行级进度（对齐 4.9.9 旧版二进制行为，消除高频 I/O 卡慢与超界溢出），并禁用 int8_storage 确保走标准 from_pixels/to_pixels 规范化管线（修复伪彩色/负片异常）
python3 -c '
with open("src/realcugan.cpp", "r") as f:
    c = f.read()

# 1. 顶部注入全局单调递增计数器
counter_def = "\nstatic int g_cugan_tile_done = 0;\n"
if "static int g_cugan_tile_done" not in c:
    c = counter_def + c

# 2. 在 process 入口处重置计数器
p_entry = "int RealCUGAN::process(const ncnn::Mat& inimage, ncnn::Mat& outimage) const\n{"
if p_entry in c:
    c = c.replace(p_entry, p_entry + "\n    g_cugan_tile_done = 0;")

# 3. 在 process_se_very_rough_stage0 的 yi 行循环末尾汇报一行进度
s0_vr_target = """        cmd.submit_and_wait();
        cmd.reset();
    }

    return 0;
}

int RealCUGAN::process_se_very_rough_sync_gap"""
s0_vr_replace = """        cmd.submit_and_wait();
        cmd.reset();

        fprintf(stderr, "TILE %d\\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_very_rough_sync_gap"""
c = c.replace(s0_vr_target, s0_vr_replace)

# 4. 在 process_se_stage0 的 yi 行循环末尾汇报一行进度
s0_target = """        cmd.submit_and_wait();
        cmd.reset();
    }

    return 0;
}

int RealCUGAN::process_se_stage2"""
s0_replace = """        cmd.submit_and_wait();
        cmd.reset();

        fprintf(stderr, "TILE %d\\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_stage2"""
c = c.replace(s0_target, s0_replace)

# 5. 在 process_se_stage2 的 yi 行循环末尾汇报一行进度
s2_target = """                }
            }
        }
    }

    return 0;
}

int RealCUGAN::process_se_sync_gap"""
s2_replace = """                }
            }
        }

        fprintf(stderr, "TILE %d\\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_sync_gap"""
c = c.replace(s2_target, s2_replace)

# 6. 在普通 process 的 yi 行循环末尾汇报一行进度
p_target = """                }
            }
        }
    }

    vkdev->reclaim_blob_allocator(blob_vkallocator);"""
p_replace = """                }
            }
        }

        fprintf(stderr, "TILE %d\\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    vkdev->reclaim_blob_allocator(blob_vkallocator);"""
c = c.replace(p_target, p_replace)

# 7. 禁用 int8 存储
c = c.replace("net.opt.use_int8_storage = true;", "net.opt.use_int8_storage = false;")

with open("src/realcugan.cpp", "w") as f:
    f.write(c)
print("Patched realcugan.cpp successfully (monotonic row-level TILE progress + disabled int8_storage).")
'
mkdir -p build && cd build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_ANIME}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN" \
    ../src
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 5. 编译 rife-ncnn-vulkan (动态链接 libncnn_anime.so，原生完美保留 pack8)
# ==============================================================================
echo "=== [4/6] Compiling rife-ncnn-vulkan ==="
if [ ! -d "rife-ncnn-vulkan" ]; then
    echo "Cloning rife-ncnn-vulkan with submodules..."
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/rife-ncnn-vulkan.git rife-ncnn-vulkan
fi
cd "${BUILD_ROOT}/rife-ncnn-vulkan"
sed -i 's/set(USE_SYSTEM_NCNN OFF)/# set(USE_SYSTEM_NCNN OFF)/g' src/CMakeLists.txt
sed -i '/include.*GLSLANG_TARGET_DIR/d' src/CMakeLists.txt
sed -i '/GLSLANG_TARGET_DIR must be defined/d' src/CMakeLists.txt
sed -i 's/\${OpenMP_CXX_LIBRARIES}/-static-openmp/g' src/CMakeLists.txt
sed -i 's/set(RIFE_LINK_LIBRARIES ncnn/set(RIFE_LINK_LIBRARIES ncnn -static-openmp/g' src/CMakeLists.txt
# 注意：严禁修改 opt.use_shader_pack8！libncnn_anime 原生支持 pack8，杜绝补帧拉丝花屏！

mkdir -p build && cd build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_ANIME}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN" \
    ../src
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 6. 编译 realsr-ncnn-vulkan (动态链接通用 libncnn.so)
# ==============================================================================
echo "=== [5/6] Compiling realsr-ncnn-vulkan ==="
if [ ! -d "realsr-ncnn-vulkan" ]; then
    echo "Cloning realsr-ncnn-vulkan with submodules..."
    git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/nihui/realsr-ncnn-vulkan.git realsr-ncnn-vulkan
fi
cd "${BUILD_ROOT}/realsr-ncnn-vulkan"
sed -i 's/set(USE_SYSTEM_NCNN OFF)/# set(USE_SYSTEM_NCNN OFF)/g' src/CMakeLists.txt
sed -i '/include.*GLSLANG_TARGET_DIR/d' src/CMakeLists.txt
sed -i '/GLSLANG_TARGET_DIR must be defined/d' src/CMakeLists.txt
sed -i 's/\${OpenMP_CXX_LIBRARIES}/-static-openmp/g' src/CMakeLists.txt

# 禁用 int8_storage 确保走标准 from_pixels/to_pixels 规范化管线（避免部分移动 GPU 驱动出现色彩异常）
python3 -c '
with open("src/realsr.cpp", "r") as f:
    c = f.read()
c = c.replace("net.opt.use_int8_storage = true;", "net.opt.use_int8_storage = false;")
with open("src/realsr.cpp", "w") as f:
    f.write(c)
print("Patched realsr.cpp (disabled int8_storage) successfully.")
'

mkdir -p build && cd build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    "-DUSE_SYSTEM_NCNN=ON" \
    "-Dncnn_DIR=${INSTALL_GEN}/lib/cmake/ncnn" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-openmp -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN" \
    ../src
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 7. 编译 rembg-ncnn (动态链接通用 libncnn.so)
# ==============================================================================
echo "=== [6/6] Compiling rembg-ncnn ==="
mkdir -p rembg-ncnn/build && cd rembg-ncnn/build
cmake "${COMMON_CMAKE_FLAGS[@]}" \
    -Dncnn_DIR="${INSTALL_GEN}/lib/cmake/ncnn" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN" \
    "${WORKSPACE_DIR}/src/rembg"
cmake --build . -j $(nproc)
cd "${BUILD_ROOT}"

# ==============================================================================
# 8. 收集产物并 Strip，自动解决动态依赖闭环
# ==============================================================================
echo "=== Packaging JNI Libs ==="
JNI_LIBS_DIR="${AAR_DIR}/jni/${ABI}"
rm -rf "${AAR_DIR}"
mkdir -p "${JNI_LIBS_DIR}"

# 通用 NCNN 运行时
cp "${INSTALL_GEN}/lib/libncnn.so" "${JNI_LIBS_DIR}/libncnn.so"

# 二次元特例 NCNN 运行时
cp "${INSTALL_ANIME}/lib/libncnn_anime.so" "${JNI_LIBS_DIR}/libncnn_anime.so"

# 各模型 CLI 执行体
cp "${BUILD_ROOT}/realcugan-ncnn-vulkan/build/realcugan-ncnn-vulkan" "${JNI_LIBS_DIR}/librealcugan_ncnn.so"
cp "${BUILD_ROOT}/rife-ncnn-vulkan/build/rife-ncnn-vulkan" "${JNI_LIBS_DIR}/librife_ncnn.so"
cp "${BUILD_ROOT}/realsr-ncnn-vulkan/build/realsr-ncnn-vulkan" "${JNI_LIBS_DIR}/librealsr_ncnn.so"
cp "${BUILD_ROOT}/rembg-ncnn/build/rembg-ncnn" "${JNI_LIBS_DIR}/librembg_ncnn.so"

# 彻底移除动态 libomp.so（libncnn 与 libncnn_anime 已内联完整运行时，各 CLI 静态链接 OpenMP 桩，无外部 libomp 依赖）
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

READELF_TOOL=$(find "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt" -name "llvm-readelf*" | head -n 1)
if [ -n "${READELF_TOOL}" ]; then
    echo "Running ELF dynamic dependency audit..."
    for so in libncnn.so libncnn_anime.so; do
        if "${READELF_TOOL}" -d "${JNI_LIBS_DIR}/${so}" 2>/dev/null | grep -q "libSPIRV.so"; then
            echo "${so} requires libSPIRV.so, bundling SPIRV and glslang..."
            find "${BUILD_ROOT}" -name "libSPIRV.so" -exec cp {} "${JNI_LIBS_DIR}/" \;
            find "${BUILD_ROOT}" -name "libglslang*.so" -exec cp {} "${JNI_LIBS_DIR}/" \;
            break
        fi
    done
fi

# 查找 llvm-strip 剥除调试符号
STRIP_TOOL=$(find "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt" -name "llvm-strip*" | head -n 1)
if [ -n "${STRIP_TOOL}" ]; then
    echo "Stripping binaries with ${STRIP_TOOL}..."
    "${STRIP_TOOL}" --strip-unneeded "${JNI_LIBS_DIR}"/*.so
fi

if [ -n "${READELF_TOOL}" ]; then
    echo "=== Verified ELF Dynamic Dependencies (DT_NEEDED) ==="
    for f in "${JNI_LIBS_DIR}"/*.so; do
        echo "-> $(basename "$f") dependencies:"
        "${READELF_TOOL}" -d "$f" | grep NEEDED || true
    done
fi

echo "=== Resulting Binaries in AAR ==="
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
