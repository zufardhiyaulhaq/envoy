#pragma once

#include "envoy/filesystem/filesystem.h"
#include "envoy/thread/thread.h"

namespace Envoy {

class PlatformImpl {
public:
  PlatformImpl();
  virtual ~PlatformImpl();
  Thread::ThreadFactory& threadFactory() { return *thread_factory_; }
  Filesystem::Instance& fileSystem() { return *file_system_; }
  virtual bool enableCoreDump();

private:
  Thread::ThreadFactoryPtr thread_factory_;
  Filesystem::InstancePtr file_system_;
};

} // namespace Envoy
