/* GLSL (Vulkan dialect) -> SPIR-V through the system's shaderc (libshaderc_shared, Arch package `shaderc`), linked
 * directly into smo_native (the interface of ssbu-recomp's vk_glslang_shim.c).
 *
 * Settings (those of ssbu-recomp's shim): Vulkan 1.3 environment, SPIR-V 1.6, no optimisation (shaderc's default
 * level is zero), entry point "main". Every source mx_translate.c / nvn_vk_draw.c produce starts with "#version 460".
 * stage: 0 vertex, 1 fragment, 2 geometry, 5 compute (the MX_* numbers of mx_translate.h).
 * Returns 1 and a malloc'ed word array (free it with smo_spirv_free), or 0 with the compiler's messages in log.
 * Thread-safe: the compiler object is made once; shaderc compilers are safe to use from several threads.
 * smo_shaderc_selftest() compiles one vertex shader (smo_native --shaderc-selftest, run by build_exe.py). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shaderc/shaderc.h>

static shaderc_compiler_t g_compiler;
static shaderc_compile_options_t g_options;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void init_once(void) {
    g_compiler = shaderc_compiler_initialize();
    g_options = shaderc_compile_options_initialize();
    if (!g_compiler || !g_options) return;
    shaderc_compile_options_set_target_env(g_options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(g_options, shaderc_spirv_version_1_6);
}

void smo_spirv_free(uint32_t *words) { free(words); }

int smo_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size) {
    int ok = 0;
    if (log && log_size) log[0] = 0;
    *words = NULL; *count = 0;
    pthread_once(&g_once, init_once);
    if (!g_compiler || !g_options) { if (log) snprintf(log, log_size, "shaderc initialisation failed"); return 0; }
    int kind = stage == 0 ? shaderc_vertex_shader : stage == 1 ? shaderc_fragment_shader : stage == 2 ? shaderc_geometry_shader
             : stage == 5 ? shaderc_compute_shader : -1;
    if (kind < 0) { if (log) snprintf(log, log_size, "stage %d is not vertex (0), fragment (1), geometry (2) or compute (5)", stage); return 0; }
    shaderc_compilation_result_t r = shaderc_compile_into_spv(g_compiler, src, strlen(src), (shaderc_shader_kind)kind, "nvn_stage", "main", g_options);
    if (!r) { if (log) snprintf(log, log_size, "shaderc_compile_into_spv returned no result"); return 0; }
    const char *msg = shaderc_result_get_error_message(r);
    if (msg && msg[0] && log) snprintf(log, log_size, "%s", msg);
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        size_t n = shaderc_result_get_length(r) / 4;
        if (n) {
            *words = malloc(n * sizeof(uint32_t));
            if (*words) { memcpy(*words, shaderc_result_get_bytes(r), n * 4); *count = n; ok = 1; }
        }
    }
    shaderc_result_release(r);
    return ok;
}

/* 0 when one vertex shader compiles to SPIR-V with the right magic number; prints one line either way. */
int smo_shaderc_selftest(void) {
    uint32_t *words; size_t count; char log[4096];
    int ok = smo_glsl_to_spirv(0, "#version 460\nvoid main() { gl_Position = vec4(float(gl_VertexIndex), 0.0, 0.0, 1.0); }\n",
                               &words, &count, log, sizeof log);
    uint32_t magic = ok && count ? words[0] : 0;
    printf("shader compiler: test vertex shader -> %zu SPIR-V words, magic 0x%08x %s%s%s\n", ok ? count : 0, magic,
           magic == 0x07230203 ? "OK" : "FAILED", log[0] ? " log: " : "", log);
    if (ok) smo_spirv_free(words);
    return magic == 0x07230203 ? 0 : 1;
}
