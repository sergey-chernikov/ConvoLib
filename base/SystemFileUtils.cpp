#include "SystemFileUtils.h"
#ifdef WIN32
#  include <ShlObj.h>
#  include <KnownFolders.h>
#  include <direct.h>
#  include <Shlwapi.h>
#else
#  include <stdlib.h>
#  include <dirent.h>
#  include <glob.h>
#  include <unistd.h>
#endif
#include <algorithm>
#include <filesystem>
#include <string.h>
#include <sys/stat.h>
#include <fstream>

namespace fs = std::filesystem;

namespace {
#ifndef WIN32
#ifdef __APPLE__
   const std::string kDataDir = "Library/Application Support";
   const std::string kConfigDir = "Library/Application Support";
#else
   const std::string kDataDir = ".local/share";
   const std::string kConfigDir = ".config";
#endif
#endif

   std::string g_appDir;
} // namespace

using namespace convo::sys_file;

bool utils::isValidFilePath(const std::string &/*path*/)
{
   // TODO: not implemented - shouldn't rely on path existence in FS
   return true;
}

bool utils::fileExist(const std::string &path)
{
   struct stat buf;
   if (::stat(path.c_str(), &buf) != 0) {
      return false;
   }
   return ((buf.st_mode & S_IFMT) == S_IFREG);
}

bool utils::pathExist(const std::string &path)
{
   struct stat buf;
   if (::stat(path.c_str(), &buf) != 0) {
      return false;
   }
   return ((buf.st_mode & S_IFMT) == S_IFDIR);
}

bool utils::cpFile(const std::string &from, const std::string &to)
{
   std::ifstream src(from);
   if (!src.good()) {
      return false;
   }
   std::ofstream dst(to);
   if (!dst.good()) {
      return false;
   }
   dst << src.rdbuf();
   return true;
}

bool utils::rmFile(const std::string& path)
{
   return (std::remove(path.c_str()) == 0);
}

std::string utils::absolutePath(const std::string &path)
{
#ifdef WIN32
   char fullPath[MAX_PATH];
   if (::GetFullPathName(path.c_str(), MAX_PATH, fullPath, nullptr) <= 0) {
      return path;
   }
   return std::string(fullPath);
#else
   //TODO: use std::filesystem::absolute from C++17
#endif
   return path;
}

bool utils::mkPath(const std::string &path)
{
   std::vector<std::string> dirs = { path };
   auto p = path;
   while (!p.empty()) {
      const auto pSep = p.find_last_of('/');
      if (pSep == std::string::npos) {
         break;
      }
      p = p.substr(0, pSep);
      if (!p.empty()) {
         dirs.push_back(p);
      }
   }
   std::reverse(dirs.begin(), dirs.end());

   for (const auto &dir : dirs) {
      if (pathExist(dir)) {
         continue;
      }
#ifdef WIN32
      if ((dir.size() == 2) && (dir[1] == ':')) {
         continue;
      }
      if (_mkdir(dir.c_str()) != 0) {
         return false;
      }
#else
      if (mkdir(dir.c_str(), 0755) != 0) {
         return false;
      }
#endif
   }
   return true;
}

std::vector<std::string> utils::readDir(const std::string &path
   , const std::string &filter, bool onlyFiles)
{
   if (!pathExist(path)) {
      return {};
   }
   std::vector<std::string> result;
   for (const auto& ent : fs::directory_iterator(path)) {
      if (ent.is_directory() && ent.path().is_relative()) {
         continue;
      }
      if (onlyFiles && ent.is_directory()) {
         continue;
      }
#ifdef WIN32
      if (!filter.empty() && !::PathMatchSpec(ent.path().generic_string().c_str()
         , filter.c_str())) {
         continue;
      }
#else //TODO: use cross-platform regex matching from std::regex
#endif
      result.push_back(ent.path().generic_string());
   }
#ifdef GLOB_UNIX_IMPL
   result.clear();
   glob_t globResult;
   memset(&globResult, 0, sizeof(globResult));
   const auto fltCopy = filter.empty() ? std::string("*") : filter;
   if (glob((path + "/" + fltCopy).c_str(), 0, NULL, &globResult) != 0) {
      globfree(&globResult);
      return {};
   }
   for (unsigned int i = 0; i < globResult.gl_pathc; ++i) {
      std::string entry = globResult.gl_pathv[i];
      if ((entry == ".") || (entry == "..")) {
         continue;
      }
      if (onlyFiles && pathExist(entry)) {
         continue;
      }
      entry.erase(0, path.size() + 1);
      result.emplace_back(std::move(entry));
   }
   globfree(&globResult);
#endif
   return result;
}

#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif
bool utils::rmDir(const std::string &path)
{
   const auto files = readDir(path);
   bool result = true;
   for (const auto &file : files) {
      result &= rmFile(path + "/" + file);
   }
   if (result) {
      result &= (rmdir(path.c_str()) == 0);
   }
   return result;
}


std::string paths::appDataLocation(const std::string& appRoot)
{
   std::string result;
#ifdef WIN32
   PWSTR path = nullptr;
   char strPath[512];
   SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path);
   wcstombs(strPath, path, sizeof(strPath));
   result = strPath;
   ::CoTaskMemFree(static_cast<void*>(path));
   std::replace(result.begin(), result.end(), '\\', '/');
#else
   result = getenv("HOME");
   result += "/" + kDataDir;
#endif   //WIN32
   result += "/" + appRoot;
   return result;
}

std::string paths::configDataLocation(const std::string& appRoot)
{
#ifdef WIN32
   return appDataLocation(appRoot);
#else
   std::string result = getenv("HOME");
   result += "/" + kConfigDir + "/" + appRoot;
   return result;
#endif
}

std::string paths::applicationDirIfKnown()
{
   return g_appDir;
}

void paths::setArgV0(const char *arg)
{
   g_appDir = arg;
#ifdef WIN32
   std::replace(g_appDir.begin(), g_appDir.end(), '\\', '/');
#endif
   const auto sepPos = g_appDir.find_last_of('/');
   if (sepPos != std::string::npos) {
      g_appDir = g_appDir.substr(0, sepPos);
   } else {
      // If binary is started with only basename (when found from $PATH) appDir is not easily known
      g_appDir.clear();
   }
}
