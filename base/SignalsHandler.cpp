#include "SignalsHandler.h"

using namespace convo;

#ifdef _WIN32

void SignalsHandler::registerHandler(const SignalCb &cb, const std::initializer_list<int> &signals)
{
   //TODO
}

#else // _WIN32

#include <thread>
#include <signal.h>

void SignalsHandler::registerHandler(const SignalCb &cb, const std::initializer_list<int> &signals)
{
   sigset_t set;

   sigemptyset(&set);
   for (int sig : signals)  {
      sigaddset(&set, sig);
   }

   sigprocmask(SIG_BLOCK, &set, nullptr);

   auto cbWrap = [set, cb] {
      while (true) {
         int sig = 0;
         sigwait(&set, &sig);
         cb(sig);
      }
   };

   std::thread(cbWrap).detach();
}

#endif // _WIN32
