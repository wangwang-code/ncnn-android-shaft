#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <net.h>
#include <mat.h>
#include <gpu.h>

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s -i input.png -o output.png -m model_dir -p profile [-g gpu_id]\n", prog);
    fprintf(stderr, "Profiles: u2netp, isnet-anime\n");
}

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    std::string model_dir;
    std::string profile = "u2netp";
    int gpu_id = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            profile = argv[++i];
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gpu_id = atoi(argv[++i]);
        }
    }

    if (input_path.empty() || output_path.empty() || model_dir.empty()) {
        print_usage(argv[0]);
        return -1;
    }

    // 1. 读取原图
    int width = 0, height = 0, channels = 0;
    unsigned char* img_data = stbi_load(input_path.c_str(), &width, &height, &channels, 3);
    if (!img_data) {
        fprintf(stderr, "failed to load %s\n", input_path.c_str());
        return -1;
    }

    // 2. 初始化 NCNN 与 GPU
    ncnn::create_gpu_instance();
    ncnn::Net net;
    net.opt.use_vulkan_compute = (gpu_id >= 0 && ncnn::get_gpu_count() > 0);
    net.opt.use_fp16_packed = true;
    net.opt.use_fp16_storage = true;
    net.opt.use_fp16_arithmetic = true;

    if (net.opt.use_vulkan_compute) {
        net.set_vulkan_device(gpu_id);
    }

    std::string param_path = model_dir + "/" + profile + ".param";
    std::string bin_path = model_dir + "/" + profile + ".bin";

    if (net.load_param(param_path.c_str()) != 0) {
        fprintf(stderr, "failed to load model from %s\n", param_path.c_str());
        stbi_image_free(img_data);
        ncnn::destroy_gpu_instance();
        return -1;
    }
    if (net.load_model(bin_path.c_str()) != 0) {
        fprintf(stderr, "failed to load model from %s\n", bin_path.c_str());
        stbi_image_free(img_data);
        ncnn::destroy_gpu_instance();
        return -1;
    }

    // 阶段进度汇报：模型加载完成
    fprintf(stderr, "33.00%% [0.0s / 0.0 ETA\n");
    fflush(stderr);

    // 3. 前处理
    int target_size = (profile == "isnet-anime") ? 1024 : 320;
    fprintf(stderr, "REMBG [%s] input %dx%d ch=%d target=%d\n", profile.c_str(), width, height, channels, target_size);

    ncnn::Mat in_mat;
    int pad_w = 0, pad_h = 0;
    int resize_w = target_size, resize_h = target_size;

    if (profile == "isnet-anime") {
        // letterbox 等比缩放加 padding
        float scale = std::min((float)target_size / width, (float)target_size / height);
        resize_w = (int)(width * scale);
        resize_h = (int)(height * scale);
        pad_w = target_size - resize_w;
        pad_h = target_size - resize_h;

        ncnn::Mat resized = ncnn::Mat::from_pixels_resize(img_data, ncnn::Mat::PIXEL_RGB, width, height, resize_w, resize_h);
        ncnn::copy_make_border(resized, in_mat, 0, pad_h, 0, pad_w, ncnn::BORDER_CONSTANT, 0.f);
    } else {
        // u2netp 直接拉伸至 320x320
        in_mat = ncnn::Mat::from_pixels_resize(img_data, ncnn::Mat::PIXEL_RGB, width, height, target_size, target_size);
    }

    // 归一化 (ImageNet standard)
    const float norm_vals[3] = { 1.0f / (0.229f * 255.f), 1.0f / (0.224f * 255.f), 1.0f / (0.225f * 255.f) };
    const float mean_vals[3] = { 0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f };
    in_mat.substract_mean_normalize(mean_vals, norm_vals);

    // 4. 推理
    ncnn::Extractor ex = net.create_extractor();
    ex.input("in0", in_mat);

    ncnn::Mat out_mat;
    if (ex.extract("out0", out_mat) != 0) {
        // 兜底尝试默认输出层
        if (ex.extract(net.output_names()[0], out_mat) != 0) {
            fprintf(stderr, "failed to extract output from model\n");
            stbi_image_free(img_data);
            ncnn::destroy_gpu_instance();
            return -1;
        }
    }

    // 阶段进度汇报：推理完成
    fprintf(stderr, "80.00%% [0.0s / 0.0 ETA\n");
    fflush(stderr);

    // 5. 后处理与生成 Alpha 通道
    // 如果是 letterbox，先裁剪有效区域
    ncnn::Mat valid_mask;
    if (profile == "isnet-anime" && (pad_w > 0 || pad_h > 0)) {
        ncnn::copy_cut_border(out_mat, valid_mask, 0, pad_h, 0, pad_w);
    } else {
        valid_mask = out_mat;
    }

    // 将单通道预测图双线性插值回原图尺寸
    ncnn::Mat mask_resized;
    ncnn::resize_bilinear(valid_mask, mask_resized, width, height);

    // Sigmoid / Clamp 处理得到 0~255 Alpha
    const float* mask_ptr = (const float*)mask_resized.data;
    std::vector<unsigned char> rgba(width * height * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            float val = mask_ptr[idx];
            // clamp 到 0~1
            if (val < 0.f) val = 0.f;
            if (val > 1.f) val = 1.f;
            unsigned char alpha = (unsigned char)(val * 255.f);

            rgba[idx * 4 + 0] = img_data[idx * 3 + 0];
            rgba[idx * 4 + 1] = img_data[idx * 3 + 1];
            rgba[idx * 4 + 2] = img_data[idx * 3 + 2];
            rgba[idx * 4 + 3] = alpha;
        }
    }

    // 6. 保存 RGBA PNG
    if (!stbi_write_png(output_path.c_str(), width, height, 4, rgba.data(), width * 4)) {
        fprintf(stderr, "failed to write png to %s\n", output_path.c_str());
        stbi_image_free(img_data);
        ncnn::destroy_gpu_instance();
        return -1;
    }

    // 释放资源
    stbi_image_free(img_data);
    net.clear();
    ncnn::destroy_gpu_instance();

    // 最终进度汇报
    fprintf(stderr, "100.00%% [0.0s / 0.0 ETA\n");
    fflush(stderr);

    return 0;
}
