// シグナル（詳細設計書 第7章）。全スレッドで SIGTERM・SIGINT・SIGHUP をブロックし、
// 専用スレッドの sigwait で受ける。SIGTERM・SIGINT は on_stop、SIGHUP は on_hangup（Phase 1
// はログだけ）。
#pragma once

#include <functional>
#include <thread>

namespace gw::adapters {

class SignalWatcher {
 public:
  // スレッドを作る前に main で呼ぶ（以後に作るスレッドはブロックを受け継ぐ）
  static void block_signals();

  SignalWatcher(std::function<void()> on_stop, std::function<void()> on_hangup);
  ~SignalWatcher();
  SignalWatcher(const SignalWatcher&) = delete;
  SignalWatcher& operator=(const SignalWatcher&) = delete;
  SignalWatcher(SignalWatcher&&) = delete;
  SignalWatcher& operator=(SignalWatcher&&) = delete;

  // SIGTERM・SIGINT を受けていなくても、待っているスレッドを終わらせる
  void stop();

 private:
  void run();

  std::function<void()> on_stop_;
  std::function<void()> on_hangup_;
  std::thread thread_;
};

}  // namespace gw::adapters
