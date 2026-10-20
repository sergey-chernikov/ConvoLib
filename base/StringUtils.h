#ifndef CONVO_STRING_UTILS_H
#define CONVO_STRING_UTILS_H

#include <stdint.h>
#include <string>
#include <vector>

namespace convo {

   std::string toHex(const std::string &str, bool uppercase = false);
   std::string toHex(const std::vector<uint8_t>& str, bool uppercase = false);
   std::string fromHexToStr(const std::string& str) noexcept;
   unsigned long fromHex(const std::string&);

   // Works for ASCII encoding only
   std::string toLower(const std::string&);
   std::string toUpper(const std::string&);

   // Very basic email address verification check (checks that there is one '@' symbol and at least one '.' after that)
   bool isValidEmail(const std::string &str);

   int convertToInt(const std::string& str, bool& converted);
   bool convertToBool(const std::string& str, bool& converted);

   std::vector<std::string> split(const std::string& str, char separator);
   std::string join(const std::vector<std::string>&, const std::string& separator);

   std::string trim(std::string str);

}

#endif
