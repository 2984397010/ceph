// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include "include/rados/librados.hpp"

#include "rgw_aio_throttle.h"
#include "rgw_rados.h"

namespace rgw {

bool AioThrottle::waiter_ready() const
{
  switch (waiter) {
  case Wait::Available: return is_available();
  case Wait::Completion: return has_completion();
  case Wait::Drained: return is_drained();
  default: return false;
  }
}

AioResultList AioThrottle::get(const RGWSI_RADOS::Obj& obj,
			       OpFunc&& f,
			       uint64_t cost, uint64_t id)
{
  auto p = std::make_unique<Pending>();
  p->obj = obj;
  p->id = id;
  p->cost = cost;

  std::unique_lock lock{mutex};
  if (cost > window) {
    p->result = -EDEADLK; // would never succeed
    completed.push_back(*p);
  } else {
    // wait for the write size to become available
    pending_size += p->cost;
    if (!is_available()) {
      ceph_assert(waiter == Wait::None);
      waiter = Wait::Available;
      cond.wait(lock, [this] { return is_available(); });
      waiter = Wait::None;
    }

    // register the pending write and attach a completion
    p->parent = this;
    pending.push_back(*p);
    lock.unlock();
    std::move(f)(this, *static_cast<AioResult*>(p.get()));
    lock.lock();
  }
  p.release();
  return std::move(completed);
}

void AioThrottle::put(AioResult& r)
{
  auto& p = static_cast<Pending&>(r);
  std::scoped_lock lock{mutex};

  // move from pending to completed
  pending.erase(pending.iterator_to(p));
  completed.push_back(p);

  pending_size -= p.cost;

  if (waiter_ready()) {
    cond.notify_one();
  }
}

AioResultList AioThrottle::poll()
{
  std::unique_lock lock{mutex};
  return std::move(completed);
}

AioResultList AioThrottle::wait()
{
  std::unique_lock lock{mutex};
  if (completed.empty() && !pending.empty()) {
    ceph_assert(waiter == Wait::None);
    waiter = Wait::Completion;
    cond.wait(lock, [this] { return has_completion(); });
    waiter = Wait::None;
  }
  return std::move(completed);
}

AioResultList AioThrottle::drain()
{
  std::unique_lock lock{mutex};
  if (!pending.empty()) {
    ceph_assert(waiter == Wait::None);
    waiter = Wait::Drained;
    cond.wait(lock, [this] { return is_drained(); });
    waiter = Wait::None;
  }
  return std::move(completed);
}

} // namespace rgw
