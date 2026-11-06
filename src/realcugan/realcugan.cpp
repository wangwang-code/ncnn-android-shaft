
static int g_cugan_tile_done = 0;
// realcugan implemented with ncnn library

#include "realcugan.h"

#include <algorithm>
#include <vector>
#include <map>

// ncnn
#include "cpu.h"

#include "realcugan_preproc.comp.hex.h"
#include "realcugan_postproc.comp.hex.h"
#include "realcugan_4x_postproc.comp.hex.h"
#include "realcugan_preproc_tta.comp.hex.h"
#include "realcugan_postproc_tta.comp.hex.h"
#include "realcugan_4x_postproc_tta.comp.hex.h"

class FeatureCache
{
public:
    void clear()
    {
        gpu_cache.clear();
        cpu_cache.clear();
    }

    std::string make_key(int yi, int xi, int ti, const std::string& name) const
    {
        return std::to_string(yi) + "-" + std::to_string(xi) + "-" + std::to_string(ti) + "-" + name;
    }

    void load(int yi, int xi, int ti, const std::string& name, ncnn::VkMat& feat)
    {
        std::string key = make_key(yi, xi, ti, name);
        auto it = gpu_cache.find(key);
        if (it != gpu_cache.end() && !it->second.empty())
        {
            feat = it->second;
            return;
        }

        std::string suffix = "-" + std::to_string(ti) + "-" + name;
        for (const auto& kv : gpu_cache)
        {
            if (!kv.second.empty() && kv.first.length() >= suffix.length() &&
                kv.first.compare(kv.first.length() - suffix.length(), suffix.length(), suffix) == 0)
            {
                feat = kv.second;
                return;
            }
        }

        std::string name_suffix = "-" + name;
        for (const auto& kv : gpu_cache)
        {
            if (!kv.second.empty() && kv.first.length() >= name_suffix.length() &&
                kv.first.compare(kv.first.length() - name_suffix.length(), name_suffix.length(), name_suffix) == 0)
            {
                feat = kv.second;
                return;
            }
        }

        feat = gpu_cache[key];
    }

    void save(int yi, int xi, int ti, const std::string& name, ncnn::VkMat& feat)
    {
        gpu_cache[make_key(yi, xi, ti, name)] = feat;
    }

    void load(int yi, int xi, int ti, const std::string& name, ncnn::Mat& feat)
    {
        std::string key = make_key(yi, xi, ti, name);
        auto it = cpu_cache.find(key);
        if (it != cpu_cache.end() && !it->second.empty())
        {
            feat = it->second;
            return;
        }

        std::string suffix = "-" + std::to_string(ti) + "-" + name;
        for (const auto& kv : cpu_cache)
        {
            if (!kv.second.empty() && kv.first.length() >= suffix.length() &&
                kv.first.compare(kv.first.length() - suffix.length(), suffix.length(), suffix) == 0)
            {
                feat = kv.second;
                return;
            }
        }

        std::string name_suffix = "-" + name;
        for (const auto& kv : cpu_cache)
        {
            if (!kv.second.empty() && kv.first.length() >= name_suffix.length() &&
                kv.first.compare(kv.first.length() - name_suffix.length(), name_suffix.length(), name_suffix) == 0)
            {
                feat = kv.second;
                return;
            }
        }

        feat = cpu_cache[key];
    }

    void save(int yi, int xi, int ti, const std::string& name, ncnn::Mat& feat)
    {
        cpu_cache[make_key(yi, xi, ti, name)] = feat;
    }

public:
    std::map<std::string, ncnn::VkMat> gpu_cache;
    std::map<std::string, ncnn::Mat> cpu_cache;
};

RealCUGAN::RealCUGAN(int gpuid, bool _tta_mode, int num_threads)
{
    vkdev = gpuid == -1 ? 0 : ncnn::get_gpu_device(gpuid);

    net.opt.num_threads = num_threads;

    realcugan_preproc = 0;
    realcugan_postproc = 0;
    realcugan_4x_postproc = 0;
    bicubic_2x = 0;
    bicubic_3x = 0;
    bicubic_4x = 0;
    tta_mode = _tta_mode;
}

RealCUGAN::~RealCUGAN()
{
    // cleanup preprocess and postprocess pipeline
    {
        delete realcugan_preproc;
        delete realcugan_postproc;
    }

    bicubic_2x->destroy_pipeline(net.opt);
    delete bicubic_2x;

    bicubic_3x->destroy_pipeline(net.opt);
    delete bicubic_3x;

    bicubic_4x->destroy_pipeline(net.opt);
    delete bicubic_4x;
}

#if _WIN32
int RealCUGAN::load(const std::wstring& parampath, const std::wstring& modelpath)
#else
int RealCUGAN::load(const std::string& parampath, const std::string& modelpath)
#endif
{
    net.opt.use_vulkan_compute = vkdev ? true : false;
    net.opt.use_fp16_packed = true;
    net.opt.use_fp16_storage = vkdev ? true : false;
    net.opt.use_fp16_arithmetic = false;
    net.opt.use_int8_storage = false;

    net.set_vulkan_device(vkdev);

#if _WIN32
    {
        FILE* fp = _wfopen(parampath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", parampath.c_str());
        }

        net.load_param(fp);

        fclose(fp);
    }
    {
        FILE* fp = _wfopen(modelpath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", modelpath.c_str());
        }

        net.load_model(fp);

        fclose(fp);
    }
#else
    net.load_param(parampath.c_str());
    net.load_model(modelpath.c_str());
#endif

    // initialize preprocess and postprocess pipeline
    if (vkdev)
    {
        std::vector<ncnn::vk_specialization_type> specializations(1);
#if _WIN32
        specializations[0].i = 1;
#else
        specializations[0].i = 0;
#endif

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_preproc_tta_comp_data, sizeof(realcugan_preproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_preproc_comp_data, sizeof(realcugan_preproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_preproc = new ncnn::Pipeline(vkdev);
            realcugan_preproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_preproc->create(spirv.data(), spirv.size() * 4, specializations);
        }

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_postproc_tta_comp_data, sizeof(realcugan_postproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_postproc_comp_data, sizeof(realcugan_postproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_postproc = new ncnn::Pipeline(vkdev);
            realcugan_postproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_postproc->create(spirv.data(), spirv.size() * 4, specializations);
        }

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_4x_postproc_tta_comp_data, sizeof(realcugan_4x_postproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_4x_postproc_comp_data, sizeof(realcugan_4x_postproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_4x_postproc = new ncnn::Pipeline(vkdev);
            realcugan_4x_postproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_4x_postproc->create(spirv.data(), spirv.size() * 4, specializations);
        }
    }

    // bicubic 2x/3x/4x for alpha channel
    {
        bicubic_2x = ncnn::create_layer("Interp");
        bicubic_2x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 2.f);
        pd.set(2, 2.f);
        bicubic_2x->load_param(pd);

        bicubic_2x->create_pipeline(net.opt);
    }
    {
        bicubic_3x = ncnn::create_layer("Interp");
        bicubic_3x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 3.f);
        pd.set(2, 3.f);
        bicubic_3x->load_param(pd);

        bicubic_3x->create_pipeline(net.opt);
    }
    {
        bicubic_4x = ncnn::create_layer("Interp");
        bicubic_4x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 4.f);
        pd.set(2, 4.f);
        bicubic_4x->load_param(pd);

        bicubic_4x->create_pipeline(net.opt);
    }

    return 0;
}

int RealCUGAN::process(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    g_cugan_tile_done = 0;
    bool syncgap_needed = tilesize < std::max(inimage.w, inimage.h);

    if (!vkdev)
    {
        // cpu only (disabled on mobile)
        return -1;
    }

    if (noise == -1 && scale == 1)
    {
        outimage = inimage;
        return 0;
    }

    if (syncgap_needed && syncgap)
    {
        if (syncgap == 1)
            return process_se(inimage, outimage);
        if (syncgap == 2)
            return process_se_rough(inimage, outimage);
        if (syncgap == 3)
            return process_se_very_rough(inimage, outimage);
    }

    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    ncnn::VkAllocator* blob_vkallocator = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_vkallocator = vkdev->acquire_staging_allocator();

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3)
        {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4)
        {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute cmd(vkdev);

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, blob_vkallocator);
        }

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3)
            {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4)
            {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("in0", in_tile_gpu[ti]);

                    ex.extract("out0", out_tile_gpu[ti], cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(11);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu[0];
                    bindings[2] = out_tile_gpu[1];
                    bindings[3] = out_tile_gpu[2];
                    bindings[4] = out_tile_gpu[3];
                    bindings[5] = out_tile_gpu[4];
                    bindings[6] = out_tile_gpu[5];
                    bindings[7] = out_tile_gpu[6];
                    bindings[8] = out_tile_gpu[7];
                    bindings[9] = out_alpha_tile_gpu;
                    bindings[10] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu[0].w;
                    constants[4].i = out_tile_gpu[0].h;
                    constants[5].i = out_tile_gpu[0].cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = out_tile_gpu[0];
                    bindings[1] = out_tile_gpu[1];
                    bindings[2] = out_tile_gpu[2];
                    bindings[3] = out_tile_gpu[3];
                    bindings[4] = out_tile_gpu[4];
                    bindings[5] = out_tile_gpu[5];
                    bindings[6] = out_tile_gpu[6];
                    bindings[7] = out_tile_gpu[7];
                    bindings[8] = out_alpha_tile_gpu;
                    bindings[9] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu[0].w;
                    constants[1].i = out_tile_gpu[0].h;
                    constants[2].i = out_tile_gpu[0].cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu;
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("in0", in_tile_gpu);

                    ex.extract("out0", out_tile_gpu, cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(4);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu;
                    bindings[2] = out_alpha_tile_gpu;
                    bindings[3] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu.w;
                    constants[4].i = out_tile_gpu.h;
                    constants[5].i = out_tile_gpu.cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = out_tile_gpu;
                    bindings[1] = out_alpha_tile_gpu;
                    bindings[2] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu.w;
                    constants[1].i = out_tile_gpu.h;
                    constants[2].i = out_tile_gpu.cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        // download
        {
            ncnn::Mat out;

            if (opt.use_fp16_storage && opt.use_int8_storage)
            {
                out = ncnn::Mat(out_gpu.w, out_gpu.h, (unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, (size_t)channels, 1);
            }

            cmd.record_clone(out_gpu, out, opt);

            cmd.submit_and_wait();

            if (!(opt.use_fp16_storage && opt.use_int8_storage))
            {
                if (channels == 3)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB2BGR);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB);
#endif
                }
                if (channels == 4)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA2BGRA);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA);
#endif
                }
            }
        }

        fprintf(stderr, "TILE %d\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    vkdev->reclaim_blob_allocator(blob_vkallocator);
    vkdev->reclaim_staging_allocator(staging_vkallocator);

    return 0;
}

int RealCUGAN::process_cpu(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    (void)inimage;
    (void)outimage;
    return -1;
}

int RealCUGAN::process_se(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    ncnn::VkAllocator* blob_vkallocator = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_vkallocator = vkdev->acquire_staging_allocator();

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    FeatureCache cache;

    std::vector<std::string> in0 = {};
    std::vector<std::string> out0 = {"gap0"};
    process_se_stage0(inimage, in0, out0, opt, cache);

    std::vector<std::string> gap0 = {"gap0"};
    process_se_sync_gap(inimage, gap0, opt, cache);

    std::vector<std::string> in1 = {"gap0"};
    std::vector<std::string> out1 = {"gap1"};
    process_se_stage0(inimage, in1, out1, opt, cache);

    std::vector<std::string> gap1 = {"gap1"};
    process_se_sync_gap(inimage, gap1, opt, cache);

    std::vector<std::string> in2 = {"gap0", "gap1"};
    std::vector<std::string> out2 = {"gap2"};
    process_se_stage0(inimage, in2, out2, opt, cache);

    std::vector<std::string> gap2 = {"gap2"};
    process_se_sync_gap(inimage, gap2, opt, cache);

    std::vector<std::string> in3 = {"gap0", "gap1", "gap2"};
    std::vector<std::string> out3 = {"gap3"};
    process_se_stage0(inimage, in3, out3, opt, cache);

    std::vector<std::string> gap3 = {"gap3"};
    process_se_sync_gap(inimage, gap3, opt, cache);

    std::vector<std::string> in4 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_stage2(inimage, in4, outimage, opt, cache);

    cache.clear();

    vkdev->reclaim_blob_allocator(blob_vkallocator);
    vkdev->reclaim_staging_allocator(staging_vkallocator);

    return 0;
}

int RealCUGAN::process_se_rough(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    ncnn::VkAllocator* blob_vkallocator = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_vkallocator = vkdev->acquire_staging_allocator();

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    FeatureCache cache;

    std::vector<std::string> in0 = {};
    std::vector<std::string> out0 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_stage0(inimage, in0, out0, opt, cache);

    std::vector<std::string> gap0 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_sync_gap(inimage, gap0, opt, cache);

    std::vector<std::string> in4 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_stage2(inimage, in4, outimage, opt, cache);

    cache.clear();

    vkdev->reclaim_blob_allocator(blob_vkallocator);
    vkdev->reclaim_staging_allocator(staging_vkallocator);

    return 0;
}

int RealCUGAN::process_se_very_rough(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    ncnn::VkAllocator* blob_vkallocator = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_vkallocator = vkdev->acquire_staging_allocator();

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    FeatureCache cache;

    std::vector<std::string> in0 = {};
    std::vector<std::string> out0 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_very_rough_stage0(inimage, in0, out0, opt, cache);

    std::vector<std::string> gap0 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_very_rough_sync_gap(inimage, gap0, opt, cache);

    std::vector<std::string> in4 = {"gap0", "gap1", "gap2", "gap3"};
    process_se_stage2(inimage, in4, outimage, opt, cache);

    cache.clear();

    vkdev->reclaim_blob_allocator(blob_vkallocator);
    vkdev->reclaim_staging_allocator(staging_vkallocator);

    return 0;
}

int RealCUGAN::process_cpu_se(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    (void)inimage;
    (void)outimage;
    return -1;
}

int RealCUGAN::process_cpu_se_rough(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    (void)inimage;
    (void)outimage;
    return -1;
}

int RealCUGAN::process_cpu_se_very_rough(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    (void)inimage;
    (void)outimage;
    return -1;
}

int RealCUGAN::process_se_stage0(const ncnn::Mat& inimage, const std::vector<std::string>& names, const std::vector<std::string>& outnames, const ncnn::Option& opt, FeatureCache& cache) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3)
        {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4)
        {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute cmd(vkdev);

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, opt.blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, opt.blob_vkallocator);
        }

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3)
            {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4)
            {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu[ti]);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, ti, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    for (size_t i = 0; i < outnames.size(); i++)
                    {
                        ncnn::VkMat feat;
                        ex.extract(outnames[i].c_str(), feat, cmd);

                        cache.save(yi, xi, ti, outnames[i], feat);
                    }
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, 0, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    for (size_t i = 0; i < outnames.size(); i++)
                    {
                        ncnn::VkMat feat;
                        ex.extract(outnames[i].c_str(), feat, cmd);

                        cache.save(yi, xi, 0, outnames[i], feat);
                    }
                }
            }

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        cmd.submit_and_wait();
        cmd.reset();

        fprintf(stderr, "TILE %d\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_stage2(const ncnn::Mat& inimage, const std::vector<std::string>& names, ncnn::Mat& outimage, const ncnn::Option& opt, FeatureCache& cache) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3)
        {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4)
        {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute cmd(vkdev);

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, opt.blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, opt.blob_vkallocator);
        }

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3)
            {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4)
            {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu[ti]);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, ti, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    ex.extract("out0", out_tile_gpu[ti], cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(11);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu[0];
                    bindings[2] = out_tile_gpu[1];
                    bindings[3] = out_tile_gpu[2];
                    bindings[4] = out_tile_gpu[3];
                    bindings[5] = out_tile_gpu[4];
                    bindings[6] = out_tile_gpu[5];
                    bindings[7] = out_tile_gpu[6];
                    bindings[8] = out_tile_gpu[7];
                    bindings[9] = out_alpha_tile_gpu;
                    bindings[10] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu[0].w;
                    constants[4].i = out_tile_gpu[0].h;
                    constants[5].i = out_tile_gpu[0].cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = out_tile_gpu[0];
                    bindings[1] = out_tile_gpu[1];
                    bindings[2] = out_tile_gpu[2];
                    bindings[3] = out_tile_gpu[3];
                    bindings[4] = out_tile_gpu[4];
                    bindings[5] = out_tile_gpu[5];
                    bindings[6] = out_tile_gpu[6];
                    bindings[7] = out_tile_gpu[7];
                    bindings[8] = out_alpha_tile_gpu;
                    bindings[9] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu[0].w;
                    constants[1].i = out_tile_gpu[0].h;
                    constants[2].i = out_tile_gpu[0].cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu;
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, 0, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    ex.extract("out0", out_tile_gpu, cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(4);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu;
                    bindings[2] = out_alpha_tile_gpu;
                    bindings[3] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu.w;
                    constants[4].i = out_tile_gpu.h;
                    constants[5].i = out_tile_gpu.cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = out_tile_gpu;
                    bindings[1] = out_alpha_tile_gpu;
                    bindings[2] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu.w;
                    constants[1].i = out_tile_gpu.h;
                    constants[2].i = out_tile_gpu.cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        // download
        {
            ncnn::Mat out;

            if (opt.use_fp16_storage && opt.use_int8_storage)
            {
                out = ncnn::Mat(out_gpu.w, out_gpu.h, (unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, (size_t)channels, 1);
            }

            cmd.record_clone(out_gpu, out, opt);

            cmd.submit_and_wait();

            if (!(opt.use_fp16_storage && opt.use_int8_storage))
            {
                if (channels == 3)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB2BGR);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB);
#endif
                }
                if (channels == 4)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA2BGRA);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA);
#endif
                }
            }
        }

        fprintf(stderr, "TILE %d\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_sync_gap(const ncnn::Mat& inimage, const std::vector<std::string>& names, const ncnn::Option& opt, FeatureCache& cache) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    std::vector< std::vector<ncnn::VkMat> > feats(names.size());
    for (int yi = 0; yi < ytiles; yi++)
    {
        for (int xi = 0; xi < xtiles; xi++)
        {
            {
                for (size_t i = 0; i < names.size(); i++)
                {
                    if (tta_mode)
                    {
                        for (int ti = 0; ti < 8; ti++)
                        {
                            ncnn::VkMat feat;
                            cache.load(yi, xi, ti, names[i], feat);

                            feats[i].push_back(feat);
                        }
                    }
                    else
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, 0, names[i], feat);

                        feats[i].push_back(feat);
                    }
                }
            }
        }
    }

    const int tiles = ytiles * xtiles * (tta_mode ? 8 : 1);

    ncnn::VkCompute cmd(vkdev);

    // download
    std::vector< std::vector<ncnn::Mat> > feats_cpu(names.size());
    for (size_t i = 0; i < names.size(); i++)
    {
        feats_cpu[i].resize(tiles);

        for (int j = 0; j < tiles; j++)
        {
            cmd.record_download(feats[i][j], feats_cpu[i][j], opt);
        }
    }

    cmd.submit_and_wait();
    cmd.reset();

    // global average
    // upload
    std::vector<ncnn::VkMat> avgfeats(names.size());
    for (size_t i = 0; i < names.size(); i++)
    {
        for (int j = 0; j < tiles; j++)
        {
            if (opt.use_fp16_storage && ncnn::cpu_support_arm_asimdhp() && feats_cpu[i][j].elembits() == 16)
            {
                ncnn::Mat feat_fp32;
                ncnn::cast_float16_to_float32(feats_cpu[i][j], feat_fp32, opt);
                feats_cpu[i][j] = feat_fp32;
            }

            if (opt.use_packing_layout && feats_cpu[i][j].elempack != 1)
            {
                ncnn::Mat feat_cpu_unpacked;
                ncnn::convert_packing(feats_cpu[i][j], feat_cpu_unpacked, 1, opt);
                feats_cpu[i][j] = feat_cpu_unpacked;
            }
        }

        // handle feats_cpu[i] vector
        {
            ncnn::Mat avgfeat;
            avgfeat.create_like(feats_cpu[i][0]);
            avgfeat.fill(0.f);

            int len = avgfeat.total();

            for (int j = 0; j < tiles; j++)
            {
                const ncnn::Mat f = feats_cpu[i][j];

                for (int k = 0; k < len; k++)
                {
                    avgfeat[k] += f[k];
                }
            }

            for (int k = 0; k < len; k++)
            {
                avgfeat[k] /= tiles;
            }

            cmd.record_upload(avgfeat, avgfeats[i], opt);
        }
    }

    cmd.submit_and_wait();
    cmd.reset();


    for (int yi = 0; yi < ytiles; yi++)
    {
        for (int xi = 0; xi < xtiles; xi++)
        {
            {
                for (size_t i = 0; i < names.size(); i++)
                {
                    if (tta_mode)
                    {
                        for (int ti = 0; ti < 8; ti++)
                        {
                            cache.save(yi, xi, ti, names[i], avgfeats[i]);
                        }
                    }
                    else
                    {
                        cache.save(yi, xi, 0, names[i], avgfeats[i]);
                    }
                }
            }
        }
    }

    return 0;
}

int RealCUGAN::process_se_very_rough_stage0(const ncnn::Mat& inimage, const std::vector<std::string>& names, const std::vector<std::string>& outnames, const ncnn::Option& opt, FeatureCache& cache) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = 32;
    const int TILE_SIZE_Y = 32;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi + 2 < ytiles; yi += 3)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3)
        {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4)
        {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute cmd(vkdev);

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, opt.blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, opt.blob_vkallocator);
        }

        for (int xi = 0; xi + 2 < xtiles; xi += 3)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3)
            {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4)
            {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu[ti]);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, ti, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    for (size_t i = 0; i < outnames.size(); i++)
                    {
                        ncnn::VkMat feat;
                        ex.extract(outnames[i].c_str(), feat, cmd);

                        cache.save(yi, xi, ti, outnames[i], feat);
                    }
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, opt.blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, opt.blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(opt.blob_vkallocator);
                    ex.set_workspace_vkallocator(opt.blob_vkallocator);
                    ex.set_staging_vkallocator(opt.staging_vkallocator);

                    ex.input("in0", in_tile_gpu);

                    for (size_t i = 0; i < names.size(); i++)
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, 0, names[i], feat);

                        ex.input(names[i].c_str(), feat);
                    }

                    for (size_t i = 0; i < outnames.size(); i++)
                    {
                        ncnn::VkMat feat;
                        ex.extract(outnames[i].c_str(), feat, cmd);

                        cache.save(yi, xi, 0, outnames[i], feat);
                    }
                }
            }

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        cmd.submit_and_wait();
        cmd.reset();

        fprintf(stderr, "TILE %d\n", ++g_cugan_tile_done);
        fflush(stderr);
    }

    return 0;
}

int RealCUGAN::process_se_very_rough_sync_gap(const ncnn::Mat& inimage, const std::vector<std::string>& names, const ncnn::Option& opt, FeatureCache& cache) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = 32;
    const int TILE_SIZE_Y = 32;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    std::vector< std::vector<ncnn::VkMat> > feats(names.size());
    for (int yi = 0; yi + 2 < ytiles; yi += 3)
    {
        for (int xi = 0; xi + 2 < xtiles; xi += 3)
        {
            {
                for (size_t i = 0; i < names.size(); i++)
                {
                    if (tta_mode)
                    {
                        for (int ti = 0; ti < 8; ti++)
                        {
                            ncnn::VkMat feat;
                            cache.load(yi, xi, ti, names[i], feat);

                            feats[i].push_back(feat);
                        }
                    }
                    else
                    {
                        ncnn::VkMat feat;
                        cache.load(yi, xi, 0, names[i], feat);

                        feats[i].push_back(feat);
                    }
                }
            }
        }
    }

    const int tiles = (ytiles / 3) * (xtiles / 3) * (tta_mode ? 8 : 1);

    ncnn::VkCompute cmd(vkdev);

    // download
    std::vector< std::vector<ncnn::Mat> > feats_cpu(names.size());
    for (size_t i = 0; i < names.size(); i++)
    {
        feats_cpu[i].resize(tiles);

        for (int j = 0; j < tiles; j++)
        {
            cmd.record_download(feats[i][j], feats_cpu[i][j], opt);
        }
    }

    cmd.submit_and_wait();
    cmd.reset();

    // global average
    // upload
    std::vector<ncnn::VkMat> avgfeats(names.size());
    for (size_t i = 0; i < names.size(); i++)
    {
        for (int j = 0; j < tiles; j++)
        {
            if (opt.use_fp16_storage && ncnn::cpu_support_arm_asimdhp() && feats_cpu[i][j].elembits() == 16)
            {
                ncnn::Mat feat_fp32;
                ncnn::cast_float16_to_float32(feats_cpu[i][j], feat_fp32, opt);
                feats_cpu[i][j] = feat_fp32;
            }

            if (opt.use_packing_layout && feats_cpu[i][j].elempack != 1)
            {
                ncnn::Mat feat_cpu_unpacked;
                ncnn::convert_packing(feats_cpu[i][j], feat_cpu_unpacked, 1, opt);
                feats_cpu[i][j] = feat_cpu_unpacked;
            }
        }

        // handle feats_cpu[i] vector
        {
            ncnn::Mat avgfeat;
            avgfeat.create_like(feats_cpu[i][0]);
            avgfeat.fill(0.f);

            int len = avgfeat.total();

            for (int j = 0; j < tiles; j++)
            {
                const ncnn::Mat f = feats_cpu[i][j];

                for (int k = 0; k < len; k++)
                {
                    avgfeat[k] += f[k];
                }
            }

            for (int k = 0; k < len; k++)
            {
                avgfeat[k] /= tiles;
            }

            cmd.record_upload(avgfeat, avgfeats[i], opt);
        }
    }

    cmd.submit_and_wait();
    cmd.reset();


    const int stage2_xtiles = (w + tilesize - 1) / tilesize;
    const int stage2_ytiles = (h + tilesize - 1) / tilesize;

    for (int yi = 0; yi < stage2_ytiles; yi++)
    {
        for (int xi = 0; xi < stage2_xtiles; xi++)
        {
            for (size_t i = 0; i < names.size(); i++)
            {
                if (tta_mode)
                {
                    for (int ti = 0; ti < 8; ti++)
                    {
                        cache.save(yi, xi, ti, names[i], avgfeats[i]);
                    }
                }
                else
                {
                    cache.save(yi, xi, 0, names[i], avgfeats[i]);
                }
            }
        }
    }

    return 0;
}

int RealCUGAN::process_cpu_se_stage0(const ncnn::Mat& inimage, const std::vector<std::string>& names, const std::vector<std::string>& outnames, FeatureCache& cache) const
{
    (void)inimage;
    (void)names;
    (void)outnames;
    (void)cache;
    return -1;
}

int RealCUGAN::process_cpu_se_stage2(const ncnn::Mat& inimage, const std::vector<std::string>& names, ncnn::Mat& outimage, FeatureCache& cache) const
{
    (void)inimage;
    (void)names;
    (void)outimage;
    (void)cache;
    return -1;
}

int RealCUGAN::process_cpu_se_sync_gap(const ncnn::Mat& inimage, const std::vector<std::string>& names, FeatureCache& cache) const
{
    (void)inimage;
    (void)names;
    (void)cache;
    return -1;
}

int RealCUGAN::process_cpu_se_very_rough_stage0(const ncnn::Mat& inimage, const std::vector<std::string>& names, const std::vector<std::string>& outnames, FeatureCache& cache) const
{
    (void)inimage;
    (void)names;
    (void)outnames;
    (void)cache;
    return -1;
}

int RealCUGAN::process_cpu_se_very_rough_sync_gap(const ncnn::Mat& inimage, const std::vector<std::string>& names, FeatureCache& cache) const
{
    (void)inimage;
    (void)names;
    (void)cache;
    return -1;
}
