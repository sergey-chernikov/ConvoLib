#ifndef __CONVO_CURRENCY_PAIR_H__
#define __CONVO_CURRENCY_PAIR_H__

#include <string>

namespace convo {

   class CurrencyPair
   {
   public:
      CurrencyPair(const std::string& pairString);

      const std::string& numCurrency() const { return numCurrency_; }
      const std::string& denomCurrency() const { return denomCurrency_; }
      const std::string& contraCurrency(const std::string& cur);

   private:
      std::string numCurrency_;
      std::string denomCurrency_;
   };

}

#endif // __CONVO_CURRENCY_PAIR_H__
