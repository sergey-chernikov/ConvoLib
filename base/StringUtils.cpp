#include "StringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace {

   const char HexUpper[16] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'A', 'B', 'C', 'D', 'E', 'F' };

   const char HexLower[16] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'a', 'b', 'c', 'd', 'e', 'f' };

}

namespace convo {

   // Copied from Botan
   std::string toHex(const std::string &str, bool uppercase)
   {
      const char* tbl = uppercase ? HexUpper : HexLower;

      std::string result;
      result.reserve(str.length() * 2);
      for (size_t i = 0; i < str.length(); ++i) {
         auto x = uint8_t(str[i]);
         result.push_back(tbl[(x >> 4) & 0x0F]);
         result.push_back(tbl[(x) & 0x0F]);
      }

      return result;
   }

   std::string toHex(const std::vector<uint8_t>& bytes, bool uppercase)
   {
      const char* tbl = uppercase ? HexUpper : HexLower;

      std::string result;
      result.reserve(bytes.size() * 2);
      for (size_t i = 0; i < bytes.size(); ++i) {
         auto x = bytes.at(i);
         result.push_back(tbl[(x >> 4) & 0x0F]);
         result.push_back(tbl[(x) & 0x0F]);
      }

      return result;
   }

   std::string fromHexToStr(const std::string& str) noexcept
   {
      if (str.length() % 2 != 0) {
         return {};
      }
      std::string result;
      for (size_t i = 0; i < str.length(); i += 2) {
         std::string byte = str.substr(i, 2);
         char chr = (char)(int)strtol(byte.c_str(), NULL, 16);
         if ((chr == 0) && (byte != "00")) {
            continue;
         }
         result.push_back(chr);
      }
      return result;
   }

   unsigned long fromHex(const std::string& str)
   {
      return std::stoul(str, nullptr, 16);
   }

   std::string toLower(const std::string& s)
   {
      auto str = s;
      std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
         return ::tolower(c);
      });
      return str;
   }

   std::string toUpper(const std::string& s)
   {
      auto str = s;
      std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
         return ::toupper(c);
      });
      return str;
   }

   bool isValidEmail(const std::string &str)
   {
      auto at = std::find(str.begin(), str.end(), '@');
      auto dot = std::find(at, str.end(), '.');
      return (at != str.end()) && (dot != str.end());
   }

   int convertToInt(const std::string& str, bool& converted)
   {
      converted = false;
      try {
         int result = std::stoi(str);
         converted = true;
         return result;
      } catch (...) {
         return 0;
      }
   }

   bool convertToBool(const std::string& str, bool& converted)
   {
      auto inStr = toLower(str);

      if (inStr == "false") {
         converted = true;
         return false;
      }

      if (inStr == "true") {
         converted = true;
         return true;
      }

      return convertToInt(inStr, converted) != 0;
   }

   std::vector<std::string> split(const std::string &str, char separator)
   {
      // Copied from https://stackoverflow.com/questions/14265581/parse-split-a-string-in-c-using-string-delimiter-standard-c
      std::vector<std::string> result;
      size_t start = 0;
      size_t end = str.find(separator);
      while (end != std::string::npos) {
          result.push_back(str.substr(start, end - start));
          start = end + 1;
          end = str.find(separator, start);
      }
      result.push_back(str.substr(start, end));
      return result;
   }

   std::string join(const std::vector<std::string>& v, const std::string& separator)
   {
      if (v.empty()) {
         return {};
      }
      std::string result;
      for (size_t i = 0; i < v.size(); ++i) {
         result += v.at(i);
         if (i < (v.size() - 1)) {
            result += separator;
         }
      }
      return result;
   }

   std::string trim(std::string str)
   {
      // Copied from https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
      str.erase(std::find_if(str.rbegin(), str.rend(), [](int ch) { return !std::isspace(ch); }).base(), str.end());
      str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](int ch) { return !std::isspace(ch); }));
      return str;
   }

} // namespace convo
