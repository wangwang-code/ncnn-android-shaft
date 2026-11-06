# ncnn-android-shaft

[![Maven Central](https://img.shields.io/maven-central/v/io.github.wangwang-code/ncnn-android-shaft)](https://central.sonatype.com/artifact/io.github.wangwang-code/ncnn-android-shaft)
[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

专为 [Pixiv-Shaft](https://github.com/CeuiLiSA/Pixiv-Shaft) 定制裁剪并高度优化的 NCNN Android 原生运行库，采取「分而治之」双运行时架构，兼顾 100% 原生画质与极致体积压缩。

---

## 🎯 核心指标对比

| 指标 | 传统独立打包方案 (Pixiv-Shaft 4.9.9 历史存量) | Shaft 双运行时 AAR (`ncnn-android-shaft`) | 优化幅度 |
| :--- | :--- | :--- | :--- |
| **arm64 Native 总未压缩体积** | **39.85 MB** (含未 strip 调试符号) | **~9.5 MB** (全部经 `llvm-strip`) | **⬇️ 76.1% (净减 30.3 MB)** |
| **AAR 下载与压缩体积** | 约 22 MB | **~4.5 MB** | **⬇️ 79.5%** |
| **NCNN 内核冗余实例** | **3 份独立内核** (NCNN + RealCUGAN + RIFE 重复内嵌) | **2 份精准特化运行时** (`libncnn` + `libncnn_anime`) | **彻底消除重复内嵌负担** |
| **16 KB 页面对齐** | 部分手工修补 | **全链路强制 `-Wl,-z,max-page-size=16384`** | 100% 符合 Android 15+ 上架规范 |
| **画质与渲染保真度** | 基准 | **100% 原生 1:1 无损无花屏** | 杜绝横向拉丝与 Tile 段错误 |
| **依赖仓库分发** | 散落 Git 仓库 `app/libs/` | **Maven Central 官方源 (全球 CDN 分发)** | 彻底清空 Git 历史二进制负担 |

---

## 🏛️ 双 NCNN 运行时架构（分而治之）

Pixiv-Shaft 内部集成了 4 款基于 NCNN 与 Vulkan 的 AI 特性：
1. **RealSR / Real-ESRGAN**：通用动漫超分，提升图片画质；
2. **Rembg**：AI 智能抠图（支持 U2NetP 与 ISNet-Anime 双模型）；
3. **Real-CUGAN**：B 站动漫专用超分，线稿锐利，且带 `TILE %d` 实时瓦片分块进度输出；
4. **RIFE**：动图/Ugoira AI 插帧核心，支持 2x/4x 均匀插帧。

### 为什么必须「分而治之」？（重点注明 / Architecture Notes）

不同 AI 算法对底层 NCNN 引擎与 Vulkan Shader 的通道布局有着本质不同的假设：

```
                        ┌────────────────────────────────────────────────┐
                        │              ncnn-android-shaft AAR             │
                        └───────────────────────┬────────────────────────┘
                                                │
                 ┌──────────────────────────────┴──────────────────────────────┐
                 ▼                                                             ▼
    ┌─────────────────────────┐                                   ┌─────────────────────────┐
    │       libncnn.so        │                                   │    libncnn_anime.so     │
    │  (通用稳定基线 v20250503) │                                   │ (2022-07-28 官方二次元基线) │
    └────────────┬────────────┘                                   └────────────┬────────────┘
                 │                                                             │
         ┌───────┴───────┐                                             ┌───────┴───────┐
         ▼               ▼                                             ▼               ▼
   librealsr_ncnn   librembg_ncnn                               librealcugan_ncnn  librife_ncnn
    (通用超分)       (智能抠图)                                  (二次元CUGAN超分) (动图RIFE补帧)
```

1. **`libncnn.so`（通用图像运行时）**：
   - **基线版本**：Tencent/ncnn 官方 Release Tag **`20250503`**；
   - **服务对象**：`librealsr_ncnn.so`（超分）与 `librembg_ncnn.so`（抠图）；
   - **设计考量**：完全对齐 Pixiv-Shaft 4.9.9 长期稳定检验的生产级推理逻辑，关闭无用 layer，提供纯净的系统库依赖。

2. **`libncnn_anime.so`（二次元增强特例运行时）**：
   - **基线版本**：nihui 官方 submodule 锁定的 **2022-07-28 原生稳定基线**；
   - **服务对象**：`librealcugan_ncnn.so` 与 `librife_ncnn.so`；
   - **设计考量**：
     - **杜绝补帧拉丝**：100% 原生保留 `opt.use_shader_pack8` 8 元素通道向量打包特性，与 RIFE 的 Warp Shader 完美契合；
     - **杜绝 Tile 越界段错误**：100% 原生保留 Winograd 卷积与 Staging Buffer 行对齐计算，保证切片边界计算万无一失；
     - **消除 14.5MB 冗余**：使 Real-CUGAN 和 RIFE 从原本各占 7.5MB 暴降为约 800KB 的轻量 CLI，两者共享单一 2.5MB 特例动态库。

---

## ⚡ 构建与发布流水线（彻底解耦）

为了保护 Maven Central 的上传与流转审核额度，CI/CD 流水线实行严格的职责解耦：

1. **日常构建流 (`.github/workflows/build-aar.yml`)**：
   - 专职负责源码拉取、双 NCNN 编译、CLI 动态链接、ELF 依赖审计并输出 `ncnn-android-shaft-aar` Artifact；
   - **绝不自动向 Maven Central 上传**，供开发者在真机抢先拉取验证画质与功能。

2. **独立发布流 (`.github/workflows/publish-central.yml`)**：
   - 支持人工手动触发（`workflow_dispatch`），填入指定版本号；
   - 自动获取已通过真机验证的成功构建产物，完成 GPG 签名与 Sonatype Central Portal API 上传流转；
   - **零浪费上传配额**。

---

## 📦 如何在项目中引入

### 1. `gradle/libs.versions.toml`

```toml
[versions]
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

引入后即可彻底清空工程中 `app/libs/arm64-v8a/` 目录下全部存量 SO。
