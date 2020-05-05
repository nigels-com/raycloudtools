// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#ifndef RAYPROGRESS_H
#define RAYPROGRESS_H

#include "raylib/raylibconfig.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

namespace ray
{
/// A utility class used to track progress of various algorithms.
///
/// The structure tracks the following values:
/// - @c phase() : a contextual name identifying the current operation. May be empty.
/// - @c target() : the target value for the current phase. May be zero when the target is unknown.
/// - @c progress() : the progress value.
///
/// The @c progress() value is either in the range `[0, target()]` when @c target() is known or has an unknown
/// range. When @c target() is known, the progress may be reported as a ratio `[0, 1]`.
///
/// Updating the progress value is threadsafe, however, the @c reset() operations are not.
class RAYLIB_EXPORT Progress
{
public:
  /// Initialise a new progress tracker with the given @p target.
  Progress(size_t target = 0);
  /// Initialise a new progress tracker with the given @p phase name and @p target.
  Progress(const std::string &phase, size_t target = 0);

  /// Protected read into @p other.
  void read(Progress *other) const
  {
    std::unique_lock<std::mutex> guard1(reset_mutex_);
    std::unique_lock<std::mutex> guard2(other->reset_mutex_);
    other->phase_ = phase_;
    guard1.unlock();
    other->progress_ = progress_.load();
    other->target_ = target_.load();
  }

  /// Query the current phase name (may be empty).
  inline std::string phase() const
  {
    std::unique_lock<std::mutex> guard(reset_mutex_);
    return phase_;
  }

  /// Query the current target value. This may be zero when the target value is unknown.
  inline size_t target() const { return target_; }

  /// Query the current prorgess value.
  inline size_t progress() const { return progress_; }

  /// Initialise a the progress tracker to the given @p target.
  void reset(size_t target = 0);
  /// Initialise a the progress tracker to the given @p phase name and @p target.
  void reset(const std::string &phase, size_t target = 0);

  /// Query the current progress as a ratio `[0, 1]`.
  ///
  /// Note: when the @c target() is unknown, this function simply reports the current @c progress() value.
  double progressRatio();

  /// Directly set the current @c progress() @p value.
  void setProgress(size_t value);

  /// Increment the @c progress() by one step. To be called from the code performing work.
  void increment();
  /// Increment the @c progress() by @p step. To be called from the code performing work.
  void increment(size_t step);

private:
  mutable std::mutex reset_mutex_;
  std::string phase_;
  std::atomic_size_t target_;
  std::atomic_size_t progress_;
};


inline Progress::Progress(size_t target)
  : target_(target)
  , progress_(0u)
{}


inline Progress::Progress(const std::string &phase, size_t target)
  : phase_(phase)
  , target_(target)
  , progress_(0u)
{}


inline void Progress::reset(size_t target)
{
  reset(std::string(), target);
}


inline void Progress::reset(const std::string &phase, size_t target)
{
  std::unique_lock<std::mutex> guard(reset_mutex_);
  phase_ = phase;
  target_ = target;
  progress_ = 0u;
}


inline double Progress::progressRatio()
{
  const size_t target = target_;
  const size_t progress = progress_;
  if (target > 0)
  {
    return double(progress) / double(target);
  }

  return double(progress);
}

inline void Progress::setProgress(size_t value)
{
  progress_ = value;
}

inline void Progress::increment()
{
  ++progress_;
}


inline void Progress::increment(size_t step)
{
  progress_ += step;
}
}  // namespace ray

#endif  // RAYPROGRESS_H
