# ncnn-android-shaft

[![Maven Central](https://img.shields.io/maven-central/v/io.github.wangwang-code/ncnn-android-shaft)](https://central.sonatype.com/artifact/io.github.wangwang-code/ncnn-android-shaft)
[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)
[![Android 15 Ready](https://img.shields.io/badge/Android%2015-16%20KB%20Page%20Aligned-success.svg)](https://developer.android.com/guide/practices/page-sizes)
[![Build Status](https://github.com/wangwang-code/ncnn-android-shaft/actions/workflows/build-aar.yml/badge.svg)](https://github.com/wangwang-code/ncnn-android-shaft/actions/workflows/build-aar.yml)

专为 [Pixiv-Shaft](https://github.com/CeuiLiSA/Pixiv-Shaft) 定制裁剪并极致优化的 NCNN Android 原生运行库。采用独创的**「分而治之」双运行时架构**与 **Zero-Glslang 离线着色器引擎**，在彻底杜绝画质拉丝、黑屏与瓦片段错误崩溃的前提下，实现体积断崖式压缩与全链路 Android 15 16 KB 页面对齐合规。

---

## 🎯 核心指标实测对比（以 PixShaft 4.9.9 官方发版 APK 为基准）

所有对比数据均基于官方正式发行版 [`PixShaft_4.9.9_classic.apk`](https://github.com/CeuiLiSA/Pixiv-Shaft/releases/tag/v4.9.9) 内部实装的 `lib/arm64-v8a/` 原生动态库与最新 `ncnn-android-shaft` 交付产物实测计量：

| 计量维度 | 官方 4.9.9 APK 存量方案<br>(`PixShaft_4.9.9_classic.apk`) | ncnn-android-shaft<br>(双运行时 + Zero-Glslang 方案) | 优化幅度 / 真实收益 |
| :--- | :--- | :--- | :--- |
| **NCNN Native 总未压缩体积** | **30.55 MB** (32,028,768 B) | **5.18 MB** (5,432,456 B) | **⬇️ 83.0% (净减 25.36 MB)** |
| **APK / AAR 压缩内嵌体积** | **9.98 MB** (10,463,929 B) | **2.32 MB** (2,432,205 B，AAR 全包 2.32 MB) | **⬇️ 76.8% (净减 7.66 MB)** |
| **CLI 二进制冗余** | Real-CUGAN, RIFE, RealSR 各自静态内嵌完整 NCNN | 统一动态链接 `libncnn` / `libncnn_anime` | **各 CLI 从 5~7 MB 骤降至 300~500 KB (⬇️ 93%)** |
| **着色器编译与冷启动** | 运行时依赖 ~1.5 MB `glslang` 编译库在线解析 GLSL | **Zero-Glslang**：NDK 原生 `glslc` 离线预编译 + zlib 流解压 | **剔除 1.5 MB 编译器代码，运行时冷启动编译 0 开销** |
| **Android 15 页面对齐** | 部分手工修补 | **全链路 `-Wl,-z,max-page-size=16384`** (所有 7 个 SO 对齐均为 `0x4000`) | **100% 符合 Google Play Android 15 强制规范** |
| **渲染保真度与稳定性** | 易产生光流拉丝与 Tile 切片越界崩溃 | **100% 逐比特 (Bit-for-bit) 原生无损对齐** | **彻底杜绝 RIFE 补帧拉丝、输出黑屏与 Tile 崩溃** |
| **依赖仓库分发** | 散落 Git 仓库 `app/libs/` 污染历史提交 | **Maven Central 官方源 (全球 CDN 分发)** | **彻底清空 Git 仓库二进制包袱** |

---

### 📊 逐文件实测对比清单（4.9.9 APK vs ncnn-android-shaft）

> 注：4.9.9 APK 中的 NCNN 动态库已经过 `strip`，以下为 `PixShaft_4.9.9_classic.apk` 与本项目的直接实测对比：

| 动态库文件 | 职责定位 | 4.9.9 APK 未压缩 | 4.9.9 APK 压缩 | ncnn-android-shaft 未压缩 | ncnn-android-shaft 压缩 | 未压缩降幅 | 压缩降幅 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`librealcugan_ncnn.so`** | Real-CUGAN 动漫超分 CLI | 7.15 MB (7,496,432 B) | 2.12 MB (2,223,254 B) | **0.45 MB (471,368 B)** | **0.16 MB (171,054 B)** | **⬇️ 93.7%** | **⬇️ 92.3%** |
| **`librife_ncnn.so`** | RIFE v4.6 动图插帧 CLI | 7.37 MB (7,728,880 B) | 2.16 MB (2,261,223 B) | **0.55 MB (579,656 B)** | **0.18 MB (188,711 B)** | **⬇️ 92.5%** | **⬇️ 91.7%** |
| **`librealsr_ncnn.so`** | Real-ESRGAN 通用动漫超分 CLI | 5.15 MB (5,402,304 B) | 2.01 MB (2,107,438 B) | **0.36 MB (375,600 B)** | **0.13 MB (138,715 B)** | **⬇️ 93.0%** | **⬇️ 93.4%** |
| **`librembg_ncnn.so`** | rembg 智能抠图 CLI | 0.19 MB (203,272 B) | 0.07 MB (76,242 B) | **0.10 MB (101,448 B)** | **0.05 MB (53,607 B)** | **⬇️ 50.1%** | **⬇️ 29.7%** |
| **`libomp.so`** | OpenMP 共享运行时 | 0.93 MB (973,944 B) | 0.32 MB (339,519 B) | **0.20 MB (214,216 B)** | **0.07 MB (71,021 B)** | **⬇️ 78.0%** | **⬇️ 79.1%** |
| **`libncnn.so`** | 通用图像 NCNN 运行时基线 | 9.75 MB (10,223,936 B) | 3.30 MB (3,456,253 B) | **1.57 MB (1,647,728 B)** | **0.79 MB (832,776 B)** | **⬇️ 83.9%** | **⬇️ 75.9%** |
| **`libncnn_anime.so`** | 二次元与时序特化 NCNN 运行时 | *(4.9.9 无此特化库)* | *(4.9.9 无此特化库)* | **1.95 MB (2,042,440 B)** | **0.93 MB (976,321 B)** | *新增独立特化库* | *新增独立特化库* |
| **总计 (NCNN 整体技术栈)** | **全部 7 个原生二进制** | **30.55 MB** (32,028,768 B) | **9.98 MB** (10,463,929 B) | **5.18 MB** (5,432,456 B) | **2.32 MB** (2,432,205 B) | **⬇️ 83.0% (净减 25.36 MB)** | **⬇️ 76.8% (净减 7.66 MB)** |

> 注：在旧版 4.9.9 中，仅单一 `libncnn.so` 便高达 9.75 MB；而在新版架构中，即使将**通用内核 (`libncnn`)** 与**二次元特化内核 (`libncnn_anime`)** 两者相加，合计也仅 3.52 MB，远小于旧版单一内核。

---

## 🏛️ 双 NCNN 运行时架构（分而治之）

Pixiv-Shaft 内部深度集成了 4 项前沿视觉 AI 特性：
1. **Real-CUGAN**：B 站二次元动漫专用超分，线稿锐利，支持 `TILE %d` 细粒度瓦片分块进度反馈；
2. **RIFE v4.6**：动图与 Ugoira 专用 AI 补帧，支持 2x / 4x 均匀光流插帧；
3. **Real-ESRGAN / RealSR**：通用动漫超分，提升噪点丰富的大图画质；
4. **Rembg**：AI 智能一键抠图（覆盖 U2NetP 与 ISNet-Anime 双模型）。

### 为什么必须「分而治之」？（核心技术机理）

不同 AI 视觉算法对底层推理引擎与 Vulkan GPU 显存排布有着截然不同的假设，强行合并到单一大一统版本会导致致命的画质受损或崩溃：

```
                         ┌────────────────────────────────────────────────────────┐
                         │               ncnn-android-shaft AAR                   │
                         └───────────────────────────┬────────────────────────────┘
                                                     │
                     ┌───────────────────────────────┴───────────────────────────────┐
                     ▼                                                               ▼
        ┌─────────────────────────┐                                     ┌─────────────────────────┐
        │       libncnn.so        │                                     │    libncnn_anime.so     │
        │  (通用稳定基线 v20250503) │                                     │ (2022-07-28 官方二次元基线) │
        └────────────┬────────────┘                                     └────────────┬────────────┘
                     │                                                               │
             ┌───────┴───────┐                                               ┌───────┴───────┐
             ▼               ▼                                               ▼               ▼
       librealsr_ncnn   librembg_ncnn                                 librealcugan_ncnn  librife_ncnn
        (通用动漫超分)    (智能抠图)                                    (二次元CUGAN超分) (动图RIFE补帧)
```

1. **`libncnn_anime.so`（二次元与时序特化运行时）**：
   - **锁定官方稳定基线**：nihui 官方 submodule 锁定的 **2022-07-28 原生基线**；
   - **杜绝补帧拉丝花屏**：RIFE 的光流变换（Warp Shader）深度依赖 NCNN 经典的 8 通道对齐特性（`opt.use_shader_pack8`）。新版 NCNN 已废弃该机制，若强行升级将破坏光流像素采样几何，产生严重的画面横向拉丝与撕裂；
   - **杜绝 Tile 切片越界段错误**：Real-CUGAN 的切片瓦片推理与 Winograd 卷积在 2022 基线下内存跨度（stride/padding）与边界 Staging Buffer 达到最稳固咬合；
   - **消解冗余巨幅减负**：使 Real-CUGAN 和 RIFE 从原本各占 ~7.5 MB 的静态内嵌独立程序，暴降为仅 ~460 KB / ~566 KB 的轻量 CLI，两者共享单一 1.95 MB 特化动态库，净省 14.5 MB+ 二进制冗余。

2. **`libncnn.so`（通用图像运行时）**：
   - **现代化官方基线**：Tencent/ncnn Release Tag **`20250503`**；
   - **服务对象**：`librealsr_ncnn.so` 与 `librembg_ncnn.so`；
   - **纯净系统级依赖**：对齐 Pixiv-Shaft 长期检验的生产级推理逻辑，彻底关闭未引用的 70+ 现代算子，提供极其轻便稳健的底层支撑。

---

## ⚡ 全栈极致优化手段剖析

为了在保证 100% 原生画质与稳定性的前提下榨干每一字节，本项目实施了系统性的全栈裁剪：

### 1. Zero-Glslang 离线 SPIR-V 编译与流式解压
- **剔除 glslang 运行时编译器**：原版 NCNN 在移动端开启 Vulkan 时必须内嵌静态编译库 `glslang`（占 ~1.5 MB 机器码）用于启动时动态将 GLSL 文本编译为 SPIR-V。本项目开发 `scripts/zero_glslang_builder.py`，在构建期直接调用 Android NDK 原生 `glslc` 离线预编译为原生 SPIR-V 字节码；
- **预编译 26 个前后处理与自定义 Filter 着色器**：全量覆盖 Real-CUGAN（6 个）、Real-ESRGAN（4 个）、RIFE（13 个，含 flow_avg, temporal_avg, timestep, warp, warp_pack4, warp_pack8）及公共着色器，全部以字节数组紧凑内嵌；
- **zlib Level 9 紧凑压缩与单例寻址**：通过 Manifest 仅收录移动端白名单算子，将 SPIR-V 字节流经 zlib 压缩存储入只读数据段（`.rodata`），在 GPU 初始化时单例一次性流式寻址解压，使着色器静态体积缩减约 70%；
- **通道排布算子精准放行**：通过正则豁免 `^(packing|cast)_`，精准放行 `packing_pack4to8` 等通道变换立交桥算子，既剔除了重型卷积 pack8 算子，又彻底消除了 `shader module stripped` 运行时告警与 RIFE 输出黑屏隐患（PSNR 达到无穷大，逐 bit 一致）。

### 2. 算子与汇编外科手术式抽脂（Pruning & Stripping）
- **70+ 无用 Layer 彻底切除**：通过 `-DWITH_LAYER_*=OFF` 批量剔除 RNN/LSTM、3D 卷积（conv3d/pooling3d）、变形卷积（deformableconv2d）、目标检测后处理（yolo/priorbox/roialign/roipooling）、Transformer 多头注意力（multiheadattention）等全量死算子；
- **卷积大核展开切除**：设置 `-DNCNN_CONV_5X5_7X7=0` 与 `-DNCNN_CONV_2X2_4X4=0`，二次元与超分模型仅需 3x3 卷积，剔除大量无用模板展开代码；
- **Pooling 算子抽脂**：切除非移动端多通道 Tree-Reduction 全局池化（移除 18 个冗余 shader）与自适应池化；
- **Deconvolution 算子抽脂**：切除直接滑动窗口反卷积与 4x4s1 汇编展开，仅保留 Winograd 与通用卷积展开；
- **Interp 算子抽脂**：定点剔除 Nearest 最近邻展开分支；
- **非移动端指令集切除**：剥离 ARM v8.6 SVE / SVE2 / BF16 / I8MM 指令扩展，剥离桌面 GPU Vulkan Cooperative Matrix，剥离多余的 C API 导出包装层。

### 3. OpenMP 终极轻量化：Tencent simpleomp 替代官方 LLVM libomp
- 原生 Android NDK 官方未裁剪的 `libomp.so` 高达 **7.10 MB**，即使经旧版构建裁剪后仍有 **973 KB**；
- 采用 Tencent 开源的 `simpleomp` 极简微型 OpenMP 运行时，在保持标准 ABI 符号兼容（`#pragma omp parallel for` 等）的前提下，将 `libomp.so` 压缩至 **214,216 字节（~209 KB）**；
- 统一动态依赖闭环：所有 6 个二进制（2 个 NCNN 核心 + 4 个 CLI）统一动态共享该微型库，杜绝静态重复内嵌。

### 4. CLI 软件分支剥离与瘦身
- **剔除 `process_cpu` 纯软件推理分支**：移动端在生产环境中 100% 运行于 Vulkan GPU 硬件管道，剔除 CLI 内部庞大的纯 CPU 图像处理与矩阵计算代码，使各 CLI 代码段体积缩减近 50%；
- **剥离静态内嵌 Google `libwebp`**：原版 CLI 静态链接了完整的 libwebp 编解码库，统一剥离后复用系统通道与 stb/原生位图流，消除重复第三方库体积。

### 5. 编译与链接器极限压榨
- **LLVM ThinLTO 全局跨模块优化**：全链路开启 `-flto=thin`，跨编译单元消除未引用符号与死函数；
- **相同代码折叠（ICF）**：链接期开启 `-Wl,--icf=all`，折叠模板展开产生的数百个相同字节码指令段；
- **内联控制抑制膨胀**：配置 `-Wl,-mllvm,-import-instr-limit=10` 与 `-fno-unroll-loops`，抑制 ThinLTO 在多层模板展开时的代码段暴增；
- **符号可见性收敛与 Version Script**：定制 `scripts/ncnn.sym`，强制所有内部 C++ 符号 `hidden`，禁用 RTTI（`-fno-rtti`）、C++ 异常（`-fno-exceptions`）与回溯表（`-fno-unwind-tables -fno-asynchronous-unwind-tables -Wl,--no-eh-frame-hdr`）；
- **全链路 Strip**：采用 `llvm-strip --strip-unneeded -R .comment` 彻底清除调试段与编译器注释段。

### 6. Android 15 16 KB 页面对齐合规
- 全链路强制注入 `-Wl,-z,max-page-size=16384`；
- 经 ELF 头 `p_align` 与 CI `llvm-readelf -l` 自动化审计，验证 AAR 包含的全部 7 个 ELF 动态库的 `PT_LOAD` 段最大对齐跨度均为 **`0x4000`（16384 字节）**，100% 满足 Google Play 对 Android 15+ 设备的最新合规要求。

---

## 🧪 多架构多平台真机验证矩阵

所有构建产物均在多款物理真机上完成了四大模型（Real-CUGAN, Real-ESRGAN, rembg, RIFE v4.6）的端到端真实推理测试与输出 SHA-256 逐比特校验：

| 测试设备与平台 | 芯片架构 / GPU | Android 版本 | 运行告警 | 跨芯片一致性 (Bit-for-bit) | 测试结论 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **vivo V2361A** | 高通骁龙 7s Gen 2<br>Adreno 720 (subgroup=64) | Android 14 | **0 告警** | 100% bit-for-bit 基准吻合 | **通过** (无花屏/无条纹) |
| **realme Q2i (`.102`)** | 联发科天玑 720 (MT6853)<br>Mali-G57 MC3 (subgroup=16) | Android 12 | **0 告警** | 与天玑 800U 输出 **100% 逐比特吻合** | **通过** (无黑屏/0 告警) |
| **vivo V2048A (`.120`)** | 联发科天玑 800U (MT6873)<br>Mali-G57 MC5 (subgroup=16) | Android 13 | **0 告警** | 与天玑 720 输出 **100% 逐比特吻合** | **通过** (无黑屏/0 告警) |

### 联发科两代架构实测 SHA-256 输出哈希对照

| 模型与参数配置 | 天玑 720 (Mali-G57 MC3) 输出哈希 | 天玑 800U (Mali-G57 MC5) 输出哈希 | 状态 |
| :--- | :--- | :--- | :--- |
| **Real-CUGAN** (`models-pro`, -s 2) | `70709cd44f159bedd79883d24652f393e77855731932c393424b32ecf25131ab` | `70709cd44f159bedd79883d24652f393e77855731932c393424b32ecf25131ab` | **逐比特一致** |
| **Real-ESRGAN** (`models-DF2K`, -s 4) | `82af0dc4a8667358b0c77876516303bae7b79a54849a7293a03e75e1a2bb7c1d` | `82af0dc4a8667358b0c77876516303bae7b79a54849a7293a03e75e1a2bb7c1d` | **逐比特一致** |
| **rembg** (`models-u2netp`, -g 0) | `0a1a07c166848491c7c2f9fa061df13d1c530f47581d554fa3ce77c420217819` | `0a1a07c166848491c7c2f9fa061df13d1c530f47581d554fa3ce77c420217819` | **逐比特一致** |
| **RIFE v4.6** (`rife-v4.6`, -g 0) | `7f44117d44c9f003c8bfb2d1034cb29573ff38971e768ce001caab7b38a381df` | `7f44117d44c9f003c8bfb2d1034cb29573ff38971e768ce001caab7b38a381df` | **逐比特一致** |

---

## 📦 如何在项目中引入

### 1. `gradle/libs.versions.toml`

```toml
[versions]
ncnn-shaft = "20241226-shaft-1.0.0"

[libraries]
ncnn-android-shaft = { module = "io.github.wangwang-code:ncnn-android-shaft", version.ref = "ncnn-shaft" }
```

### 2. `app/build.gradle.kts`

```kotlin
dependencies {
    implementation(libs.ncnn.android.shaft)
}
```

引入后即可彻底移除宿主工程中 `app/libs/arm64-v8a/` 目录下的所有遗留动态库文件，减负 Git 仓库历史体积。

---

## 🛠️ 本地编译与 CI/CD 流水线

### 本地编译

本项目所有第三方依赖（ncnn 双基线、各 CLI 源码、simpleomp、NDK 离线工具链）均已本地 Vendor 化，**不需要联网拉取任何 1.5 GB 外部模型仓库**：

```bash
# 确保已配置 ANDROID_NDK_ROOT 或 ANDROID_NDK_HOME (推荐 NDK r27c)
export ANDROID_NDK_ROOT=/path/to/android-ndk-r27c

# 一键编译 arm64-v8a AAR (输出至 dist/ncnn-android-shaft.aar)
./scripts/build-android.sh arm64-v8a 24
```

### CI/CD 双流彻底解耦

为保护 Maven Central 上传额度并支持灵活的真机验证闭环，流水线实施彻底的职责解耦：
1. **日常构建流 (`.github/workflows/build-aar.yml`)**：
   - 专职负责双 NCNN 编译、CLI 链接、ELF 依赖审计并输出 `ncnn-android-shaft-aar` Artifact；
   - **绝不自动向 Maven Central 上传**，供在真机设备上直接拉取验证画质与功能；
2. **独立发布流 (`.github/workflows/publish-central.yml`)**：
   - 支持人工手动触发（`workflow_dispatch`），填入指定版本号；
   - 自动获取已通过真机验证的成功构建产物，完成 GPG 签名与 Sonatype Central Portal API 上传流转。

---

## 📄 许可证与致谢

- 本项目遵循 **BSD 3-Clause License**；
- 感谢 [Tencent/ncnn](https://github.com/Tencent/ncnn) 提供的卓越高性能神经网络前向计算框架；
- 感谢 [nihui](https://github.com/nihui) 为移动端视觉开源生态所作出的杰出贡献；
- 感谢 [realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan)、[rife-ncnn-vulkan](https://github.com/nihui/rife-ncnn-vulkan)、[realsr-ncnn-vulkan](https://github.com/nihui/realsr-ncnn-vulkan) 与 [simpleomp](https://github.com/Tencent/ncnn/tree/master/src/simpleomp) 项目。
