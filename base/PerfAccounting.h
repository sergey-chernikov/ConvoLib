#ifndef CONVO_PERF_ACCOUNTING_H
#define CONVO_PERF_ACCOUNTING_H

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace spdlog {
   class logger;
}

namespace convo {

   class TimeInterval
   {
   public:
      TimeInterval() : point_(std::chrono::high_resolution_clock::now()) {}
      virtual void report(const std::string& name, unsigned nbCalls = 1) = 0;

   protected:
      std::chrono::microseconds get() const
      {
         return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - point_);
      }

   protected:
      const std::chrono::high_resolution_clock::time_point point_;
   };

   class PerfAccounting
   {
   public:
      void add(int key, const std::chrono::microseconds &interval);
      void addQueueTime(const std::chrono::microseconds &interval, int queue = -1);
      void reset();

      void report(const std::shared_ptr<spdlog::logger> &, const std::string &name
         , const std::map<int, std::string> &keyMapping) const;

      using ReportCb = std::function<void(int user, bool isBroadcast
         , double msMinProcTime, double msAvgProcTime, double msMaxProcTime
         , size_t count)>;
      void report(const ReportCb &) const;

   private:
      class Entry
      {
      public:
         void add(std::chrono::microseconds interval);

         double min() const { return min_.count() / 1000.0; }
         double max() const { return max_.count() / 1000.0; }
         double avg() const { return total_.count() / 1000.0 / count_; }
         size_t count() const { return count_; }

      private:
         size_t   count_{ 0 };
         std::chrono::microseconds  total_;
         std::chrono::microseconds  min_;
         std::chrono::microseconds  max_;
      };

      std::map<int, Entry> entries_;
   };

} // namespace convo

#endif	// CONVO_PERF_ACCOUNTING_H
