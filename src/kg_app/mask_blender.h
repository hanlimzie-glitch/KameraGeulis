#pragma once
// M2h - tahap blending mask kulit di pipeline aplikasi (GL 3.3 sendiri,
// TANPA menyentuh source gpupixel). Rantai tetap:
//   raw -> gpupixel (BeautyFace whole-frame) -> sink
// lalu di sini: out = mix(raw, beauty, mask * gain) lewat shader GPU.
// Mask = tekstur R8 resolusi rendah (bilinear), gain = fade saat wajah
// hilang (LandmarkFade), debug_mask = overlay merah F6.
//
// Jujur: komponen ini hanya bisa diverifikasi di Windows nyata (GL/GPU);
// unit test Linux hanya menutupi matematika mask/komposit (kgcore).
#include <glad/glad.h>

#include <cstdint>
#include <string>
#include <vector>

struct MaskBlender {
  GLuint prog = 0;
  GLuint vao = 0;
  GLuint fbo = 0;
  GLuint tex_raw = 0, tex_beauty = 0, tex_mask = 0, tex_out = 0;
  int out_w = 0, out_h = 0;    // dimensi blend (== frame)
  int mask_w = 0, mask_h = 0;  // dimensi tekstur mask
  bool ready = false;          // tex_out pernah berisi hasil blend
  bool init_failed = false;    // shader gagal - pakai jalur lama
  std::string last_error;

  // Wajib dipanggil dengan konteks GL aktif (sama seperti PreviewTexture).
  bool Init() {
    if (prog || init_failed) return prog != 0;
    static const char* kVs = R"(#version 330 core
out vec2 v_uv;
void main() {
  const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  vec2 pos = p[gl_VertexID];
  v_uv = pos * 0.5 + 0.5;
  gl_Position = vec4(pos, 0.0, 1.0);
})";
    static const char* kFs = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_raw;
uniform sampler2D u_beauty;
uniform sampler2D u_mask;
uniform float u_gain;
uniform int u_debug;
void main() {
  float m = clamp(texture(u_mask, v_uv).r * u_gain, 0.0, 1.0);
  vec3 c = mix(texture(u_raw, v_uv).rgb, texture(u_beauty, v_uv).rgb, m);
  if (u_debug == 1) c = mix(c, vec3(1.0, 0.12, 0.12), m * 0.55);
  frag = vec4(c, 1.0);
})";
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs, "mask_blender.vs");
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFs, "mask_blender.fs");
    if (!vs || !fs) {
      if (vs) glDeleteShader(vs);
      if (fs) glDeleteShader(fs);
      init_failed = true;
      return false;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
      char logbuf[1024] = {};
      glGetProgramInfoLog(prog, sizeof(logbuf), nullptr, logbuf);
      last_error = logbuf;
      glDeleteProgram(prog);
      prog = 0;
      init_failed = true;
      return false;
    }
    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex_raw);
    glGenTextures(1, &tex_beauty);
    glGenTextures(1, &tex_mask);
    glGenTextures(1, &tex_out);
    return true;
  }

  void Destroy() {
    if (prog) glDeleteProgram(prog);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    const GLuint texes[] = {tex_raw, tex_beauty, tex_mask, tex_out};
    glDeleteTextures(4, texes);
    prog = vao = fbo = tex_raw = tex_beauty = tex_mask = tex_out = 0;
    out_w = out_h = mask_w = mask_h = 0;
    ready = false;
  }

  // Satu blend per frame: upload 3 input + render pass ke tex_out.
  bool Blend(const uint8_t* raw, int w, int h, const uint8_t* beauty,
             const uint8_t* mask, int mw, int mh, float gain, bool debug_mask) {
    if (!Init() || !raw || !beauty || !mask || w <= 0 || h <= 0 || mw <= 0 ||
        mh <= 0) {
      return false;
    }
    UploadRGBA(tex_raw, raw, w, h);
    UploadRGBA(tex_beauty, beauty, w, h);
    UploadMask(mask, mw, mh);
    // FBO keluaran.
    glBindTexture(GL_TEXTURE_2D, tex_out);
    if (out_w != w || out_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      out_w = w;
      out_h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex_out, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      if (!init_failed) {
        last_error = "FBO mask_blender tidak lengkap";
      }
      init_failed = true;
      return false;
    }
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(prog);
    glUniform1i(Loc("u_raw"), 0);
    glUniform1i(Loc("u_beauty"), 1);
    glUniform1i(Loc("u_mask"), 2);
    glUniform1f(Loc("u_gain"), gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain));
    glUniform1i(Loc("u_debug"), debug_mask ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_raw);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_beauty);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ready = true;
    return true;
  }

 private:
  GLint Loc(const char* n) const { return glGetUniformLocation(prog, n); }

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
      glDeleteShader(sh);
      return 0;
    }
    return sh;
  }

  static void UploadRGBA(GLuint tex, const uint8_t* data, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Selalu glTexImage (Ukuran bisa berubah tiap mode kamera) - data RGBA
    // top-down, tanpa mirror (mirror ditangani UV saat digambar).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, data);
  }

  void UploadMask(const uint8_t* data, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex_mask);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mask_w != w || mask_h != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                   nullptr);  // alokasi ukuran (data menyusul via SubImage)
      mask_w = w;
      mask_h = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    data);
  }
};
