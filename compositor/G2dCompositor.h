/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The Android Open Source Project
 */
#pragma once

#include <memory>
#include <vector>

#include <th1520_g2d/th1520_g2d.h>

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {

class HwcLayer;
struct DrmDisplayPipeline;

struct G2dTarget {
  LayerData layer;
  SharedFd busy_until;
};

struct G2dFrame {
  std::shared_ptr<G2dTarget> target;
  std::vector<const HwcLayer *> layers;
  std::vector<const HwcLayer *> occluded;
};

class G2dCompositor {
 public:
  static std::unique_ptr<G2dCompositor> Create();
  ~G2dCompositor();
  std::shared_ptr<G2dFrame> Prepare(DrmDisplayPipeline &pipe,
                                   const std::vector<const HwcLayer *> &layers,
                                   uint32_t width, uint32_t height);
  bool Render(const std::shared_ptr<G2dFrame> &frame);
  void Presented(const std::shared_ptr<G2dFrame> &frame,
                 const SharedFd &present_fence);

 private:
  explicit G2dCompositor(th1520_g2d *context) : context_(context) {}
  th1520_g2d *context_;
  std::vector<std::shared_ptr<G2dTarget>> targets_;
  std::shared_ptr<G2dTarget> active_target_;
};

}  // namespace android::drm_hwcomposer
