#ifndef __CONVO_SYSTEM_FILE_UTILS_H__
#define __CONVO_SYSTEM_FILE_UTILS_H__

#include <string>
#include <vector>

namespace convo {
   namespace sys_file {
      namespace utils {
         bool isValidFilePath(const std::string&);
         bool fileExist(const std::string&);
         bool pathExist(const std::string&);

         bool cpFile(const std::string& from, const std::string& to);
         bool rmFile(const std::string&);

         std::string absolutePath(const std::string&);
         bool mkPath(const std::string&);
         bool rmDir(const std::string&);

         std::vector<std::string> readDir(const std::string& path
            , const std::string& filter = "", bool onlyFiles = true);
      }

      namespace paths {
         std::string appDataLocation(const std::string& appRoot);
         std::string configDataLocation(const std::string& appRoot);
         std::string applicationDirIfKnown();

         void setArgV0(const char*);
      }
   }
}

#endif // __CONVO_SYSTEM_FILE_UTILS_H__
