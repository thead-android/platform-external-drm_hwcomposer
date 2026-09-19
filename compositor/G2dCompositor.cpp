/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The Android Open Source Project
 */
#include "G2dCompositor.h"

#include <poll.h>
#include <sync/sync.h>

#include "compositor/G2dGeometry.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcLayer.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {
namespace {
bool FenceDone(const SharedFd &fence) {
  if (!fence)
    return true;
  pollfd pfd{.fd = *fence, .events = POLLIN, .revents = 0};
  return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0 &&
         (pfd.revents & (POLLERR | POLLNVAL)) == 0;
}
}  // namespace

std::unique_ptr<G2dCompositor> G2dCompositor::Create() {
  auto *context = th1520_g2d_open("/dev/dri/renderD129");
  if (!context) {
    ALOGW("GC620 compositor unavailable");
    return nullptr;
  }
  return std::unique_ptr<G2dCompositor>(new G2dCompositor(context));
}

G2dCompositor::~G2dCompositor() {
  th1520_g2d_close(context_);
}

std::shared_ptr<G2dFrame> G2dCompositor::Prepare(
    DrmDisplayPipeline &pipe, const std::vector<const HwcLayer *> &layers,
    uint32_t width, uint32_t height) {
  if (layers.size() < 2 || layers.size() > 16)
    return nullptr;
  for (const auto *layer : layers) {
    // Respect SF-forced client composition (blur, color, follower pacing etc.).
    if (layer->GetSfType() != CompositionType::kDevice &&
        layer->GetSfType() != CompositionType::kCursor)
      return nullptr;
  }
  size_t first = 0;
  for (size_t i = 0; i < layers.size(); ++i) {
    auto geometry = GetG2dGeometry(layers[i]->GetLayerData(), width, height);
    if (geometry && !geometry->source_over && geometry->destination.x == 0 &&
        geometry->destination.y == 0 && geometry->destination.width == width &&
        geometry->destination.height == height)
      first = i;  // Everything below this opaque, full-screen layer is hidden.
  }
  if (layers.size() - first < 2)
    return nullptr;  // Leave single-buffer scanout to the ordinary planner.
  for (size_t i = first; i < layers.size(); ++i) {
    if (!GetG2dGeometry(layers[i]->GetLayerData(), width, height))
      return nullptr;
  }
  // Old mode buffers remain alive through the active target and DRM FB refs.
  targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                               [&](const auto &target) {
                                 return target->layer.bi->width != width ||
                                        target->layer.bi->height != height;
                               }), targets_.end());
  std::shared_ptr<G2dTarget> target;
  for (const auto &candidate : targets_) {
    if (candidate != active_target_ && candidate.use_count() == 1 &&
        FenceDone(candidate->busy_until)) {
      target = candidate;
      break;
    }
  }
  if (!target && targets_.size() < 3) {
    auto bi = pipe.device->CreateBufferForModeset(width, height);
    if (!bi)
      return nullptr;
    auto fb = pipe.importer->GetOrCreateFbId(&*bi);
    if (!fb)
      return nullptr;
    target = std::make_shared<G2dTarget>();
    target->layer.bi = std::move(bi);
    target->layer.fb = std::move(fb);
    target->layer.pi.alpha = 1.0F;
    target->layer.pi.source_crop.f_rect = FRect{0, 0, float(width), float(height)};
    target->layer.pi.display_frame.i_rect = IRect{0, 0, int32_t(width), int32_t(height)};
    target->layer.colorspace = Colorspace::kDefault;
    target->layer.transfer_func = TransferFunction::kSrgb;
    targets_.push_back(target);
  }
  if (!target)
    return nullptr;  // Never overwrite a buffer still being scanned out.
  auto frame = std::make_shared<G2dFrame>();
  frame->target = target;
  frame->occluded.assign(layers.begin(), layers.begin() + first);
  frame->layers.assign(layers.begin() + first, layers.end());
  return frame;
}

bool G2dCompositor::Render(const std::shared_ptr<G2dFrame> &frame) {
  auto &target = *frame->target;
  const auto &bi = *target.layer.bi;
  std::vector<G2dBlitGeometry> geometries;
  for (const auto *layer : frame->layers) {
    auto geometry = GetG2dGeometry(layer->GetLayerData(), bi.width, bi.height);
    if (!geometry)
      return false;  // Buffer/geometry changed after validation: request revalidate.
    geometries.push_back(*geometry);
  }
  const th1520_g2d_image dst{.width = bi.width,
                           .height = bi.height,
                           .format = bi.format,
                           .num_planes = 1,
                           .modifier = 0,
                           .planes = {{bi.prime_fds[0], bi.offsets[0], bi.pitches[0]}}};
  const th1520_g2d_rect full{0, 0, bi.width, bi.height};
  int fence = -1;
  const auto &first = geometries.front();
  const bool covered = !first.source_over && first.destination.x == 0 &&
                       first.destination.y == 0 &&
                       first.destination.width == bi.width &&
                       first.destination.height == bi.height;
  if (!covered) {
    if (th1520_g2d_clear(context_, &dst, &full, 0xff000000, -1, &fence) != 0)
      return false;
    target.busy_until = MakeSharedFd(fence);
  }
  for (size_t i = 0; i < frame->layers.size(); ++i) {
    const auto &acquire = frame->layers[i]->GetLayerData().acquire_fence;
    SharedFd input = target.busy_until;
    if (acquire && input) {
      int merged = sync_merge("gc620-input", *input, *acquire);
      if (merged < 0)
        return false;
      input = MakeSharedFd(merged);
    } else if (acquire) {
      input = acquire;
    }
    auto &g = geometries[i];
    auto operation = g.source_over ? th1520_g2d_blend : th1520_g2d_blit;
    if (operation(context_, &g.image, &g.source, &dst, &g.destination,
                  input ? *input : -1, &fence) != 0)
      return false;
    target.busy_until = MakeSharedFd(fence);
  }
  target.layer.acquire_fence = target.busy_until;
  return true;
}

void G2dCompositor::Presented(const std::shared_ptr<G2dFrame> &frame,
                            const SharedFd &present_fence) {
  // The NEXT flip releases the previously scanned target, not its own flip.
  if (active_target_) {
    // Without a release fence keep the old buffer quarantined rather than reuse it.
    if (present_fence)
      active_target_->busy_until = present_fence;
    else
      targets_.erase(std::remove(targets_.begin(), targets_.end(), active_target_),
                     targets_.end());
  }
  active_target_ = frame ? frame->target : nullptr;
}

}  // namespace android::drm_hwcomposer
