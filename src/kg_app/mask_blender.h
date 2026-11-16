#pragma once
// M2h/M2i/M2l - tahap blending mask kulit di pipeline aplikasi (GL 3.3 sendiri,
// TANPA menyentuh source gpupixel).
// Perbaikan M2l:
//  - Double-buffered tex_out (tex_out_pair[2]) agar display tidak pernah
//    membaca tekstur saat FBO sedang dirender atau gagal di tengah jalan.
//  - Kebersihan state GL penuh (disable GL_SCISSOR_TEST, GL_CULL_FACE,
//    GL_STENCIL_TEST, GL_BLEND; reset glColorMask, GL_UNPACK_ALIGNMENT, dan
//    glActiveTexture(GL_TEXTURE0)) karena ImGui meninggalkan scissor/blend aktif.
//  - Texture completeness eksplisit (BASE_LEVEL/MAX_LEVEL=0, LINEAR, CLAMP) pada
//    SEMUA tekstur termasuk tex_raw, tex_beauty, tex_mask, tex_out_pair, tex_probe.
//  - Pass overlay F6 dipisah ke tex_out_debug agar tex_out (dan dump F5) SELALU
//    bersih dari overlay debug.
//  - Probe luma 16x16 cepat (1 KB) untuk swa-uji berkelanjutan frame hitam.
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "kgcore/log.h"
#include "kgcore/skin_mask.h"
#include "kgcore/face_mesh_topology.h"

struct MaskBlender {
  using GetQueryObjectUi64Proc = void (APIENTRYP)(GLuint, GLenum, GLuint64*);
  static constexpr GLenum kTimeElapsedQuery = 0x88BF;  // GL_TIME_ELAPSED
  static constexpr int kProbeSize = 16;

  // state inti
  GLuint prog_final = 0, prog_encode = 0, prog_blur = 0;
  GLuint prog_face = 0, prog_excl = 0, prog_excl_blur = 0, prog_compose_mask = 0;
  GLuint prog_debug_mask = 0, prog_probe = 0;
  GLuint vao = 0, fbo = 0, mesh_vao = 0, mesh_vbo = 0;
  GLuint tex_raw = 0, tex_beauty = 0, tex_mask = 0, tex_cpu_mask = 0;
  GLuint tex_face = 0, tex_face_prob = 0, tex_excl = 0, tex_excl_blur = 0;
  GLuint tex_out_pair[2] = {0, 0};
  int out_front_idx = 0;
  GLuint tex_out = 0;        // selalu menunjuk ke tex_out_pair[out_front_idx]
  GLuint tex_out_debug = 0;  // tex_out + lapisan merah F6 (hanya untuk display)
  GLuint tex_probe = 0;      // 16x16 RGBA8 untuk baca-balik luma cepat
  GLuint tex_stat = 0, tex_tmp = 0;
  GLuint mask_timer_query = 0;
  GetQueryObjectUi64Proc get_query_object_ui64 = nullptr;
  bool timer_query_supported = false;
  bool mask_query_pending = false;
  double last_gpu_mask_ms = 0.0;
  uint64_t mask_gen_frames = 0;
  int out_w = 0, out_h = 0;
  int out_pair_w[2] = {0, 0}, out_pair_h[2] = {0, 0};
  int debug_w = 0, debug_h = 0;
  int mask_w = 0, mask_h = 0;
  int cpu_mask_w = 0, cpu_mask_h = 0;
  int face_prob_w = 0, face_prob_h = 0;
  int target_w = 0, target_h = 0;
  int raw_w = 0, raw_h = 0, beauty_w = 0, beauty_h = 0;
  int stat_w = 0, stat_h = 0, tmp_w = 0, tmp_h = 0;
  double cpu_mask_submit_ms = 0.0;
  bool ready = false;         // tex_out pernah terisi
  bool debug_ready = false;   // tex_out_debug valid untuk frame terakhir
  bool init_failed = false;   // shader/FBO gagal - pakai jalur lama
  bool has_stats = true;      // RGBA16F bisa dirender (guided aktif)
  bool disabled = false;      // M2i/M2l: dimatikan (flag --no-mask-gl / swa-uji)
  bool warned_no_stats = false;
  std::string last_error;

  GLuint DisplayTex(bool debug_mask) const {
    if (debug_mask && debug_ready && tex_out_debug) return tex_out_debug;
    return tex_out;
  }

  bool Init() {
    if (init_failed) return false;
    if (prog_final) return true;
    static const char* kVs = R"(#version 330 core
out vec2 v_uv;
void main() {
  const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  vec2 pos = p[gl_VertexID];
  v_uv = pos * 0.5 + 0.5;
  gl_Position = vec4(pos, 0.0, 1.0);
})";
    static const char* kFsEncode = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_beauty;
void main() {
  vec3 c = texture(u_beauty, v_uv).rgb;
  float y = dot(c, vec3(0.299, 0.587, 0.114));
  frag = vec4(y, y * y, 0.0, 1.0);
})";
    static const char* kFsBlur = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_in;
uniform vec2 u_dir;
uniform int u_radius;
void main() {
  float sum_y = 0.0, sum_y2 = 0.0;
  int n = 0;
  for (int i = -u_radius; i <= u_radius; ++i) {
    vec2 uv = v_uv + u_dir * float(i);
    vec4 t = texture(u_in, uv);
    sum_y += t.r;
    sum_y2 += t.g;
    ++n;
  }
  frag = vec4(sum_y / float(n), sum_y2 / float(n), 0.0, 1.0);
})";
    // Final: bersih TANPA overlay debug F6 (F6 dirender terpisah ke tex_out_debug).
    static const char* kFsFinal = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_raw;
uniform sampler2D u_beauty;
uniform sampler2D u_mask;
uniform sampler2D u_stat;
uniform float u_gain;
uniform float u_smooth_amount;
uniform float u_chroma_retain;
uniform float u_smooth_eps;
uniform float u_lift;
uniform int u_has_stats;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 ycbcr(float r, float g, float b) {
  float y = luma(vec3(r, g, b));
  return vec3(y, 128.0 + 0.564 * (b - y), 128.0 + 0.713 * (r - y));
}
vec3 to_rgb(float y, float cb, float cr) {
  float r = y + 1.402 * (cr - 128.0);
  float b = y + 1.772 * (cb - 128.0);
  float g = (y - 0.299 * r - 0.114 * b) / 0.587;
  return clamp(vec3(r, g, b), 0.0, 255.0);
}
void main() {
  vec3 raw255 = texture(u_raw, v_uv).rgb * 255.0;
  vec3 beauty255 = texture(u_beauty, v_uv).rgb * 255.0;
  float m = clamp(texture(u_mask, v_uv).r * u_gain, 0.0, 1.0);
  if (m < (8.0 / 255.0)) { frag = vec4(raw255 / 255.0, 1.0); return; }

  float yb = luma(beauty255) / 255.0;
  float ys = yb;
  if (u_has_stats == 1 && u_smooth_amount > 0.0) {
    vec2 st = texture(u_stat, v_uv).rg;
    float var = max(st.g - st.r * st.r, 0.0);
    float a = var / (var + max(u_smooth_eps, 1e-6));
    float yg = a * yb + (1.0 - a) * st.r;
    ys = mix(yb, yg, u_smooth_amount);
  }
  float dy = (ys - yb) * 255.0;
  vec3 cand255 = clamp(beauty255 + vec3(dy), 0.0, 255.0);

  vec3 mixed255 = mix(raw255, cand255, m);
  if (u_lift > 0.0) mixed255 += (u_lift * m) * (255.0 - mixed255);
  vec3 cm = ycbcr(mixed255.r, mixed255.g, mixed255.b);
  vec3 crw = ycbcr(raw255.r, raw255.g, raw255.b);
  float cb = mix(cm.g, crw.g, u_chroma_retain);
  float cr = mix(cm.b, crw.b, u_chroma_retain);
  vec3 out255 = to_rgb(cm.r, cb, cr);
  frag = vec4(out255 / 255.0, 1.0);
})";
    static const char* kFsDebugMask = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_out;
uniform sampler2D u_mask;
uniform float u_gain;
void main() {
  vec3 c = texture(u_out, v_uv).rgb;
  float m = clamp(texture(u_mask, v_uv).r * u_gain, 0.0, 1.0);
  vec3 red = vec3(1.0, 30.0 / 255.0, 30.0 / 255.0);
  frag = vec4(mix(c, red, m * 0.55), 1.0);
})";
    static const char* kFsProbe = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
void main() {
  vec3 c = texture(u_tex, v_uv).rgb;
  frag = vec4(c, 1.0);
})";
    static const char* kVsMesh = R"(#version 330 core
layout(location=0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0); }
)";
    static const char* kFsMaskWhite = R"(#version 330 core
out vec4 frag;
void main() { frag = vec4(1.0, 0.0, 0.0, 1.0); }
)";
    static const char* kFsExclBlur = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_in;
uniform vec2 u_texel;
void main() {
  float sum = 0.0, norm = 0.0;
  for (int y=-2; y<=2; ++y) for (int x=-2; x<=2; ++x) {
    float w = exp(-0.25 * float(x*x + y*y));
    sum += texture(u_in, v_uv + vec2(x,y) * u_texel).r * w;
    norm += w;
  }
  frag = vec4(sum / norm, 0.0, 0.0, 1.0);
}
)";
    static const char* kFsComposeMask = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_cpu;
uniform sampler2D u_face;
uniform sampler2D u_face_prob;
uniform sampler2D u_excl;
uniform sampler2D u_excl_core;
uniform sampler2D u_raw;
uniform int u_has_seg;
uniform int u_has_color_gate;
uniform vec2 u_skin_cbcr;
uniform vec4 u_gate_limits;
uniform vec2 u_gate_radii;
uniform float u_seg_body_gain;
void main() {
  float mesh_coverage = texture(u_face, v_uv).r;
  // M2m (3b): di dalam jaring wajah, mask HANYA bergantung pada landmark frame
  // saat ini (tidak dipotong oleh u_face_prob seg yang bisa tertinggal).
  float mesh = mesh_coverage;
  float body = texture(u_cpu, v_uv).r * u_seg_body_gain;
  // Di sekitar batas wajah, pengaruh seg dibatasi & di-feather ke batas mesh.
  float near_face_fade = 1.0 - smoothstep(0.0, 0.5, mesh_coverage);
  body *= near_face_fade;
  if (u_has_color_gate == 1 && mesh_coverage < 0.5) {
    vec3 rgb = texture(u_raw, v_uv).rgb * 255.0;
    float y = dot(rgb, vec3(0.299, 0.587, 0.114));
    float cb = 128.0 + 0.564 * (rgb.b - y);
    float cr = 128.0 + 0.713 * (rgb.r - y);
    if (cb < u_gate_limits.x || cb > u_gate_limits.y ||
        cr < u_gate_limits.z || cr > u_gate_limits.w) {
      body = 0.0;
    } else {
      float d = length(vec2(cb, cr) - u_skin_cbcr);
      if (d > u_gate_radii.x)
        body *= clamp(1.0 - (d - u_gate_radii.x) /
                              max(0.001, u_gate_radii.y - u_gate_radii.x),
                      0.0, 1.0);
    }
  }
  float v = max(body, mesh);
  // M2m: pertahankan inti eksklusi >= 0.95 (1.0 di dalam core) sekaligus
  // feather halus di luar core dari tex_excl_blur.
  float excl = max(texture(u_excl_core, v_uv).r, texture(u_excl, v_uv).r);
  v *= 1.0 - clamp(excl, 0.0, 1.0);
  if (v < 0.02) v = 0.0;
  frag = vec4(v, 0.0, 0.0, 1.0);
}
)";
    prog_face = Build(GL_FRAGMENT_SHADER, kFsMaskWhite, "mask_gen.face");
    prog_excl = Build(GL_FRAGMENT_SHADER, kFsMaskWhite, "mask_gen.exclude");
    prog_excl_blur = Build(GL_FRAGMENT_SHADER, kFsExclBlur, "mask_gen.exclude_blur");
    prog_compose_mask = Build(GL_FRAGMENT_SHADER, kFsComposeMask, "mask_gen.compose");
    prog_debug_mask = Build(GL_FRAGMENT_SHADER, kFsDebugMask, "mask_blender.debug_mask");
    prog_probe = Build(GL_FRAGMENT_SHADER, kFsProbe, "mask_blender.probe");
    if (!prog_face || !prog_excl || !prog_excl_blur || !prog_compose_mask ||
        !prog_debug_mask || !prog_probe) {
      init_failed = true;
      return false;
    }

    prog_encode = Build(GL_FRAGMENT_SHADER, kFsEncode, "mask_blender.encode");
    prog_blur = Build(GL_FRAGMENT_SHADER, kFsBlur, "mask_blender.blur");
    prog_final = Build(GL_FRAGMENT_SHADER, kFsFinal, "mask_blender.final");
    if (!prog_encode || !prog_blur || !prog_final) {
      init_failed = true;
      return false;
    }
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs, "mask_blender.vs");
    if (!vs) {
      init_failed = true;
      return false;
    }
    if (!Link(prog_encode, vs) || !Link(prog_blur, vs) ||
        !Link(prog_final, vs) || !Link(prog_debug_mask, vs) ||
        !Link(prog_probe, vs)) {
      glDeleteShader(vs);
      init_failed = true;
      return false;
    }
    GLuint mesh_vs = Compile(GL_VERTEX_SHADER, kVsMesh, "mask_gen.mesh_vs");
    if (!mesh_vs || !Link(prog_face, mesh_vs) || !Link(prog_excl, mesh_vs)) {
      if (mesh_vs) glDeleteShader(mesh_vs);
      glDeleteShader(vs);
      init_failed = true;
      return false;
    }
    glDeleteShader(mesh_vs);
    if (!Link(prog_excl_blur, vs) || !Link(prog_compose_mask, vs)) {
      glDeleteShader(vs);
      init_failed = true;
      return false;
    }
    glDeleteShader(vs);

    glGenVertexArrays(1, &vao);
    glGenVertexArrays(1, &mesh_vao);
    glGenBuffers(1, &mesh_vbo);
    glBindVertexArray(mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glGenFramebuffers(1, &fbo);

    get_query_object_ui64 = reinterpret_cast<GetQueryObjectUi64Proc>(
        glfwGetProcAddress("glGetQueryObjectui64v"));
    const bool arb_timer_query =
        glfwExtensionSupported("GL_ARB_timer_query") == GLFW_TRUE;
    const bool ext_timer_query =
        glfwExtensionSupported("GL_EXT_timer_query") == GLFW_TRUE;
    int gl_major = 0, gl_minor = 0;
    const auto* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (gl_version) std::sscanf(gl_version, "%d.%d", &gl_major, &gl_minor);
    const bool timer_query_core =
        gl_major > 3 || (gl_major == 3 && gl_minor >= 3);
    if (!get_query_object_ui64 && ext_timer_query) {
      get_query_object_ui64 = reinterpret_cast<GetQueryObjectUi64Proc>(
          glfwGetProcAddress("glGetQueryObjectui64vEXT"));
    }
    timer_query_supported = get_query_object_ui64 &&
        (timer_query_core || arb_timer_query || ext_timer_query);
    if (timer_query_supported) {
      glGenQueries(1, &mask_timer_query);
    } else {
      kgcore::Log("mask_gen", "timer query GPU tidak tersedia; hanya CPU submit timing yang dilaporkan");
    }

    glGenTextures(1, &tex_raw);
    glGenTextures(1, &tex_beauty);
    glGenTextures(1, &tex_mask);
    glGenTextures(1, &tex_cpu_mask);
    glGenTextures(1, &tex_face);
    glGenTextures(1, &tex_face_prob);
    glGenTextures(1, &tex_excl);
    glGenTextures(1, &tex_excl_blur);
    glGenTextures(2, tex_out_pair);
    glGenTextures(1, &tex_out_debug);
    glGenTextures(1, &tex_probe);
    glGenTextures(1, &tex_stat);
    glGenTextures(1, &tex_tmp);

    // Parameter sampling & completeness eksplisit untuk SELURUH tekstur.
    SetLinear(tex_raw);
    SetLinear(tex_beauty);
    SetLinear(tex_mask);
    SetLinear(tex_cpu_mask);
    SetLinear(tex_face);
    SetLinear(tex_face_prob);
    SetLinear(tex_excl);
    SetLinear(tex_excl_blur);
    SetLinear(tex_out_pair[0]);
    SetLinear(tex_out_pair[1]);
    SetLinear(tex_out_debug);
    SetLinear(tex_probe);
    SetLinear(tex_stat);
    SetLinear(tex_tmp);
    glBindTexture(GL_TEXTURE_2D, tex_probe);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kProbeSize, kProbeSize, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    out_front_idx = 0;
    tex_out = tex_out_pair[0];

    kgcore::Log("blend",
                "init OK: prog(final/encode/blur/dbg/probe)=%u/%u/%u/%u/%u vao=%u fbo=%u "
                "tex out=(%u,%u) dbg=%u stat=%u tmp=%u raw/beauty/mask=%u/%u/%u",
                prog_final, prog_encode, prog_blur, prog_debug_mask, prog_probe,
                vao, fbo, tex_out_pair[0], tex_out_pair[1], tex_out_debug,
                tex_stat, tex_tmp, tex_raw, tex_beauty, tex_mask);
    return true;
  }

  void Destroy() {
    if (prog_final) glDeleteProgram(prog_final);
    if (prog_encode) glDeleteProgram(prog_encode);
    if (prog_blur) glDeleteProgram(prog_blur);
    if (prog_face) glDeleteProgram(prog_face);
    if (prog_excl) glDeleteProgram(prog_excl);
    if (prog_excl_blur) glDeleteProgram(prog_excl_blur);
    if (prog_compose_mask) glDeleteProgram(prog_compose_mask);
    if (prog_debug_mask) glDeleteProgram(prog_debug_mask);
    if (prog_probe) glDeleteProgram(prog_probe);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (mesh_vao) glDeleteVertexArrays(1, &mesh_vao);
    if (mesh_vbo) glDeleteBuffers(1, &mesh_vbo);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (mask_timer_query) glDeleteQueries(1, &mask_timer_query);
    const GLuint texes[] = {
        tex_raw, tex_beauty, tex_mask, tex_cpu_mask, tex_face, tex_face_prob,
        tex_excl, tex_excl_blur, tex_out_pair[0], tex_out_pair[1],
        tex_out_debug, tex_probe, tex_stat, tex_tmp};
    glDeleteTextures(14, texes);
    prog_final = prog_encode = prog_blur = prog_face = prog_excl =
        prog_excl_blur = prog_compose_mask = prog_debug_mask = prog_probe = 0;
    vao = mesh_vao = mesh_vbo = fbo = 0;
    tex_raw = tex_beauty = tex_mask = tex_cpu_mask = tex_face = tex_face_prob =
        tex_excl = tex_excl_blur = tex_out = tex_out_debug = tex_probe =
        tex_stat = tex_tmp = 0;
    tex_out_pair[0] = tex_out_pair[1] = 0;
    out_front_idx = 0;
    out_w = out_h = mask_w = mask_h = 0;
    out_pair_w[0] = out_pair_w[1] = out_pair_h[0] = out_pair_h[1] = 0;
    debug_w = debug_h = 0;
    cpu_mask_w = cpu_mask_h = face_prob_w = face_prob_h = 0;
    target_w = target_h = raw_w = raw_h = beauty_w = beauty_h = 0;
    stat_w = stat_h = tmp_w = tmp_h = 0;
    ready = false;
    debug_ready = false;
    mask_timer_query = 0;
    mask_query_pending = false;
    last_gpu_mask_ms = 0.0;
    mask_gen_frames = 0;
  }

  // F5 selalu membaca tex_out bersih (tidak pernah mengandung overlay F6).
  bool ReadbackOutput(std::vector<uint8_t>* rgba) const {
    if (!rgba || !ready || !tex_out || out_w <= 0 || out_h <= 0) return false;
    rgba->resize(static_cast<size_t>(out_w) * out_h * 4);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return glGetError() == GL_NO_ERROR;
  }
  bool ReadbackMask(std::vector<uint8_t>* mask) const {
    if (!mask || !tex_mask || mask_w <= 0 || mask_h <= 0) return false;
    mask->resize(static_cast<size_t>(mask_w) * mask_h);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, mask->data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return glGetError() == GL_NO_ERROR;
  }

  // M2l: probe luma cepat (16x16 = 1 KB) dari tex_out aktif untuk swa-uji
  // berkelanjutan per iterasi render tanpa stalling baca 8.3 MB.
  bool ProbeOutputLuma(float* out_y) {
    if (!out_y || !ready || !tex_out || !tex_probe || out_w <= 0 || out_h <= 0)
      return false;
    ResetRenderState();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex_probe, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RestoreStateAfterPass();
      return false;
    }
    glViewport(0, 0, kProbeSize, kProbeSize);
    glUseProgram(prog_probe);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glUniform1i(glGetUniformLocation(prog_probe, "u_tex"), 0);
    glBindVertexArray(vao);
    Draw();

    uint8_t buf[kProbeSize * kProbeSize * 4]{};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_probe);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    RestoreStateAfterPass();
    if (glGetError() != GL_NO_ERROR) return false;
    double sum = 0.0;
    for (int i = 0; i < kProbeSize * kProbeSize; ++i) {
      const uint8_t* p = buf + i * 4;
      sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    }
    *out_y = static_cast<float>(sum / (kProbeSize * kProbeSize));
    return true;
  }

  // Render pass overlay F6 terpisah ke tex_out_debug (agar tex_out tetap bersih).
  bool UpdateDebugTexture(float gain) {
    if (!ready || !tex_out || !tex_mask || out_w <= 0 || out_h <= 0) {
      debug_ready = false;
      return false;
    }
    ResetRenderState();
    glBindTexture(GL_TEXTURE_2D, tex_out_debug);
    if (debug_w != out_w || debug_h != out_h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out_w, out_h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      SetLinear(tex_out_debug);
      debug_w = out_w;
      debug_h = out_h;
    }
    AttachRGBA(tex_out_debug);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RestoreStateAfterPass();
      debug_ready = false;
      return false;
    }
    glViewport(0, 0, out_w, out_h);
    glUseProgram(prog_debug_mask);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glUniform1i(glGetUniformLocation(prog_debug_mask, "u_out"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    glUniform1i(glGetUniformLocation(prog_debug_mask, "u_mask"), 1);
    glUniform1f(glGetUniformLocation(prog_debug_mask, "u_gain"),
                std::clamp(gain, 0.0f, 1.0f));
    glBindVertexArray(vao);
    Draw();
    RestoreStateAfterPass();
    debug_ready = (LogGl("debug_mask pass") == GL_NO_ERROR);
    return debug_ready;
  }

  bool Blend(const kgcore::BeautyComposeConfig& cfg, const uint8_t* raw, int w,
             int h, const uint8_t* beauty, const uint8_t* mask, int mw, int mh,
             float gain, bool debug_mask, const float* landmarks_xy = nullptr,
             int landmark_count = 0,
             const uint8_t* face_probability = nullptr,
             bool has_seg_probability = false, float skin_cb0 = 128.0f,
             float skin_cr0 = 128.0f, bool has_color_calibration = false,
             const kgcore::SkinMaskConfig* skin_cfg = nullptr,
             bool use_gpu_mask_gen = true, float seg_body_gain = 1.0f) {
    if (disabled) return false;
    if (!Init() || !raw || !beauty || !mask || w <= 0 || h <= 0 || mw <= 0 ||
        mh <= 0) {
      return false;
    }
    const auto cpu_submit_t0 = std::chrono::steady_clock::now();
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {}
    ResetRenderState();

    UploadRGBA(tex_raw, raw, w, h);
    UploadRGBA(tex_beauty, beauty, w, h);
    UploadCpuMask(mask, mw, mh);

    bool gpu_mask_ok = false;
    if (use_gpu_mask_gen && landmarks_xy && landmark_count >= 468) {
      gpu_mask_ok = GenerateMaskGpu(
          landmarks_xy, landmark_count, w, h, mw, mh, face_probability,
          has_seg_probability, skin_cb0, skin_cr0, has_color_calibration,
          skin_cfg, seg_body_gain);
      cpu_mask_submit_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - cpu_submit_t0)
                               .count();
    }
    if (!gpu_mask_ok) {
      // Fallback sinkron ke mask CPU (baik saat landmark kosong, --no-mask-gen-gpu,
      // maupun saat FBO mask_gen gagal).
      UploadMask(mask, mw, mh);
    }

    // Double buffer: tulis selalu ke back buffer, swap ke front hanya bila sukses.
    const int back_idx = ready ? (1 - out_front_idx) : 0;
    const GLuint back_tex = tex_out_pair[back_idx];
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, back_tex);
    if (out_pair_w[back_idx] != w || out_pair_h[back_idx] != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      SetLinear(back_tex);
      out_pair_w[back_idx] = w;
      out_pair_h[back_idx] = h;
    }

    ResetRenderState();
    glBindVertexArray(vao);
    glViewport(0, 0, w, h);

    // ---- pass statistik guided (RGBA16F): encode + box H + box V ----
    if (has_stats && cfg.smooth_amount > 0.0f) {
      Attach16F(tex_stat, stat_w, stat_h, w, h);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        has_stats = false;
        if (!warned_no_stats) {
          warned_no_stats = true;
          kgcore::Log("blend",
                      "RGBA16F tidak bisa dirender di GPU ini - guided smoothing nonaktif (blend+chroma tetap jalan)");
        }
      } else {
        glUseProgram(prog_encode);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_beauty);
        glUniform1i(glGetUniformLocation(prog_encode, "u_beauty"), 0);
        Draw();
        Attach16F(tex_tmp, tmp_w, tmp_h, w, h);
        glUseProgram(prog_blur);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_stat);
        glUniform1i(glGetUniformLocation(prog_blur, "u_in"), 0);
        glUniform2f(glGetUniformLocation(prog_blur, "u_dir"),
                    1.0f / static_cast<float>(w), 0.0f);
        glUniform1i(glGetUniformLocation(prog_blur, "u_radius"),
                    std::max(1, cfg.smooth_radius));
        Draw();
        Attach16F(tex_stat, stat_w, stat_h, w, h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_tmp);
        glUniform2f(glGetUniformLocation(prog_blur, "u_dir"), 0.0f,
                    1.0f / static_cast<float>(h));
        Draw();
        if (LogGl("stat pass") != GL_NO_ERROR) {
          has_stats = false;
        }
      }
    }

    // ---- pass final -> back_tex ----
    AttachRGBA(back_tex);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RestoreStateAfterPass();
      last_error = "FBO tex_out tidak lengkap";
      kgcore::Log("blend", "GAGAL: FBO tex_out tidak lengkap (fbo=%u %dx%d)",
                  fbo, w, h);
      init_failed = true;
      return false;
    }
    glViewport(0, 0, w, h);
    glUseProgram(prog_final);
    auto u1 = [&](const char* n, int unit) {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D,
                    unit == 0 ? tex_raw : unit == 1 ? tex_beauty : unit == 2
                                                                             ? tex_mask
                                                                             : tex_stat);
      glUniform1i(glGetUniformLocation(prog_final, n), unit);
    };
    u1("u_raw", 0);
    u1("u_beauty", 1);
    u1("u_mask", 2);
    u1("u_stat", 3);
    glUniform1f(glGetUniformLocation(prog_final, "u_gain"),
                std::clamp(gain, 0.0f, 1.0f));
    glUniform1f(glGetUniformLocation(prog_final, "u_smooth_amount"),
                cfg.smooth_amount);
    glUniform1f(glGetUniformLocation(prog_final, "u_chroma_retain"),
                cfg.chroma_retain);
    glUniform1f(glGetUniformLocation(prog_final, "u_smooth_eps"), cfg.smooth_eps);
    glUniform1f(glGetUniformLocation(prog_final, "u_lift"),
                std::clamp(cfg.lift, 0.0f, 0.95f));
    glUniform1i(glGetUniformLocation(prog_final, "u_has_stats"),
                (has_stats && cfg.smooth_amount > 0.0f) ? 1 : 0);
    Draw();
    RestoreStateAfterPass();

    const GLenum err = LogGl("final pass");
    if (err != GL_NO_ERROR) {
      init_failed = true;
      return false;
    }

    // Swap front buffer hanya setelah final pass sukses 100%.
    out_front_idx = back_idx;
    tex_out = back_tex;
    out_w = w;
    out_h = h;
    ready = true;

    if (debug_mask) {
      UpdateDebugTexture(gain);
    } else {
      debug_ready = false;
    }
    return true;
  }

  // M2i swa-uji: baca kembali luma rata-rata keluaran (subsample 1/4).
  bool ReadOutputLuma(float* out_y) {
    if (!ready || !tex_out || out_w <= 0 || out_h <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(out_w) * out_h * 4);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    if (LogGl("selftest glGetTexImage") != GL_NO_ERROR) return false;
    double sum = 0.0;
    long n = 0;
    for (int y = 0; y < out_h; y += 4) {
      const uint8_t* row = buf.data() + static_cast<size_t>(y) * out_w * 4;
      for (int x = 0; x < out_w; x += 4) {
        const uint8_t* p = row + static_cast<size_t>(x) * 4;
        sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
        ++n;
      }
    }
    if (n == 0) return false;
    *out_y = static_cast<float>(sum / n);
    return true;
  }

 private:
  // ImGui meninggalkan GL_SCISSOR_TEST, GL_BLEND, dan active texture unit
  // di state terakhirnya; wajib dibersihkan sebelum/sesudah setiap pass FBO.
  static void ResetRenderState() {
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  static void RestoreStateAfterPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    for (int u = 5; u >= 0; --u) {
      glActiveTexture(GL_TEXTURE0 + u);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
  }
  static void SetLinear(GLuint tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  }
  void UploadRGBA(GLuint tex, const uint8_t* data, int w, int h) {
    int& tex_w = tex == tex_raw ? raw_w : beauty_w;
    int& tex_h = tex == tex_raw ? raw_h : beauty_h;
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (tex_w != w || tex_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      tex_w = w;
      tex_h = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA,
                    GL_UNSIGNED_BYTE, data);
    SetLinear(tex);
  }
  void UploadMask(const uint8_t* data, int w, int h) {
    UploadR8(tex_mask, data, w, h, mask_w, mask_h);
  }
  void UploadCpuMask(const uint8_t* data, int w, int h) {
    UploadR8(tex_cpu_mask, data, w, h, cpu_mask_w, cpu_mask_h);
  }
  void UploadFaceProbability(const uint8_t* data, int w, int h) {
    UploadR8(tex_face_prob, data, w, h, face_prob_w, face_prob_h);
  }
  void UploadR8(GLuint tex, const uint8_t* data, int w, int h, int& tex_w,
                int& tex_h) {
    if (!data || w <= 0 || h <= 0) return;
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (tex_w != w || tex_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED,
                   GL_UNSIGNED_BYTE, nullptr);
      tex_w = w;
      tex_h = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    data);
    SetLinear(tex);
  }
  void AllocateMaskTarget(GLuint tex, int w, int h) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                 nullptr);
    SetLinear(tex);
  }
  bool AttachMask(GLuint tex, int w, int h, const char* label) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
      kgcore::Log("mask_gen", "GAGAL: FBO %s tidak lengkap (status=0x%04X %dx%d)",
                  label, static_cast<unsigned>(st), w, h);
      return false;
    }
    glViewport(0, 0, w, h);
    return true;
  }
  bool GenerateMaskGpu(const float* xy_px, int count, int fw, int fh, int mw,
                       int mh, const uint8_t* face_probability,
                       bool has_seg_probability, float skin_cb0,
                       float skin_cr0, bool has_color_calibration,
                       const kgcore::SkinMaskConfig* skin_cfg,
                       float seg_body_gain) {
    if (!xy_px || count < 468 || fw <= 0 || fh <= 0) return false;
    if (mask_query_pending) {
      GLint available = GL_FALSE;
      glGetQueryObjectiv(mask_timer_query, GL_QUERY_RESULT_AVAILABLE, &available);
      if (available == GL_TRUE) {
        GLuint64 ns = 0;
        get_query_object_ui64(mask_timer_query, GL_QUERY_RESULT, &ns);
        last_gpu_mask_ms = static_cast<double>(ns) / 1000000.0;
        mask_query_pending = false;
      }
    }
    const bool measure_gpu = timer_query_supported && !mask_query_pending;
    if (measure_gpu) glBeginQuery(kTimeElapsedQuery, mask_timer_query);
    if (target_w != mw || target_h != mh) {
      AllocateMaskTarget(tex_face, mw, mh);
      AllocateMaskTarget(tex_excl, mw, mh);
      AllocateMaskTarget(tex_excl_blur, mw, mh);
      AllocateMaskTarget(tex_mask, mw, mh);
      target_w = mw;
      target_h = mh;
      mask_w = mw;
      mask_h = mh;
    }

    auto abort_gpu_mask = [&]() {
      if (measure_gpu) glEndQuery(kTimeElapsedQuery);
      RestoreStateAfterPass();
      return false;
    };

    // 1) FACEMESH_TESSELATION triangles
    std::vector<float> verts;
    const kgcore::TessTriangle* tris = kgcore::TessTriangles();
    const int tri_n = kgcore::TessTriangleCount();
    verts.reserve(static_cast<size_t>(tri_n) * 6);
    auto push_pt = [&](int idx) {
      verts.push_back(std::clamp(xy_px[idx * 2] / fw, 0.0f, 1.0f));
      verts.push_back(std::clamp(xy_px[idx * 2 + 1] / fh, 0.0f, 1.0f));
    };
    for (int i = 0; i < tri_n; ++i) {
      push_pt(tris[i].a); push_pt(tris[i].b); push_pt(tris[i].c);
    }
    if (!AttachMask(tex_face, mw, mh, "tex_face")) return abort_gpu_mask();
    ResetRenderState();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog_face);
    glBindVertexArray(mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 2));

    // 2) Exclusion polygons + ellipses
    kgcore::ExclusionShapes shapes = kgcore::BuildExclusionShapes(xy_px);
    std::vector<float> exverts;
    auto add_triangle = [&](float ax, float ay, float bx, float by, float cx,
                            float cy) {
      exverts.push_back(std::clamp(ax / fw, 0.0f, 1.0f));
      exverts.push_back(std::clamp(ay / fh, 0.0f, 1.0f));
      exverts.push_back(std::clamp(bx / fw, 0.0f, 1.0f));
      exverts.push_back(std::clamp(by / fh, 0.0f, 1.0f));
      exverts.push_back(std::clamp(cx / fw, 0.0f, 1.0f));
      exverts.push_back(std::clamp(cy / fh, 0.0f, 1.0f));
    };
    auto triangulate_loop = [&](const std::vector<int>& loop) {
      if (loop.size() < 3) return;
      std::vector<int> polygon = loop;
      double area = 0.0;
      for (size_t i = 0; i < polygon.size(); ++i) {
        const int a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        area += static_cast<double>(xy_px[a * 2]) * xy_px[b * 2 + 1] -
                static_cast<double>(xy_px[b * 2]) * xy_px[a * 2 + 1];
      }
      const float winding = area >= 0.0 ? 1.0f : -1.0f;
      auto orient = [&](int a, int b, int c) {
        return (xy_px[b * 2] - xy_px[a * 2]) *
                   (xy_px[c * 2 + 1] - xy_px[a * 2 + 1]) -
               (xy_px[b * 2 + 1] - xy_px[a * 2 + 1]) *
                   (xy_px[c * 2] - xy_px[a * 2]);
      };
      auto in_tri = [&](float px, float py, int a, int b, int c) {
        const float d0 = orient(a, b, c);
        if (std::fabs(d0) < 1e-5f) return false;
        const float d1 = (xy_px[b * 2] - xy_px[a * 2]) * (py - xy_px[a * 2 + 1]) -
                         (xy_px[b * 2 + 1] - xy_px[a * 2 + 1]) * (px - xy_px[a * 2]);
        const float d2 = (xy_px[c * 2] - xy_px[b * 2]) * (py - xy_px[b * 2 + 1]) -
                         (xy_px[c * 2 + 1] - xy_px[b * 2 + 1]) * (px - xy_px[b * 2]);
        const float d3 = (xy_px[a * 2] - xy_px[c * 2]) * (py - xy_px[c * 2 + 1]) -
                         (xy_px[a * 2 + 1] - xy_px[c * 2 + 1]) * (px - xy_px[c * 2]);
        return winding > 0 ? (d1 >= -1e-4f && d2 >= -1e-4f && d3 >= -1e-4f)
                           : (d1 <= 1e-4f && d2 <= 1e-4f && d3 <= 1e-4f);
      };
      size_t guard = 0;
      while (polygon.size() > 3 && guard++ < loop.size() * loop.size()) {
        bool clipped = false;
        for (size_t i = 0; i < polygon.size(); ++i) {
          const int a = polygon[(i + polygon.size() - 1) % polygon.size()];
          const int b = polygon[i];
          const int c = polygon[(i + 1) % polygon.size()];
          if (orient(a, b, c) * winding <= 1e-5f) continue;
          bool contains = false;
          for (int pidx : polygon) {
            if (pidx == a || pidx == b || pidx == c) continue;
            if (in_tri(xy_px[pidx * 2], xy_px[pidx * 2 + 1], a, b, c)) {
              contains = true; break;
            }
          }
          if (contains) continue;
          add_triangle(xy_px[a * 2], xy_px[a * 2 + 1],
                       xy_px[b * 2], xy_px[b * 2 + 1],
                       xy_px[c * 2], xy_px[c * 2 + 1]);
          polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(i));
          clipped = true;
          break;
        }
        if (!clipped) break;
      }
      if (polygon.size() == 3) {
        const int a = polygon[0], b = polygon[1], c = polygon[2];
        add_triangle(xy_px[a * 2], xy_px[a * 2 + 1],
                     xy_px[b * 2], xy_px[b * 2 + 1],
                     xy_px[c * 2], xy_px[c * 2 + 1]);
      } else {
        kgcore::Log("mask_gen", "ear clipping gagal untuk exclusion loop (%zu titik); contour dilewati",
                    loop.size());
      }
    };
    for (const auto& loop : shapes.loops) triangulate_loop(loop);
    for (const auto& e : shapes.ellipses) {
      constexpr int kSteps = 24;
      const float rx_eff = std::max(e.rx, 0.80f * static_cast<float>(fw) / mw);
      const float ry_eff = std::max(e.ry, 0.80f * static_cast<float>(fh) / mh);
      for (int i = 0; i < kSteps; ++i) {
        const float a0 = 6.28318530718f * i / kSteps;
        const float a1 = 6.28318530718f * (i + 1) / kSteps;
        add_triangle(e.cx, e.cy, e.cx + rx_eff * std::cos(a0),
                     e.cy + ry_eff * std::sin(a0),
                     e.cx + rx_eff * std::cos(a1),
                     e.cy + ry_eff * std::sin(a1));
      }
    }
    if (!AttachMask(tex_excl, mw, mh, "tex_excl")) return abort_gpu_mask();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendEquation(GL_MAX); glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(prog_excl);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(exverts.size() * sizeof(float)),
                 exverts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(exverts.size() / 2));
    glDisable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);

    // 3) Feather eksklusi -> 4) compose tex_mask
    if (!AttachMask(tex_excl_blur, mw, mh, "tex_excl_blur")) return abort_gpu_mask();
    glUseProgram(prog_excl_blur);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_excl);
    glUniform1i(glGetUniformLocation(prog_excl_blur, "u_in"), 0);
    glUniform2f(glGetUniformLocation(prog_excl_blur, "u_texel"),
                1.0f / mw, 1.0f / mh);
    glBindVertexArray(vao); Draw();

    if (face_probability) {
      UploadFaceProbability(face_probability, mw, mh);
    } else {
      std::vector<uint8_t> full(static_cast<size_t>(mw) * mh, 255);
      UploadFaceProbability(full.data(), mw, mh);
    }
    if (!AttachMask(tex_mask, mw, mh, "tex_mask")) return abort_gpu_mask();
    glUseProgram(prog_compose_mask);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_cpu_mask);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_cpu"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex_face);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_face"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex_excl_blur);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_excl"), 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, tex_face_prob);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_face_prob"), 3);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, tex_raw);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_raw"), 4);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, tex_excl);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_excl_core"), 5);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_has_seg"),
                has_seg_probability ? 1 : 0);
    glUniform1i(glGetUniformLocation(prog_compose_mask, "u_has_color_gate"),
                has_color_calibration ? 1 : 0);
    glUniform2f(glGetUniformLocation(prog_compose_mask, "u_skin_cbcr"),
                skin_cb0, skin_cr0);
    const kgcore::SkinMaskConfig default_skin_cfg{};
    const kgcore::SkinMaskConfig& gate_cfg = skin_cfg ? *skin_cfg : default_skin_cfg;
    glUniform4f(glGetUniformLocation(prog_compose_mask, "u_gate_limits"),
                gate_cfg.cb_min, gate_cfg.cb_max, gate_cfg.cr_min,
                gate_cfg.cr_max);
    glUniform2f(glGetUniformLocation(prog_compose_mask, "u_gate_radii"),
                gate_cfg.chroma_radius * gate_cfg.chroma_feather,
                gate_cfg.chroma_radius * gate_cfg.seg_gate_fade);
    glUniform1f(glGetUniformLocation(prog_compose_mask, "u_seg_body_gain"),
                std::clamp(seg_body_gain, 0.0f, 1.0f));
    Draw();
    RestoreStateAfterPass();
    if (measure_gpu) {
      glEndQuery(kTimeElapsedQuery);
      mask_query_pending = true;
    }
    ++mask_gen_frames;
    if ((mask_gen_frames % 90) == 0) {
      if (timer_query_supported) {
        kgcore::Log("mask_gen", "CPU mask submit=%.3f ms | GPU raster+exclude+compose=%.3f ms (mask %dx%d), frame=%llu",
                    cpu_mask_submit_ms, last_gpu_mask_ms, mw, mh,
                    static_cast<unsigned long long>(mask_gen_frames));
      } else {
        kgcore::Log("mask_gen", "CPU mask submit=%.3f ms | GPU timing unavailable (mask %dx%d), frame=%llu",
                    cpu_mask_submit_ms, mw, mh,
                    static_cast<unsigned long long>(mask_gen_frames));
      }
    }
    return LogGl("mask_gen") == GL_NO_ERROR;
  }
  void AttachRGBA(GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
  }
  void Attach16F(GLuint tex, int& tex_w, int& tex_h, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (tex_w != w || tex_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA,
                   GL_FLOAT, nullptr);
      tex_w = w;
      tex_h = h;
      SetLinear(tex);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
  }
  static void Draw() { glDrawArrays(GL_TRIANGLES, 0, 3); }

  static GLenum LogGl(const char* stage) {
    GLenum first = GL_NO_ERROR;
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
      if (first == GL_NO_ERROR) first = e;
      kgcore::Log("blend", "glGetError=0x%04X setelah tahap %s",
                  static_cast<unsigned>(e), stage);
    }
    return first;
  }

  GLuint Compile(GLenum type, const char* src, const char* label) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char logbuf[1024] = {};
      glGetShaderInfoLog(sh, sizeof(logbuf), nullptr, logbuf);
      last_error = std::string(label) + ": " + logbuf;
      kgcore::Log("blend", "GAGAL kompilasi %s: %s", label, logbuf);
      glDeleteShader(sh);
      return 0;
    }
    kgcore::Log("blend", "kompilasi %s OK", label);
    return sh;
  }
  GLuint Build(GLenum type, const char* src, const char* label) {
    GLuint sh = Compile(type, src, label);
    if (!sh) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, sh);
    glDeleteShader(sh);
    return p;
  }
  bool Link(GLuint prog, GLuint vs) {
    glAttachShader(prog, vs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
      char logbuf[1024] = {};
      glGetProgramInfoLog(prog, sizeof(logbuf), nullptr, logbuf);
      last_error = "link: " + std::string(logbuf);
      kgcore::Log("blend", "GAGAL link program %u: %s", prog, logbuf);
      return false;
    }
    return true;
  }
};
