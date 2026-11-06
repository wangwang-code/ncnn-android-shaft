import os
import sys
import argparse
import subprocess
import tempfile
import zlib

CORE_LAYERS = [
    'batchnorm', 'binaryop', 'concat', 'convolution', 'crop', 'deconvolution',
    'eltwise', 'flatten', 'innerproduct', 'interp', 'packing', 'padding',
    'pixelshuffle', 'pooling', 'prelu', 'relu', 'reshape', 'scale', 'sigmoid', 'split'
]

EXCLUDED = ['1d', '3d', 'depthwise', 'cm', 'adaptive', 'direct', 'col2im', 'sum8']

def is_whitelisted(shader_name):
    low = shader_name.lower().replace('.comp', '')
    match = any(low == c or low.startswith(c + '_') or low.startswith(c + '1') or low.startswith(c + '3') for c in CORE_LAYERS)
    if not match:
        return False
    if any(e in low for e in EXCLUDED):
        return False
    return True

ANIME_EXTS = '''
#extension GL_EXT_shader_16bit_storage: require
struct sfpvec8 { f16vec4 abcd; f16vec4 efgh; };
'''

ANIME_PREAMBLE = ANIME_EXTS + '''
#define sfp float16_t
#define sfpvec2 f16vec2
#define sfpvec4 f16vec4
#define afp float
#define afpvec2 vec2
#define afpvec4 vec4
#define afpvec8 mat2x4
#define afpmat4 mat4
#define lfp float
#define lfpvec4 uvec2
#define sfp2lfp(v) float(v)
#define sfp2lfpvec4(v) uvec2(packHalf2x16(vec4(v).rg),packHalf2x16(vec4(v).ba))
#define lfp2afp(v) v
#define lfp2afpvec4(v) vec4(unpackHalf2x16(v.x),unpackHalf2x16(v.y))
#define buffer_ld1(buf,i) float(buf[i])
#define buffer_st1(buf,i,v) {buf[i]=float16_t(v);}
#define buffer_cp1(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_cp1to4(buf,i,sbuf,si4) {buf[i].r=sbuf[si4.r];buf[i].g=sbuf[si4.g];buf[i].b=sbuf[si4.b];buf[i].a=sbuf[si4.a];}
#define buffer_cp1to8(buf,i,sbuf,si4,sii4) {buf[i].abcd.r=sbuf[si4.r];buf[i].abcd.g=sbuf[si4.g];buf[i].abcd.b=sbuf[si4.b];buf[i].abcd.a=sbuf[si4.a];buf[i].efgh.r=sbuf[sii4.r];buf[i].efgh.g=sbuf[sii4.g];buf[i].efgh.b=sbuf[sii4.b];buf[i].efgh.a=sbuf[sii4.a];}
#define buffer_ld2(buf,i) vec2(buf[i])
#define buffer_st2(buf,i,v) {buf[i]=f16vec2(v);}
#define buffer_cp2(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_ld4(buf,i) vec4(buf[i])
#define buffer_st4(buf,i,v) {buf[i]=f16vec4(v);}
#define buffer_cp4(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_cp4to1(buf,i4,sbuf,si) {buf[i4.r]=sbuf[si].r;buf[i4.g]=sbuf[si].g;buf[i4.b]=sbuf[si].b;buf[i4.a]=sbuf[si].a;}
#define buffer_cp4to8(buf,i,sbuf,si2) {buf[i].abcd=sbuf[si2.r];buf[i].efgh=sbuf[si2.g];}
#define buffer_ld8(buf,i) mat2x4(vec4(buf[i].abcd),vec4(buf[i].efgh))
#define buffer_st8(buf,i,v) {buf[i].abcd=f16vec4(v[0]);buf[i].efgh=f16vec4(v[1]);}
#define buffer_cp8(buf,i,sbuf,si) {buf[i].abcd=sbuf[si].abcd;buf[i].efgh=sbuf[si].efgh;}
#define buffer_cp8to1(buf,i4,ii4,sbuf,si) {buf[i4.r]=sbuf[si].abcd.r;buf[i4.g]=sbuf[si].abcd.g;buf[i4.b]=sbuf[si].abcd.b;buf[i4.a]=sbuf[si].abcd.a; buf[ii4.r]=sbuf[si].efgh.r;buf[ii4.g]=sbuf[si].efgh.g;buf[ii4.b]=sbuf[si].efgh.b;buf[ii4.a]=sbuf[si].efgh.a;}
#define buffer_cp8to4(buf,i2,sbuf,si) {buf[i2.r]=sbuf[si].abcd;buf[i2.g]=sbuf[si].efgh;}
#define psc(x) (x==0?p.x:x)
#define NCNN_fp16_storage 1
'''

GENERAL_EXTS = '''
#extension GL_EXT_shader_16bit_storage: require
struct sfpvec8 { f16vec4 abcd; f16vec4 efgh; };
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
'''

GENERAL_PREAMBLE = GENERAL_EXTS + '''
#define sfp float16_t
#define sfpvec2 f16vec2
#define sfpvec4 f16vec4
#define sfpvec8 f16mat2x4
#define sfpmat4 f16mat4
#define afp float16_t
#define afpvec2 f16vec2
#define afpvec4 f16vec4
#define afpvec8 f16mat2x4
#define afpmat4 f16mat4
#define lfp float16_t
#define lfpvec4 f16vec4
#define sfp2lfp(v) v
#define sfp2lfpvec4(v) v
#define lfp2afp(v) v
#define lfp2afpvec4(v) v
#define buffer_ld1(buf,i) buf[i]
#define buffer_st1(buf,i,v) {buf[i]=v;}
#define buffer_cp1(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_cp1to4(buf,i,sbuf,si4) {buf[i]=f16vec4(sbuf[si4.r],sbuf[si4.g],sbuf[si4.b],sbuf[si4.a]);}
#define buffer_cp1to8(buf,i,sbuf,si4,sii4) {buf[i]=f16mat2x4(sbuf[si4.r],sbuf[si4.g],sbuf[si4.b],sbuf[si4.a],sbuf[sii4.r],sbuf[sii4.g],sbuf[sii4.b],sbuf[sii4.a]);}
#define buffer_ld2(buf,i) buf[i]
#define buffer_st2(buf,i,v) {buf[i]=v;}
#define buffer_cp2(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_ld4(buf,i) buf[i]
#define buffer_st4(buf,i,v) {buf[i]=v;}
#define buffer_cp4(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_cp4to1(buf,i4,sbuf,si) {buf[i4.r]=sbuf[si].r;buf[i4.g]=sbuf[si].g;buf[i4.b]=sbuf[si].b;buf[i4.a]=sbuf[si].a;}
#define buffer_cp4to8(buf,i,sbuf,si2) {buf[i]=f16mat2x4(sbuf[si2.r],sbuf[si2.g]);}
#define buffer_ld8(buf,i) buf[i]
#define buffer_st8(buf,i,v) {buf[i]=v;}
#define buffer_cp8(buf,i,sbuf,si) {buf[i]=sbuf[si];}
#define buffer_cp8to1(buf,i4,ii4,sbuf,si) {f16mat2x4 _v=sbuf[si]; buf[i4.r]=_v[0].r;buf[i4.g]=_v[0].g;buf[i4.b]=_v[0].b;buf[i4.a]=_v[0].a; buf[ii4.r]=_v[1].r;buf[ii4.g]=_v[1].g;buf[ii4.b]=_v[1].b;buf[ii4.a]=_v[1].a;}
#define buffer_cp8to4(buf,i2,sbuf,si) {f16mat2x4 _v=sbuf[si]; buf[i2.r]=_v[0];buf[i2.g]=_v[1];}
#define sfp2afpmat4(v) v
#define afp2sfpmat4(v) v
#define psc(x) (x==0?p.x:x)
#define NCNN_fp16_storage 1
#define NCNN_fp16_arithmetic 1
'''

def find_glslc(ndk_dir):
    candidates = [
        os.path.join(ndk_dir, 'shader-tools', 'linux-x86_64', 'glslc'),
        os.path.join(ndk_dir, 'shader-tools', 'windows-x86_64', 'glslc.exe'),
        os.path.join(ndk_dir, 'shader-tools', 'darwin-x86_64', 'glslc')
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return 'glslc'

def compile_single_shader(glslc, shader_path, include_dir, preamble):
    with open(shader_path, 'r', encoding='utf-8') as f:
        content = f.read()
    v_idx = content.find('#version 450')
    if v_idx == -1:
        return None
    new_content = content[:v_idx+12] + '\n' + preamble + '\n' + content[v_idx+12:]
    with tempfile.NamedTemporaryFile(suffix='.comp', mode='w', delete=False) as tmp:
        tmp.write(new_content)
        tmp_name = tmp.name
    spv_name = tmp_name + '.spv'
    cmd = [glslc, f'-I{include_dir}', '--target-env=vulkan1.1', '-fshader-stage=compute', '-Os', tmp_name, '-o', spv_name]
    res = subprocess.run(cmd, capture_output=True)
    spv_bytes = None
    if res.returncode == 0 and os.path.exists(spv_name):
        with open(spv_name, 'rb') as spv_f:
            spv_bytes = spv_f.read()
    try:
        os.remove(tmp_name)
    except: pass
    try:
        if os.path.exists(spv_name): os.remove(spv_name)
    except: pass
    return spv_bytes

def process_ncnn_shaders(target_type, repo_root, ndk_dir, output_dir):
    ncnn_dir = os.path.join(repo_root, 'external', f'ncnn-{target_type}')
    shader_dir = os.path.join(ncnn_dir, 'src', 'layer', 'vulkan', 'shader')
    glslc = find_glslc(ndk_dir)
    preamble = ANIME_PREAMBLE if target_type == 'anime' else GENERAL_PREAMBLE

    cmake_path = os.path.join(ncnn_dir, 'src', 'CMakeLists.txt')
    layer_names = []
    with open(cmake_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('ncnn_add_layer('):
                arg = line.split('(')[1].split(')')[0].split()[0]
                layer_names.append(arg.lower())

    ordered_shaders = []
    for l in layer_names:
        sh1 = f'{l}.comp'
        if os.path.exists(os.path.join(shader_dir, sh1)):
            ordered_shaders.append(sh1)
        sub_files = [f for f in sorted(os.listdir(shader_dir)) if f.startswith(f'{l}_') and f.endswith('.comp')]
        ordered_shaders.extend(sub_files)

    if os.path.exists(os.path.join(ncnn_dir, 'src', 'convert_ycbcr.comp')):
        ordered_shaders.append('convert_ycbcr.comp')
    if os.path.exists(os.path.join(shader_dir, 'vulkan_activation.comp')):
        ordered_shaders.append('vulkan_activation.comp')

    print(f'[{target_type}] Discovered {len(ordered_shaders)} shaders in exact registry order.')

    raw_blob = bytearray()
    registry_entries = []

    compiled_count = 0
    skipped_count = 0

    for idx, s_name in enumerate(ordered_shaders):
        s_path = os.path.join(shader_dir, s_name)
        if not os.path.exists(s_path):
            s_path = os.path.join(ncnn_dir, 'src', s_name)

        if is_whitelisted(s_name) and os.path.exists(s_path):
            spv = compile_single_shader(glslc, s_path, shader_dir, preamble)
            if spv and len(spv) > 0:
                offset = len(raw_blob)
                size = len(spv)
                raw_blob.extend(spv)
                registry_entries.append((offset, size))
                compiled_count += 1
            else:
                registry_entries.append((0, 0))
                skipped_count += 1
        else:
            registry_entries.append((0, 0))
            skipped_count += 1

    compressed_blob = zlib.compress(raw_blob, level=9)
    print(f'[{target_type}] Compiled {compiled_count} shaders, skipped {skipped_count}.')
    print(f'[{target_type}] Raw SPV size: {len(raw_blob)/1024:.1f} KB, Compressed size: {len(compressed_blob)/1024:.1f} KB')

    os.makedirs(output_dir, exist_ok=True)
    
    # 1. layer_shader_spv_blob.h
    blob_header = os.path.join(output_dir, 'layer_shader_spv_blob.h')
    with open(blob_header, 'w', encoding='utf-8') as f:
        f.write('// Auto-generated Zero-Glslang SPV Compressed Blob\n')
        f.write('#ifndef LAYER_SHADER_SPV_BLOB_H\n#define LAYER_SHADER_SPV_BLOB_H\n\n')
        f.write(f'static const uint32_t ncnn_spv_uncompressed_size = {len(raw_blob)};\n\n')
        f.write(f'static const uint8_t ncnn_spv_compressed_blob[] = {{\n')
        hex_items = [f'0x{b:02x}' for b in compressed_blob]
        for i in range(0, len(hex_items), 16):
            f.write('    ' + ', '.join(hex_items[i:i+16]) + ',\n')
        f.write('};\n\n')
        f.write('#endif // LAYER_SHADER_SPV_BLOB_H\n')

    # 2. layer_shader_registry_zero_glslang.h
    reg_header = os.path.join(output_dir, 'layer_shader_registry_zero_glslang.h')
    with open(reg_header, 'w', encoding='utf-8') as f:
        for offset, size in registry_entries:
            f.write(f'{{{offset}, {size}}},\n')

    print(f'[{target_type}] Generated {blob_header} and {reg_header}')

def process_filter_shaders(repo_root, ndk_dir):
    glslc = find_glslc(ndk_dir)
    dirs = [
        os.path.join(repo_root, 'src', 'realcugan'),
        os.path.join(repo_root, 'src', 'realsr'),
        os.path.join(repo_root, 'src', 'rife')
    ]
    for d in dirs:
        for f in os.listdir(d):
            if f.endswith('.comp'):
                p = os.path.join(d, f)
                spv = compile_single_shader(glslc, p, d, ANIME_PREAMBLE)
                if spv:
                    base = f.replace('.comp', '')
                    out_h = os.path.join(d, f'{base}.comp.hex.h')
                    with open(out_h, 'w', encoding='utf-8') as out_f:
                        out_f.write(f'// Auto-generated native SPIR-V header for {f}\n')
                        out_f.write(f'static const char {base}_comp_data[] = {{\n')
                        hex_items = [f'0x{b:02x}' for b in spv]
                        for i in range(0, len(hex_items), 16):
                            out_f.write('    ' + ', '.join(hex_items[i:i+16]) + ',\n')
                        out_f.write('};\n')
                    print(f'[filter] Generated {os.path.basename(out_h)} ({len(spv)} bytes SPIR-V)')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', choices=['anime', 'general', 'filters', 'all'], required=True)
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK', ''))
    parser.add_argument('--repo', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
    parser.add_argument('--outdir', default='')

    args = parser.parse_args()

    if args.target in ['anime', 'general', 'all']:
        target_list = ['anime', 'general'] if args.target == 'all' else [args.target]
        for t in target_list:
            out = args.outdir if args.outdir else os.path.join(args.repo, 'external', f'ncnn-{t}', 'src')
            process_ncnn_shaders(t, args.repo, args.ndk, out)

    if args.target in ['filters', 'all']:
        process_filter_shaders(args.repo, args.ndk)
