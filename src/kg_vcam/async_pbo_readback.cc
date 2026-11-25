#include "async_pbo_readback.h"

#include <chrono>
#include <cstring>

#include "kgcore/log.h"
#include "vcam_service.h"

namespace kgcore {
namespace {
using clock_type = std::chrono::steady_clock;
}  // namespace

AsyncPboReadback::~AsyncPboReadback() { Destroy(); }

bool AsyncPboReadback::Init() {
  if (initialized_) return true;
  glGenFramebuffers(1, &fbo_);
  glGenBuffers(2, pbo_);
  if (fbo_ == 0 || pbo_[0] == 0 || pbo_[1] == 0) {
    Log("vcam", "AsyncPboReadback gagal membuat FBO/PBO (glErr=0x%X)",
        static_cast<unsigned>(glGetError()));
    Destroy();
    return false;
  }
  initialized_ = true;
  Log("vcam", "AsyncPboReadback siap (double-buffer PBO: %u, %u)", pbo_[0],
      pbo_[1]);
  return true;
}

void AsyncPboReadback::Destroy() {
  for (int i = 0; i < 2; ++i) {
    if (fence_[i]) {
      glDeleteSync(fence_[i]);
      fence_[i] = nullptr;
    }
    pbo_w_[i] = pbo_h_[i] = 0;
    pbo_ts_[i] = 0;
  }
  if (pbo_[0] || pbo_[1]) {
    glDeleteBuffers(2, pbo_);
    pbo_[0] = pbo_[1] = 0;
  }
  if (fbo_) {
    glDeleteFramebuffers(1, &fbo_);
    fbo_ = 0;
  }
  slot_idx_ = 0;
  initialized_ = false;
}

void AsyncPboReadback::CaptureAndSubmit(GLuint src_tex, int width, int height,
                                        int64_t capture_ts_us,
                                        VcamService* service) {
  if (!service || !service->IsEnabled() || src_tex == 0 || width <= 0 ||
      height <= 0) {
    return;
  }
  if (!initialized_ && !Init()) return;

  const auto t0 = clock_type::now();
  const int cur = slot_idx_;

  // 1) Panen slot `cur` secara non-blocking (timeout=0) bila ada transfer sebelumnya.
  if (fence_[cur]) {
    const GLenum wait = glClientWaitSync(fence_[cur], 0, 0);
    if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
      const GLsizeiptr bytes =
          static_cast<GLsizeiptr>(pbo_w_[cur]) * pbo_h_[cur] * 4;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[cur]);
      const void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes,
                                         GL_MAP_READ_BIT);
      if (ptr) {
        const float map_ms =
            std::chrono::duration<float, std::milli>(clock_type::now() - t0)
                .count();
        service->SubmitFrame(static_cast<const uint8_t*>(ptr), pbo_w_[cur],
                             pbo_h_[cur], pbo_ts_[cur], map_ms);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      glDeleteSync(fence_[cur]);
      fence_[cur] = nullptr;
    } else {
      // GPU belum selesai DMA -> jangan pernah memblokir render thread.
      service->NotePboBusySkip();
      slot_idx_ = 1 - slot_idx_;
      return;
    }
  }

  // 2) Jadwalkan DMA asinkron untuk frame saat ini ke PBO `cur`.
  const GLsizeiptr bytes = static_cast<GLsizeiptr>(width) * height * 4;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, src_tex, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[cur]);
  if (pbo_w_[cur] != width || pbo_h_[cur] != height) {
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    pbo_w_[cur] = width;
    pbo_h_[cur] = height;
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  fence_[cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  pbo_ts_[cur] = capture_ts_us;

  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

  slot_idx_ = 1 - slot_idx_;
}

}  // namespace kgcore
