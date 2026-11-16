#pragma once
// M2h/M2i - tahap blending mask kulit di pipeline aplikasi (GL 3.3 sendiri,
// TANPA menyentuh source gpupixel). Rantai tetap:
//   raw -> gpupixel (BeautyFace whole-frame) -> sink
// lalu di sini:
//   1) guided smoothing luma pada kandidat beauty (pass statistik 16F:
//      encode Y,Y^2 -> box blur H -> box blur V);
//   2) final: mixed = mix(raw, cand, mask*gain);
//      CbCr_out = lerp(CbCr(mixed), CbCr(raw), chroma_retain)  <- M2i (3a):
//      cerahkan lewat luma saja, chroma mentah dipertahankan (anti cast
//      dingin/pink); bila mask*gain == 0 keluaran DIJAMIN byte = raw.
//
// Catatan M2i (bug preview hitam): tekstur keluaran HARUS punya
// TEXTURE_MIN/MAG filter eksplisit - default MIN_FILTER
// NEAREST_MIPMAP_LINEAR tanpa mipmap membuat tekstur INCOMPLETE dan semua
// sampling menghasilkan hitam tanpa error GL (glErr tetap 0).
//
// Jujur: komponen ini hanya terverifikasi di Windows nyata (GL/GPU); unit
// test Linux menutupi matematika mask/komposit/metrik (kgcore).
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
  static constexpr GLenum kTimeElapsedQuery = 0x88BF;  // GL_TIME_ELAPSED (ARB_timer_query)
  // state inti
  GLuint prog_final = 0, prog_encode = 0, prog_blur = 0;
  GLuint prog_face = 0, prog_excl = 0, prog_excl_blur = 0, prog_compose_mask = 0;
  GLuint vao = 0, fbo = 0, mesh_vao = 0, mesh_vbo = 0;
  GLuint tex_raw = 0, tex_beauty = 0, tex_mask = 0, tex_cpu_mask = 0;
  GLuint tex_face = 0, tex_face_prob = 0, tex_excl = 0, tex_excl_blur = 0, tex_out = 0;
  GLuint tex_stat = 0, tex_tmp = 0;
  GLuint mask_timer_query = 0;
  GetQueryObjectUi64Proc get_query_object_ui64 = nullptr;
  bool timer_query_supported = false;
  bool mask_query_pending = false;
  double last_gpu_mask_ms = 0.0;
  uint64_t mask_gen_frames = 0;  // RGBA16F utk statistik guided
  int out_w = 0, out_h = 0;
  int mask_w = 0, mask_h = 0;
  int cpu_mask_w = 0, cpu_mask_h = 0;
  int face_prob_w = 0, face_prob_h = 0;
  int target_w = 0, target_h = 0;
  int raw_w = 0, raw_h = 0, beauty_w = 0, beauty_h = 0;
  int stat_w = 0, stat_h = 0, tmp_w = 0, tmp_h = 0;
  double cpu_mask_submit_ms = 0.0;
  bool ready = false;         // tex_out pernah terisi
  bool init_failed = false;   // shader/FBO gagal - pakai jalur lama
  bool has_stats = true;      // RGBA16F bisa dirender (guided aktif)
  bool disabled = false;      // M2i: dimatikan (flag --no-mask-gl / swa-uji)
  bool warned_no_stats = false;
  std::string last_error;

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
    // Encode: RG = (Y, Y^2) dari tekstur beauty (0..1).
    static const char* kFsEncode = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_beauty;
void main() {
  vec3 c = texture(u_beauty, v_uv).rgb;
  float y = dot(c, vec3(0.299, 0.587, 0.114));
  frag = vec4(y, y * y, 0.0, 1.0);
})";
    // Box blur 1D (H atau V lewat u_dir), radius integer.
    static const char* kFsBlur = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_in;
uniform vec2 u_dir;      // (1/w,0) atau (0,1/h)
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
    // Final: guided smoothing luma + mix mask + retensi chroma (M2i).
    static const char* kFsFinal = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_raw;
uniform sampler2D u_beauty;
uniform sampler2D u_mask;
uniform sampler2D u_stat;   // (meanY, meanY2) - RGBA16F
uniform float u_gain;
uniform float u_smooth_amount;
uniform float u_chroma_retain;
uniform float u_smooth_eps;
uniform float u_lift;      // M2j: lift Cerahan dalam mask (0 = mati)
uniform int u_has_stats;
uniform int u_debug;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 ycbcr(float r, float g, float b) {   // skala level 0..255, Cb/Cr basis 128
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
  // Hard zero the same tiny feather band reported as "outside" by F5.
  // This prevents lift/guided rounding from changing pixels where mask*gain
  // is below 8/255; raw passthrough is one source of truth for leak checks.
  if (m < (8.0 / 255.0)) { frag = texture(u_raw, v_uv); return; }

  // 1) guided smoothing luma pada beauty (M2i 3b)
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

  // 2) mix dgn raw
  vec3 mixed255 = mix(raw255, cand255, m);
  // 2b) M2j: lift Cerahan proporsional (identik ComposeBeautyRaw CPU)
  if (u_lift > 0.0) mixed255 += (u_lift * m) * (255.0 - mixed255);
  // 3) retensi chroma MENTAH (M2i 3a) - Y tetap dari hasil mix
  vec3 cm = ycbcr(mixed255.r, mixed255.g, mixed255.b);
  vec3 crw = ycbcr(raw255.r, raw255.g, raw255.b);
  float cb = mix(cm.g, crw.g, u_chroma_retain);
  float cr = mix(cm.b, crw.b, u_chroma_retain);
  vec3 out255 = to_rgb(cm.r, cb, cr);

  if (u_debug == 1) {
    vec3 red = vec3(255.0, 30.0, 30.0);
    out255 = mix(out255, red, m * 0.55);
  }
  frag = vec4(out255 / 255.0, 1.0);
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
uniform sampler2D u_raw;
uniform int u_has_seg;
uniform int u_has_color_gate;
uniform vec2 u_skin_cbcr;
uniform vec4 u_gate_limits; // cb min/max, cr min/max
uniform vec2 u_gate_radii;  // fade start/end in chroma units
void main() {
  float mesh_coverage = texture(u_face, v_uv).r;
  float mesh = mesh_coverage;
  if (u_has_seg == 1) mesh *= smoothstep(0.30, 0.50, texture(u_face_prob, v_uv).r);
  float body = texture(u_cpu, v_uv).r;
  if (mesh_coverage >= 0.5) body = 0.0;  // face mesh/p3 is authoritative inside face
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
  v *= 1.0 - clamp(texture(u_excl, v_uv).r, 0.0, 1.0);
  if (v < 0.02) v = 0.0;
  frag = vec4(v, 0.0, 0.0, 1.0);
}
)";
    prog_face = Build(GL_FRAGMENT_SHADER, kFsMaskWhite, "mask_gen.face");
    prog_excl = Build(GL_FRAGMENT_SHADER, kFsMaskWhite, "mask_gen.exclude");
    prog_excl_blur = Build(GL_FRAGMENT_SHADER, kFsExclBlur, "mask_gen.exclude_blur");
    prog_compose_mask = Build(GL_FRAGMENT_SHADER, kFsComposeMask, "mask_gen.compose");
    if (!prog_face || !prog_excl || !prog_excl_blur || !prog_compose_mask) {
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
    // Link vertex+fragment utk tiap program (vs dipakai bersama).
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs, "mask_blender.vs");
    if (!vs) {
      init_failed = true;
      return false;
    }
    if (!Link(prog_encode, vs) || !Link(prog_blur, vs) || !Link(prog_final, vs)) {
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
      kgcore::Log("mask_gen", "timer query GPU tidak tersedia/terekspos; hanya CPU submit timing yang dilaporkan");
    }
    glGenTextures(1, &tex_raw);
    glGenTextures(1, &tex_beauty);
    glGenTextures(1, &tex_mask);
    glGenTextures(1, &tex_cpu_mask);
    glGenTextures(1, &tex_face);
    glGenTextures(1, &tex_face_prob);
    glGenTextures(1, &tex_excl);
    glGenTextures(1, &tex_excl_blur);
    glGenTextures(1, &tex_out);
    glGenTextures(1, &tex_stat);
    glGenTextures(1, &tex_tmp);
    // M2i ROOT FIX: parameter sampling eksplisit - termasuk tex_out
    // (default NEAREST_MIPMAP_LINEAR tanpa mipmap = INCOMPLETE = HITAM
    // tanpa error GL; itulah penyebab preview 0x0/gelap di M2h).
    SetLinear(tex_out);
    SetLinear(tex_cpu_mask);
    SetLinear(tex_face);
    SetLinear(tex_face_prob);
    SetLinear(tex_excl);
    SetLinear(tex_excl_blur);
    SetLinear(tex_stat);
    SetLinear(tex_tmp);
    kgcore::Log("blend",
                "init OK: prog(final/encode/blur)=%u/%u/%u vao=%u fbo=%u "
                "tex out=%u stat=%u tmp=%u raw/beauty/mask=%u/%u/%u",
                prog_final, prog_encode, prog_blur, vao, fbo, tex_out, tex_stat,
                tex_tmp, tex_raw, tex_beauty, tex_mask);
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
    if (vao) glDeleteVertexArrays(1, &vao);
    if (mesh_vao) glDeleteVertexArrays(1, &mesh_vao);
    if (mesh_vbo) glDeleteBuffers(1, &mesh_vbo);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (mask_timer_query) glDeleteQueries(1, &mask_timer_query);
    const GLuint texes[] = {tex_raw, tex_beauty, tex_mask, tex_cpu_mask,
                            tex_face, tex_face_prob, tex_excl, tex_excl_blur,
                            tex_out, tex_stat, tex_tmp};
    glDeleteTextures(11, texes);
    prog_final = prog_encode = prog_blur = prog_face = prog_excl =
        prog_excl_blur = prog_compose_mask = 0;
    vao = mesh_vao = mesh_vbo = fbo = 0;
    tex_raw = tex_beauty = tex_mask = tex_cpu_mask = tex_face = tex_face_prob =
        tex_excl = tex_excl_blur = tex_out = tex_stat = tex_tmp = 0;
    out_w = out_h = mask_w = mask_h = 0;
    cpu_mask_w = cpu_mask_h = face_prob_w = face_prob_h = 0;
    target_w = target_h = raw_w = raw_h = beauty_w = beauty_h = 0;
    stat_w = stat_h = tmp_w = tmp_h = 0;
    ready = false;
    mask_timer_query = 0;
    mask_query_pending = false;
    last_gpu_mask_ms = 0.0;
    mask_gen_frames = 0;
  }

  // Satu frame: upload -> pass statistik -> pass final (3-4 draw call).
  // F5 capture: read exactly the textures consumed by the final blend pass.
  // glGetTexImage rows retain upload order (top-down CPU input is y=0 in tex).
  bool ReadbackOutput(std::vector<uint8_t>* rgba) const {
    if (!rgba || !ready || !tex_out || out_w <= 0 || out_h <= 0) return false;
    rgba->resize(static_cast<size_t>(out_w) * out_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
    return glGetError() == GL_NO_ERROR;
  }
  bool ReadbackMask(std::vector<uint8_t>* mask) const {
    if (!mask || !tex_mask || mask_w <= 0 || mask_h <= 0) return false;
    mask->resize(static_cast<size_t>(mask_w) * mask_h);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, mask->data());
    return glGetError() == GL_NO_ERROR;
  }

  bool Blend(const kgcore::BeautyComposeConfig& cfg, const uint8_t* raw, int w,
             int h, const uint8_t* beauty, const uint8_t* mask, int mw, int mh,
             float gain, bool debug_mask, const float* landmarks_xy = nullptr,
             int landmark_count = 0,
             const uint8_t* face_probability = nullptr,
             bool has_seg_probability = false, float skin_cb0 = 128.0f,
             float skin_cr0 = 128.0f, bool has_color_calibration = false,
             const kgcore::SkinMaskConfig* skin_cfg = nullptr) {
    if (disabled) return false;
    if (!Init() || !raw || !beauty || !mask || w <= 0 || h <= 0 || mw <= 0 ||
        mh <= 0) {
      return false;
    }
    const auto cpu_submit_t0 = std::chrono::steady_clock::now();
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {}  // bersihkan sisa tahap lalu
    UploadRGBA(tex_raw, raw, w, h);
    UploadRGBA(tex_beauty, beauty, w, h);
    UploadCpuMask(mask, mw, mh);
    if (landmarks_xy && landmark_count >= 468) {
      GenerateMaskGpu(landmarks_xy, landmark_count, w, h, mw, mh,
                      face_probability, has_seg_probability, skin_cb0, skin_cr0,
                      has_color_calibration, skin_cfg);
      cpu_mask_submit_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - cpu_submit_t0)
                               .count();
    } else {
      // Selalu sinkronkan mask final bila landmark tidak valid.
      UploadMask(mask, mw, mh);
    }
    glBindTexture(GL_TEXTURE_2D, tex_out);
    if (out_w != w || out_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      SetLinear(tex_out);
      out_w = w;
      out_h = h;
    }
    glBindVertexArray(vao);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);

    // ---- pass statistik guided (RGBA16F): encode + box H + box V ----
    if (has_stats && cfg.smooth_amount > 0.0f) {
      Attach16F(tex_stat, stat_w, stat_h);
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
        // box H: tex_stat -> tex_tmp
        Attach16F(tex_tmp, tmp_w, tmp_h);
        glUseProgram(prog_blur);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_stat);
        glUniform1i(glGetUniformLocation(prog_blur, "u_in"), 0);
        glUniform2f(glGetUniformLocation(prog_blur, "u_dir"),
                    1.0f / static_cast<float>(w), 0.0f);
        glUniform1i(glGetUniformLocation(prog_blur, "u_radius"),
                    std::max(1, cfg.smooth_radius));
        Draw();
        // box V: tex_tmp -> tex_stat
        Attach16F(tex_stat, stat_w, stat_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_tmp);
        glUniform2f(glGetUniformLocation(prog_blur, "u_dir"), 0.0f,
                    1.0f / static_cast<float>(h));
        Draw();
        if (LogGl("stat pass") != GL_NO_ERROR) {
          has_stats = false;  // gagal di tengah - pakai jalur tanpa guided
        }
      }
    }

    // ---- pass final -> tex_out ----
    AttachRGBA(tex_out);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glBindVertexArray(0);
      last_error = "FBO tex_out tidak lengkap";
      kgcore::Log("blend", "GAGAL: FBO tex_out tidak lengkap (fbo=%u %dx%d)",
                  fbo, w, h);
      init_failed = true;
      return false;
    }
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
                gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain));
    glUniform1f(glGetUniformLocation(prog_final, "u_smooth_amount"),
                cfg.smooth_amount);
    glUniform1f(glGetUniformLocation(prog_final, "u_chroma_retain"),
                cfg.chroma_retain);
    glUniform1f(glGetUniformLocation(prog_final, "u_smooth_eps"), cfg.smooth_eps);
    glUniform1f(glGetUniformLocation(prog_final, "u_lift"),
                cfg.lift < 0.0f ? 0.0f : (cfg.lift > 0.95f ? 0.95f : cfg.lift));
    glUniform1i(glGetUniformLocation(prog_final, "u_has_stats"),
                (has_stats && cfg.smooth_amount > 0.0f) ? 1 : 0);
    glUniform1i(glGetUniformLocation(prog_final, "u_debug"), debug_mask ? 1 : 0);
    Draw();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    const GLenum err = LogGl("final pass");
    if (err != GL_NO_ERROR) {
      init_failed = true;
      return false;
    }
    ready = true;
    return true;
  }

  // M2i swa-uji: baca kembali luma rata-rata keluaran (subsample 1/4).
  bool ReadOutputLuma(float* out_y) {
    if (!ready || out_w <= 0 || out_h <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(out_w) * out_h * 4);
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
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
  static void SetLinear(GLuint tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  void UploadRGBA(GLuint tex, const uint8_t* data, int w, int h) {
    int& tex_w = tex == tex_raw ? raw_w : beauty_w;
    int& tex_h = tex == tex_raw ? raw_h : beauty_h;
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
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                 nullptr);
    SetLinear(tex);
  }
  void AttachMask(GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    glViewport(0, 0, w, h);
  }
  void GenerateMaskGpu(const float* xy_px, int count, int fw, int fh, int mw,
                       int mh, const uint8_t* face_probability,
                       bool has_seg_probability, float skin_cb0,
                       float skin_cr0, bool has_color_calibration,
                       const kgcore::SkinMaskConfig* skin_cfg) {
    if (!xy_px || count < 468 || fw <= 0 || fh <= 0) return;
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
    // 1) FACEMESH_TESSELATION triangles — rasterisasi GPU setiap frame.
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
    AttachMask(tex_face, mw, mh);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog_face);
    glBindVertexArray(mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 2));

    // 2) Exclusion fans: mata, alis (dua rantai digabung), bibir luar, lubang
    // hidung kecil. GL_MAX menyatukan semua kontur tanpa ring tebal.
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
    // Ear clipping menutup poligon cekung (terutama bibir luar) tanpa
    // centroid fan yang dapat mengisi area di luar kontur.
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
      for (int i = 0; i < kSteps; ++i) {
        const float a0 = 6.28318530718f * i / kSteps;
        const float a1 = 6.28318530718f * (i + 1) / kSteps;
        add_triangle(e.cx, e.cy, e.cx + e.rx * std::cos(a0),
                     e.cy + e.ry * std::sin(a0),
                     e.cx + e.rx * std::cos(a1),
                     e.cy + e.ry * std::sin(a1));
      }
    }
    AttachMask(tex_excl, mw, mh);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendEquation(GL_MAX); glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(prog_excl);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(exverts.size() * sizeof(float)),
                 exverts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(exverts.size() / 2));
    glDisable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);

    // 3) Feather eksklusi di resolusi mask (Gaussian 5x5), lalu 4) union CPU
    // mask + teselasi → (1−excl) → ambang keras. Mask final inilah yang dipakai
    // shader blend DAN F5 readback (single source of truth).
    AttachMask(tex_excl_blur, mw, mh);
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
    AttachMask(tex_mask, mw, mh);
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
    Draw();
    glActiveTexture(GL_TEXTURE0);
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
  }
  void AttachRGBA(GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
  }
  void Attach16F(GLuint tex, int& tex_w, int& tex_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (tex_w != out_w || tex_h != out_h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, out_w, out_h, 0, GL_RGBA,
                   GL_FLOAT, nullptr);
      tex_w = out_w;
      tex_h = out_h;
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
    // Buat program sementara utk menampung hasil compile awal; link menyusul
    // dengan vertex shader - cukup simpan shader via program sederhana:
    GLuint sh = Compile(type, src, label);
    if (!sh) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, sh);
    glDeleteShader(sh);
    // Link awal (hanya fs) akan gagal utk vs wajib? Tidak - fs-only program
    // valid; Link() berikutnya menambah vs dan link ulang.
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
