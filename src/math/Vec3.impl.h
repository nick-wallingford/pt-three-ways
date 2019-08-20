#pragma once

#include "Norm3.impl.h"

inline Norm3 Vec3::normalised() const noexcept {
  return Norm3(*this * (1.0 / length()));
}

inline constexpr Vec3::Vec3(const Norm3 &norm)
    : x_(norm.x()), y_(norm.y()), z_(norm.z()) {}
