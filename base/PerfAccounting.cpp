#include "PerfAccounting.h"
#include <spdlog/spdlog.h>

using namespace convo;

static const std::string kQTnameLong{ "Queue time" };
static const std::string kQTnameShort{ " Q time" };

void PerfAccounting::Entry::add(std::chrono::microseconds interval)
{
   if (interval < std::chrono::microseconds{}) {
      interval = {};
   }
   if (!count_ || (interval < min_)) {
      min_ = interval;
   }
   if (!count_ || (interval > max_)) {
      max_ = interval;
   }
   total_ += interval;
   count_++;
}


void PerfAccounting::add(int key, const std::chrono::microseconds &interval)
{
   entries_[key].add(interval);
}

void PerfAccounting::addQueueTime(const std::chrono::microseconds &interval, int queue)
{
   add(queue, interval);
}

void PerfAccounting::reset()
{
   entries_.clear();
}

void PerfAccounting::report(const std::shared_ptr<spdlog::logger> &logger
   , const std::string &qName, const std::map<int, std::string> &keyMapping) const
{
   std::string output;
   std::string name;
   for (const auto &entry : entries_) {
      if (entry.first < 0) {
         if (qName.empty()) {
            name = kQTnameLong;
         }
         else {
            name = qName + std::to_string(entry.first) + kQTnameShort;
         }
      }
      else {
         const int userVal = entry.first & ~0x1000;
         const bool isBC = (entry.first & 0x1000);
         const auto itMapping = keyMapping.find(userVal);
         name = (itMapping == keyMapping.end())
            ? std::to_string(userVal) : itMapping->second;
         if (isBC) {
            name = "*" + name;
         }
      }
      output += fmt::format("\n\t{}:\t{:.3f} / {:.3f} / {:.3f}\t{}", name
         , entry.second.min(), entry.second.avg(), entry.second.max()
         , entry.second.count());
   }
   logger->info("Performance accounting info{} [min/avg/max count] in "
      "milliseconds (* is broadcast):{}", qName.empty() ? "" : " for " + qName
      , output);
}

void PerfAccounting::report(const ReportCb& cb) const
{
   for (const auto& entry : entries_) {
      const int user = (entry.first >= 0) ? entry.first & ~0x1000 : entry.first;
      const bool isBC = (entry.first >= 0x1000) ? true : false;
      cb(user, isBC, entry.second.min(), entry.second.avg(), entry.second.max()
         , entry.second.count());
   }
}
