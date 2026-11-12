#pragma once
#include <algorithm>
#include <cmath>

namespace kgcore {
struct Vec2 {
  float x = 0.0f, y = 0.0f;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
  static Vec2 Lerp(const Vec2& a, const Vec2& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  }
  float Length() const { return std::sqrt(x * x + y * y); }
};
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float Clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
}  // namespace kgcore
