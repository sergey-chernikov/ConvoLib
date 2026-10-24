#ifndef CONVO_WS_PRIVATE_H
#define CONVO_WS_PRIVATE_H

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#if BUILD_LWS
#include <libwebsockets.h>

struct lws_context;
struct lws_sorted_usec_list;
#endif

namespace convo {
      namespace ws {
#if BUILD_LWS
         class TimerHelperData;
         struct TimerHelperDataInt : lws_sorted_usec_list_t
         {
            TimerHelperData* owner;
         };

         using TimerCallback = std::function<void()>;

         class TimerHelper;
         class TimerHelperData
         {
         public:
            TimerHelper* owner_{};
            TimerHelperDataInt timerInt_;
            uint64_t timerId_{};
            TimerCallback callback_;
         };

         class TimerHelper
         {
         public:
            TimerHelper();
            ~TimerHelper();

            void scheduleCallback(lws_context *context, std::chrono::milliseconds timeout, TimerCallback callback);

            void clear();

         private:
            static void timerCallback(lws_sorted_usec_list *list);

            std::map<uint64_t, std::unique_ptr<TimerHelperData>> timers_;
            uint64_t nextTimerId_{};
         };
#endif
   }
}

#endif // CONVO_WS_PRIVATE_H
