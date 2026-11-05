# ncnn-android-shaft

[![Maven Central](https://img.shields.io/maven-central/v/io.github.wangwang-code/ncnn-android-shaft)](https://central.sonatype.com/artifact/io.github.wangwang-code/ncnn-android-shaft)
[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

专为 [Pixiv-Shaft](https://github.com/CeuiLiSA/Pixiv-Shaft) 定制裁剪并高度统一动态链接的轻量级 NCNN Android 运行库，已正式发布至 **Maven Central**。

---

## 🎯 核心优化与指标

| 指标 | 传统独立打包方案 (Pixiv-Shaft 历史存量) | Shaft 统一动态 AAR (`ncnn-android-shaft`) | 优化幅度 |
| :--- | :--- | :--- | :--- |
| **arm64 Native 总未压缩体积** | **32.9 MB** | **~10.5 MB** | **⬇️ 68.1% (净减 22.4 MB)** |
| **APK 安装包压缩体积** | 历史基准 | 整体 APK 减小 | **⬇️ 约 9.3 MB** |
| **NCNN 内核实例数** | **3 份** (NCNN + RealCUGAN + RIFE 各自静态内联) | **全局唯 1 份** (`libncnn.so`) | **消除 2 份完全冗余的静态副本** |
| **16 KB 页面对齐** | 部分手工修补 | **全链路强制 `-Wl,-z,max-page-size=16384`** | 100% 符合 Android 15+ 上架规范 |
| **C++ 运行时依赖** | 需额外携带 `libc++_shared.so` (1.37MB) | **静态链接 `libc++_static`** | **彻底消除外部 `libc++_shared.so`** |
| **OpenMP 依赖** | 历史残留无用 `libomp.so` (1.23MB) | **静态内联 OpenMP 运行时** | **彻底消除独立 `libomp.so`** |
| **依赖仓库分发** | 散落 Git 仓库 `app/libs/` | **Maven Central 官方源 (全球 CDN 分发)** | 彻底清空 Git 历史二进制负担 |

---

## 🛠️ 技术关键点与架构设计

Pixiv-Shaft 内部集成了 4 款基于 NCNN 与 Vulkan 的 AI 特性：
1. **Real-ESRGAN / RealSR**：通用动漫超分，提升图片画质；
2. **Real-CUGAN**：B 站动漫专用超分，线稿锐利，且带 `TILE %d` 实时瓦片分块进度输出；
3. **RIFE**：动图/Ugoira AI 插帧核心，支持 2x/4x 均匀插帧；
4. **Rembg**：AI 智能抠图（支持 U2NetP 与 ISNet-Anime 双模型）。

### 历史痛点
- 过去各个 CLI 工具在不同时期分别由不同维护者编译，`realcugan-ncnn-vulkan` 和 `rife-ncnn-vulkan` 采用了默认配置，静态打包了完整的 NCNN 引擎与 OpenMP，导致 APK 中被打入了 **3 份完整的 NCNN Vulkan 内核**，每个 CLI 膨胀至 7.5MB~7.7MB；
- 原本存量还需要依赖未 strip 的 `libc++_shared.so`（9.3MB）与未被链接的 `libomp.so`（1.23MB）。

### 本项目的解决方案
- **全局单核动态链接**：将 Tencent NCNN 编译为单一的高性能动态库 `libncnn.so`（启用 Vulkan 加速、静态 OpenMP、16KB 页面对齐）；
- **轻量化 CLI**：为 RealCUGAN、RIFE、RealSR、Rembg 开启 `-DUSE_SYSTEM_NCNN=ON`，所有计算内核与 Vulkan Pipeline 统一动态复用宿主 `libncnn.so`。每个 CLI 二进制急剧缩减至 **200KB ~ 450KB**；
- **真实瓦片进度注入**：在 RealCUGAN 处理循环中注入 `TILE %d` 标准错误流输出，与 Pixiv-Shaft 前端进度环无缝对接。

---

## 📦 如何在项目中引入

本库已发布至 **Maven Central**，无需配置任何第三方 JitPack 或 GitHub 镜像源。

### 1. `gradle/libs.versions.toml`

```toml
[versions]
# 支持动态版本语法，自动解析并拉取最新构建，规避本地构建缓存
ncnn = "20241226-shaft-+"

[libraries]
ncnn = { module = "io.github.wangwang-code:ncnn-android-shaft", version.ref = "ncnn" }
```

### 2. `app/build.gradle`

```groovy
dependencies {
    implementation libs.ncnn
}
```

引入后即可彻底删除工程中 `app/libs/arm64-v8a/` 目录下的所有相关 `.so` 文件。

---

## ⚡ 自动化构建与发布

- **CI 工作流**：基于 GitHub Actions (`.github/workflows/build-aar.yml`)，一键执行全自动化源码克隆、NDK 交叉编译、Strip 符号剥离、AAR 打包与 GPG 签名；
- **Maven 发布**：通过 `publish_central.py` 自动化上传至 Sonatype Central Portal API 并自动流转发布；
- **产物缓存极速发布**：支持 `reuse_cached_aar` 模式，在非 Native 变动时跳过漫长的 C++ 编译，秒级完成发版。
