/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The Android Open Source Project
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <th1520_g2d/th1520_g2d.h>

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {

struct G2dBlitGeometry {
  th1520_g2d_image image{};
  th1520_g2d_rect source{};
  th1520_g2d_rect destination{};
  bool source_over = false;
};

// Deliberately conservative. Unsupported effects stay with the existing
// compositor; never reinterpret a private gralloc handle or a tiled allocation.
inline std::optional<G2dBlitGeometry> GetG2dGeometry(const LayerData &layer,
                                                   uint32_t width,
                                                   uint32_t height) {
  constexpr uint64_t kProtectedUsage = 0x4000;  // GRALLOC_USAGE_PROTECTED
  if (!layer.bi || !width || !height || width > 16384 || height > 16384)
    return std::nullopt;
  const auto &bi = *layer.bi;
  if (!bi.usage || (*bi.usage & kProtectedUsage) != 0 ||
      bi.prime_fds[0] < 0 || bi.modifiers[0] != 0 ||
      bi.pitches[1] != 0 || bi.sizes[1] != 0 ||
      bi.width == 0 || bi.height == 0 || bi.width > 16384 || bi.height > 16384 ||
      bi.pitches[0] < uint64_t(bi.width) * 4 || (bi.offsets[0] & 3U) != 0 ||
      layer.pi.alpha != 1.0F || layer.pi.transform.hflip ||
      layer.pi.transform.vflip || layer.pi.transform.rotate90 ||
      (layer.colorspace != Colorspace::kDefault &&
       !(layer.colorspace == Colorspace::kBt709Ycc &&
         layer.transfer_func == TransferFunction::kSrgb)) ||
      (layer.transfer_func != TransferFunction::kUnknown &&
       layer.transfer_func != TransferFunction::kSrgb))
    return std::nullopt;
  switch (bi.format) {
    case TH1520_G2D_FORMAT_XRGB8888:
    case TH1520_G2D_FORMAT_ARGB8888:
    case TH1520_G2D_FORMAT_XBGR8888:
    case TH1520_G2D_FORMAT_ABGR8888:
      break;
    default:
      return std::nullopt;
  }
  if (bi.blend_mode != BufferBlendMode::kNone &&
      bi.blend_mode != BufferBlendMode::kPreMult)
    return std::nullopt;

  const FRect src = layer.pi.source_crop.f_rect.value_or(
      FRect{0, 0, float(bi.width), float(bi.height)});
  for (float value : {src.left, src.top, src.right, src.bottom}) {
    if (!std::isfinite(value) || value < 0 || value > 16384 ||
        std::floor(value) != value)
      return std::nullopt;
  }
  if (src.right <= src.left || src.bottom <= src.top ||
      src.right > bi.width || src.bottom > bi.height)
    return std::nullopt;
  const auto dst = layer.pi.display_frame.i_rect.value_or(
      IRect{0, 0, int32_t(width), int32_t(height)});
  // Widen BEFORE subtraction: incoming rectangles can contain INT_MIN/MAX.
  if (int64_t(dst.right) - dst.left != int64_t(src.Width()) ||
      int64_t(dst.bottom) - dst.top != int64_t(src.Height()))
    return std::nullopt;
  const int64_t left = std::max<int64_t>(0, dst.left);
  const int64_t top = std::max<int64_t>(0, dst.top);
  const int64_t right = std::min<int64_t>(width, dst.right);
  const int64_t bottom = std::min<int64_t>(height, dst.bottom);
  if (right <= left || bottom <= top)
    return std::nullopt;
  return G2dBlitGeometry{
      .image = {.width = bi.width,
                .height = bi.height,
                .format = bi.format,
                .num_planes = 1,
                .modifier = 0,
                .planes = {{bi.prime_fds[0], bi.offsets[0], bi.pitches[0]}}},
      .source = {uint32_t(int64_t(src.left) + left - dst.left),
                 uint32_t(int64_t(src.top) + top - dst.top),
                 uint32_t(right - left), uint32_t(bottom - top)},
      .destination = {uint32_t(left), uint32_t(top), uint32_t(right - left),
                      uint32_t(bottom - top)},
      .source_over = bi.blend_mode == BufferBlendMode::kPreMult,
  };
}

}  // namespace android::drm_hwcomposer
