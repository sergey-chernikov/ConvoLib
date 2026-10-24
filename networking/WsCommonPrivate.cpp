#include "WsCommonPrivate.h"

#include <cassert>
#include <cstring>
#if BUILD_LWS
#include <libwebsockets.h>
#endif

using namespace convo::ws;

#if BUILD_LWS

TimerHelper::TimerHelper() = default;
TimerHelper::~TimerHelper() = default;

void TimerHelper::scheduleCallback(lws_context *context, std::chrono::milliseconds timeout
   , TimerCallback callback)
{
   const auto timerId = nextTimerId_++;
   auto timer = std::make_unique<TimerHelperData>();
   std::memset(&timer->timerInt_, 0, sizeof(timer->timerInt_));
   timer->timerInt_.owner = timer.get();
   timer->owner_ = this;
   timer->timerId_ = timerId;
   timer->callback_ = std::move(callback);

   lws_sul_schedule(context, 0, &timer->timerInt_, timerCallback, static_cast<lws_usec_t>(timeout / std::chrono::microseconds(1)));

   timers_.insert(std::make_pair(timerId, std::move(timer)));
}

void TimerHelper::clear()
{
   timers_.clear();
   nextTimerId_ = {};
}

void TimerHelper::timerCallback(lws_sorted_usec_list *list)
{
   auto dataInt = static_cast<TimerHelperDataInt*>(list);
   auto data = dataInt->owner;
   data->callback_();
   auto count = data->owner_->timers_.erase(data->timerId_);
   assert(count == 1);
}
#endif   //BUILD_LWS
