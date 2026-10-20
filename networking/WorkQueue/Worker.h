#ifndef CONVO_WORKER_H
#define CONVO_WORKER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace convo {

   enum class ProcessingResult : uint8_t {
      Retry = 0,  // this is to map obsoleted boolean value
      Success,
      Ignored,
      TimedOut,
      Error,
      Captured    // special value for supervising adapter only
   };

   using WorkerClock = std::chrono::steady_clock;

   struct InData {
      virtual ~InData() = default;
      virtual std::string name() const { return "BaseInData"; }

      WorkerClock::time_point  execAfter{};
   };

   struct OutData {
      virtual ~OutData() = default;
   };

   using Callback = std::function<ProcessingResult(const std::shared_ptr<OutData>&)>;

   class WorkerQueue
   {
   public:
      virtual bool accepts(const std::shared_ptr<InData>&) const { return true; }

      size_t size() const;
      size_t nbPending() const;
      bool empty() const;
      void cancel();

      void clear()
      {
         std::unique_lock<std::mutex> dataLock(dataMutex_);
         queue_.clear();
      }

      void notifyAll()
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.notify_all();
      }

      void notify()
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.notify_one();
      }

      void push(const std::pair<std::shared_ptr<InData>, Callback>& v)
      {
         std::unique_lock<std::mutex> lock(dataMutex_);
         queue_.push_back(v);
      }

      std::pair<std::shared_ptr<InData>, Callback> pop();

      void wait(const std::chrono::milliseconds& d = std::chrono::milliseconds{ 10 })
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.wait_for(lock, d);
      }

   protected:
      std::deque<std::pair<std::shared_ptr<InData>, Callback>> queue_;
      std::condition_variable cvQueue_;
      std::mutex              cvMutex_;
      mutable std::mutex      dataMutex_;
   };

   class Worker
   {
   public:
      virtual ~Worker() { cancel(); }
      virtual std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>&) = 0;

      virtual void cancel() { isRunning_ = false; }
      bool isRunning() const { return isRunning_; }
      virtual bool isReady() const { return true; }
      virtual void setQueue(const std::shared_ptr<WorkerQueue>& q) { queue_ = q; }
      virtual void pushBack(const std::shared_ptr<InData>&, const Callback&);

   protected:
      std::atomic_bool  isRunning_{ true };
      std::shared_ptr<WorkerQueue>  queue_;
   };

   class Handler
   {
   public:
      virtual ~Handler() = default;
      virtual std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>&) = 0;
   };

   class WorkerImpl : public Worker
   {
   public:
      using Handlers = std::vector<std::shared_ptr<Handler>>;
      WorkerImpl(const Handlers& handlers)
         : handlers_(handlers) {}

      std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>& inData) override
      {
         std::vector<std::shared_ptr<OutData>> result;
         for (const auto& handler : handlers_) {
            if (!isRunning()) {
               return { nullptr };
            }
            const auto& outData = handler->process(inData);
            if (!outData.empty()) {
               result.insert(result.end(), outData.cbegin(), outData.cend());
            }
         }
         return result;
      }

   private:
      const std::vector<std::shared_ptr<Handler>>  handlers_;
   };

   template <typename TIn, typename TOut>
   class HandlerImpl : public Handler
   {
   public:
      std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>& inData) override final
      {
         std::shared_ptr<TIn> data = std::dynamic_pointer_cast<TIn>(inData);
         if (data == nullptr) {
            return {};
         }
         return { std::static_pointer_cast<OutData>(processData(data)) };
      }

   protected:
      virtual std::shared_ptr<TOut> processData(const std::shared_ptr<TIn>&) = 0;
   };


   class WorkerPool
   {
   public:
      WorkerPool(const unsigned int nbThreadsThreshold = 1
         , const unsigned int threadLimit = std::thread::hardware_concurrency() / 2
         , const std::vector<std::shared_ptr<WorkerQueue>>& queues = {});
      virtual ~WorkerPool();

      virtual void processQueued(const std::shared_ptr<InData>&, const Callback&);
      void cancel();
      size_t nbThreads() const;
      virtual bool finished() const;

   protected:  // allow different worker types based on input data to allow by-data thread affinity
      virtual std::shared_ptr<Worker> worker(const std::shared_ptr<InData>&) = 0;
      virtual void newThread(const std::shared_ptr<Worker>&
         , const std::shared_ptr<WorkerQueue>&, unsigned qIndex);
      virtual void doProcess(const std::shared_ptr<Worker>&
         , const std::shared_ptr<InData>&, const Callback&);

   protected:
      const unsigned int threadLimit_;
      const unsigned int nbThreadsThreshold_;
      const std::vector<std::shared_ptr<WorkerQueue>> queues_;
      std::map<unsigned, std::vector<std::pair<std::jthread, std::shared_ptr<Worker>>>> threads_;
      mutable std::mutex      threadsMutex_;
      std::vector<std::atomic_int>   nbProcessing_;
   };

}

#endif	// CONVO_WORKER_H
