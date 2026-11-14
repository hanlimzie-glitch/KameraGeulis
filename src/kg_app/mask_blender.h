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

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "kgcore/log.h"
#include "kgcore/skin_mask.h"

struct MaskBlender {
  // state inti
  GLuint prog_final = 0, prog_encode = 0, prog_blur = 0;
  GLuint vao = 0, fbo = 0;
  GLuint tex_raw = 0, tex_beauty = 0, tex_mask = 0, tex_out = 0;
  GLuint tex_stat = 0, tex_tmp = 0;  // RGBA16F utk statistik guided
  int out_w = 0, out_h = 0;
  int mask_w = 0, mask_h = 0;
  bool ready = false;         // tex_out pernah terisi
  bool init_failed = false;   // shader/FBO gagal - pakai jalur lama
  bool has_stats = true;      // RGBA16F bisa dirender (guided aktif)
  bool disabled = false;      // M2i: dimatikan (flag --no-mask-gl / swa-uji)
  bool warned_no_stats = false;
  std::string last_error;

  bool Init() {
    if (prog_final || init_failed) return prog_final != 0;
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
  if (m <= 0.0) { frag = texture(u_raw, v_uv); return; }  // byte-exact raw

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
  if (u_lift > 0.0) mixed255 += u_lift * (255.0 - mixed255);
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
    glDeleteShader(vs);
    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex_raw);
    glGenTextures(1, &tex_beauty);
    glGenTextures(1, &tex_mask);
    glGenTextures(1, &tex_out);
    glGenTextures(1, &tex_stat);
    glGenTextures(1, &tex_tmp);
    // M2i ROOT FIX: parameter sampling eksplisit - termasuk tex_out
    // (default NEAREST_MIPMAP_LINEAR tanpa mipmap = INCOMPLETE = HITAM
    // tanpa error GL; itulah penyebab preview 0x0/gelap di M2h).
    SetLinear(tex_out);
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
    if (vao) glDeleteVertexArrays(1, &vao);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    const GLuint texes[] = {tex_raw, tex_beauty, tex_mask, tex_out, tex_stat,
                            tex_tmp};
    glDeleteTextures(6, texes);
    prog_final = prog_encode = prog_blur = 0;
    vao = fbo = 0;
    tex_raw = tex_beauty = tex_mask = tex_out = tex_stat = tex_tmp = 0;
    out_w = out_h = mask_w = mask_h = 0;
    ready = false;
  }

  // Satu frame: upload -> pass statistik -> pass final (3-4 draw call).
  bool Blend(const kgcore::BeautyComposeConfig& cfg, const uint8_t* raw, int w,
             int h, const uint8_t* beauty, const uint8_t* mask, int mw, int mh,
             float gain, bool debug_mask) {
    if (disabled) return false;
    if (!Init() || !raw || !beauty || !mask || w <= 0 || h <= 0 || mw <= 0 ||
        mh <= 0) {
      return false;
    }
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {}  // bersihkan sisa tahap lalu
    UploadRGBA(tex_raw, raw, w, h);
    UploadRGBA(tex_beauty, beauty, w, h);
    UploadMask(mask, mw, mh);
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
      Attach16F(tex_stat);
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
        Attach16F(tex_tmp);
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
        Attach16F(tex_stat);
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
  static void UploadRGBA(GLuint tex, const uint8_t* data, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 data);
    SetLinear(tex);  // idempoten - jaga-jaga bila status sempat hilang
  }
  void UploadMask(const uint8_t* data, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    if (mask_w != w || mask_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                   nullptr);
      mask_w = w;
      mask_h = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    data);
    SetLinear(tex_mask);
  }
  void AttachRGBA(GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
  }
  void Attach16F(GLuint tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    // Alokasi ulang tiap pass aman (databox kosong); format 16F.
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, out_w, out_h, 0, GL_RGBA,
                 GL_FLOAT, nullptr);
    SetLinear(tex);
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
