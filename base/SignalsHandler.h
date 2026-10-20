#ifndef CONVO_SIGNALS_HANDLER_H
#define CONVO_SIGNALS_HANDLER_H

#include <csignal>
#include <functional>

namespace convo {

   using SignalCb = std::function<void(int)>;

   class SignalsHandler
   {
   public:
      // Starts background thread that will wait for the listed POSIX signals.
      // Must be called from main thread before starting other threads (we use sigwait and need to block listed signals first).
      // Does nothing on Windows.
      static void registerHandler(const SignalCb&, const std::initializer_list<int>& signals = { SIGTERM, SIGINT });
   };
}

#endif
