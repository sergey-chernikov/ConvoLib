#ifndef __CONVO_DB_SQLITE_ENGINE_H__
#define __CONVO_DB_SQLITE_ENGINE_H__

#include "base/SerDeser.h"
#include "Wrapper.h"

#if BUILD_SQLITE
struct sqlite3;
struct sqlite3_stmt;
#endif

namespace convo {
   namespace db {

#if BUILD_SQLITE
      class SqliteEngine : public Engine
      {
      public:
         SqliteEngine(const std::string& dbName)
            : dbName_(dbName) {}
         virtual ~SqliteEngine();

         bool isOpen() const override;
         bool open() override;
         bool close() override;
         std::unordered_set<std::string> tables() const override;

         bool execute(const Query*) override;
         bool execute(const std::string&) override;
         Data fetch(const Query*) override;
         std::string lastError() const override { return lastError_; }
         std::unique_ptr<Lock> lock() override { return std::move(std::make_unique<SqlLock>(this)); }
         bool hasFeature(const Feature) const override;

      protected:
         bool bind(const Query*, sqlite3_stmt*);
         bool prepare(const Query*, sqlite3_stmt**);
         static ser::Struct getRow(sqlite3_stmt*, const Columns&, const Tables&);
         static ser::Type getValue(sqlite3_stmt*, int idx, const Column&);

      private:
         const std::string dbName_;
         sqlite3* db_{ nullptr };
         mutable std::string lastError_;
         std::unordered_map<std::string, sqlite3_stmt*>  fetchStmts_;
      };
#endif	//BUILD_SQLITE
   }
}

#endif	//__CONVO_DB_SQLITE_ENGINE_H__
