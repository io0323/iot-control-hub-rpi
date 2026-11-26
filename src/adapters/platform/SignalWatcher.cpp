#include "adapters/platform/SignalWatcher.h"

#include <csignal>

#include <pthread.h>

namespace gw::adapters {

namespace {

// stop() で待ちを解くための合図（sigwait の対象に含める）
constexpr int kWakeSignal = SIGUSR1;

sigset_t watched_set() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, kWakeSignal);
  return set;
}

}  // namespace

void SignalWatcher::block_signals() {
  const sigset_t set = watched_set();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

SignalWatcher::SignalWatcher(std::function<void()> on_stop, std::function<void()> on_hangup)
    : on_stop_(std::move(on_stop)), on_hangup_(std::move(on_hangup)), thread_([this] { run(); }) {
}

SignalWatcher::~SignalWatcher() {
  stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SignalWatcher::stop() {
  if (thread_.joinable()) {
    pthread_kill(thread_.native_handle(), kWakeSignal);
  }
}

void SignalWatcher::run() {
  const sigset_t set = watched_set();
  for (;;) {
    int signal = 0;
    if (sigwait(&set, &signal) != 0) {
      continue;
    }
    if (signal == SIGHUP) {
      if (on_hangup_) {
        on_hangup_();
      }
      continue;
    }
    if ((signal == SIGTERM || signal == SIGINT) && on_stop_) {
      on_stop_();
    }
    return;
  }
}

}  // namespace gw::adapters
