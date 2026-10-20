#include "SqliteEngine.h"
#include <algorithm>
#include <cstring>

using namespace convo::db;


#if BUILD_SQLITE
#include <sqlite3.h>

SqliteEngine::~SqliteEngine()
{
   close();
}

bool SqliteEngine::isOpen() const
{
   return (db_ != nullptr);
}

bool SqliteEngine::open()
{
   const int rc = sqlite3_open(dbName_.c_str(), &db_);
   if (rc != SQLITE_OK) {
      lastError_ = "open DB " + dbName_ + " failed: " + sqlite3_errmsg(db_);
      return false;
   }
   return true;
}

bool SqliteEngine::close()
{
   const int rc = sqlite3_close(db_);
   if (rc != SQLITE_OK) {
      lastError_ = "close DB " + dbName_ + " failed: " + sqlite3_errmsg(db_);
      return false;
   }
   db_ = nullptr;
   return true;
}

std::unordered_set<std::string> SqliteEngine::tables() const
{
   const auto query = "SELECT name FROM sqlite_master WHERE type='table'";
   sqlite3_stmt* stmt = nullptr;
   int rc = sqlite3_prepare_v2(db_, query, int(std::strlen(query) + 1), &stmt, nullptr);
   if (rc != SQLITE_OK) {
      lastError_ = sqlite3_errmsg(db_);
      return {};
   }
   std::unordered_set<std::string> result;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      result.insert((const char*)sqlite3_column_text(stmt, 0));
   }
   sqlite3_finalize(stmt);
   return result;
}

bool SqliteEngine::execute(const Query* query)
{
   if (!query->fetchable() && (query->tables().empty() || query->bindVals().empty())) {
      return execute(query->text());
   }
   if (!isOpen()) {
      lastError_ = "DB is not open";
      return false;
   }
   sqlite3_stmt* stmt = nullptr;
   if (!prepare(query, &stmt)) {
      return false;
   }
   try {
      if (!bind(query, stmt)) {
         return false;
      }
   }
   catch (const Error& e) {
      lastError_ = e.what();
      return false;
   }
   if (query->fetchable()) {
      fetchStmts_[query->text()] = stmt;
   }
   else {
      const int rc = sqlite3_step(stmt);
      if (rc != SQLITE_DONE) {
         sqlite3_finalize(stmt);
         lastError_ = sqlite3_errmsg(db_);
         return false;
      }
      sqlite3_reset(stmt);
      //sqlite3_clear_bindings(stmt);
      sqlite3_finalize(stmt);
   }
   return true;
}

bool SqliteEngine::execute(const std::string& query)
{
   if (!isOpen()) {
      lastError_ = "DB is not open";
      return false;
   }
   const int rc = sqlite3_exec(db_, query.c_str(), nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK) {
      lastError_ = sqlite3_errmsg(db_);
      return false;
   }
   return true;
}

Data SqliteEngine::fetch(const Query* query)
{
   const auto& columns = query->columns();
   if (columns.empty()) {
      throw Error("nothing to fetch");
   }
   const auto& itStmt = fetchStmts_.find(query->text());
   if (itStmt == fetchStmts_.end()) {
      throw Error("can't find statement for query " + query->text());
   }
   Data result{};
   int rc = 0;
   while ((rc = sqlite3_step(itStmt->second)) == SQLITE_ROW) {
      auto row = getRow(itStmt->second, columns, query->tables());
      switch (query->filter(row)) {
      case Query::FilterResult::pass:
         result.push_back(row);
         break;
      case Query::FilterResult::ignore:
      case Query::FilterResult::aggregate:
         break;
      }
   }
   sqlite3_finalize(itStmt->second);
   fetchStmts_.erase(itStmt);
   return result;
}

convo::ser::Struct SqliteEngine::getRow(sqlite3_stmt* stmt, const Columns& columns
   , const Tables& tables)
{
   convo::ser::Struct result;
   for (int index = 0; index < columns.size(); ++index) {
      const auto column = columns.at(index);
      auto [colIndex, table] = findColumn(column, tables);
      if (colIndex == INT_MAX) {
         colIndex = 0;
      }
      const auto& col = column.column();
      const auto& val = getValue(stmt, index, table->columns().at(colIndex));
      result.push_back({ col, val });
   }
   return result;
}

convo::ser::Type SqliteEngine::getValue(sqlite3_stmt* stmt, int idx, const Column& col)
{
   convo::ser::Type result{};
   auto colType = std::string{ col.type() };
   std::transform(colType.cbegin(), colType.cend(), colType.begin()
      , [](unsigned char c) { return std::tolower(c); });
   if ((colType == "int") || (colType == "integer")) {
      result = sqlite3_column_int(stmt, idx);
   }
   else if (colType == "uint") {
      result = (uint32_t)sqlite3_column_int(stmt, idx);
   }
   else if (colType == "bool") {
      result = (bool)sqlite3_column_int(stmt, idx);
   }
   else if (colType == "bigint") {
      result = (int64_t)sqlite3_column_int64(stmt, idx);
   }
   else if (colType == "biguint") {
      result = (uint64_t)sqlite3_column_int64(stmt, idx);
   }
   else if (colType == "float") {
      result = (float)sqlite3_column_double(stmt, idx);
   }
   else if (colType == "double") {
      result = sqlite3_column_double(stmt, idx);
   }
   else if ((colType == "string") || (colType == "text") || (colType.find("varchar(") == 0)) {
      const auto str = sqlite3_column_text(stmt, idx);
      const int len = sqlite3_column_bytes(stmt, idx);
      result = std::string((const char*)str, len);
   }
   else if (colType == "blob") {
      auto blob = (uint8_t const*)sqlite3_column_blob(stmt, idx);
      int len = sqlite3_column_bytes(stmt, idx);
#if BUILD_ARMORY
      result = BinaryData(blob, len);
#else
      result = std::string((char*)blob, len);
#endif
   }
   else {
      throw Error("unknown type " + std::string{ col.type() });
   }
   return result;
}

bool SqliteEngine::hasFeature(const Feature) const
{
   return false;
}

bool SqliteEngine::bind(const Query* query, sqlite3_stmt* stmt)
{
   if (query->tables().empty()) {
      throw Error("no table associated with query");
   }
   const auto& bindVals = query->bindVals();
   if (bindVals.empty()) {
      return true;
   }
   int bindIndex = 0;
   for (const auto& bindVal : bindVals) { //TODO: get proper table based on ColumnId's ::table()
      auto [colIdx, table] = findColumn(bindVal.second.first, query->tables());
      if (!table || (colIdx < 0)) {
         throw Error("unknown bind column " + bindVal.first + " in " +
            (table ? std::string{ table->name() } : "<null>"));
      }
      const auto& bindName = ":" + bindVal.first;
      bindIndex = sqlite3_bind_parameter_index(stmt, bindName.c_str());
      if (!bindIndex) {
         throw Error("can't obtain index for bind param " + bindName);
      }
      auto colType = table->columns().at(colIdx).type();
      std::transform(colType.cbegin(), colType.cend(), colType.begin()
         , [](unsigned char c) { return std::tolower(c); });
      int rc = SQLITE_NOTFOUND;
      try {
         if ((colType == "int") || (colType == "integer")) {
            int value = 0;
            if (std::holds_alternative<uint32_t>(bindVal.second.second)) {
               value = (int)std::get<uint32_t>(bindVal.second.second);
            }
            else {
               value = std::get<int>(bindVal.second.second);
            }
            rc = sqlite3_bind_int(stmt, bindIndex, value);
         }
         else if (colType == "uint") {
            unsigned value = 0;
            if (std::holds_alternative<uint32_t>(bindVal.second.second)) {
               value = std::get<uint32_t>(bindVal.second.second);
            }
            else if (std::holds_alternative<unsigned>(bindVal.second.second)) {
               value = std::get<unsigned>(bindVal.second.second);
            }
            else {
               value = std::get<int>(bindVal.second.second);
            }
            rc = sqlite3_bind_int(stmt, bindIndex, (int)value);
         }
         else if (colType == "bool") {
            bool value = false;
            if (std::holds_alternative<bool>(bindVal.second.second)) {
               value = std::get<bool>(bindVal.second.second);
            }
            else {
               value = (bool)std::get<int>(bindVal.second.second);
            }
            rc = sqlite3_bind_int(stmt, bindIndex, (int)value);
         }
         else if ((colType == "string") || (colType == "text") || (colType.find("varchar(") == 0)) {
            const auto& str = std::get<std::string>(bindVal.second.second);
            rc = sqlite3_bind_text(stmt, bindIndex, str.c_str(), int(str.size()), SQLITE_TRANSIENT);
         }
         else if (colType == "bigint") {
            sqlite3_int64 value = 0;
            if (std::holds_alternative<int64_t>(bindVal.second.second)) {
               value = std::get<int64_t>(bindVal.second.second);
            }
            else if (std::holds_alternative<uint64_t>(bindVal.second.second)) {
               value = (sqlite3_int64)std::get<uint64_t>(bindVal.second.second);
            }
            else {
               value = std::get<int>(bindVal.second.second);
            }
            rc = sqlite3_bind_int64(stmt, bindIndex, value);
         }
         else if (colType == "biguint") {
            sqlite3_int64 value = 0;
            if (std::holds_alternative<uint64_t>(bindVal.second.second)) {
               value = std::get<uint64_t>(bindVal.second.second);
            }
            else if (std::holds_alternative<int64_t>(bindVal.second.second)) {
               value = (sqlite3_int64)std::get<int64_t>(bindVal.second.second);
            }
            else {
               value = (sqlite3_int64)std::get<int>(bindVal.second.second);
            }
            rc = sqlite3_bind_int64(stmt, bindIndex, value);
         }
         else if (colType == "float") {
            if (std::holds_alternative<float>(bindVal.second.second)) {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<float>(bindVal.second.second));
            }
            else if (std::holds_alternative<int>(bindVal.second.second)) {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<int>(bindVal.second.second));
            }
            else {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<double>(bindVal.second.second));
            }
         }
         else if (colType == "double") {
            if (std::holds_alternative<float>(bindVal.second.second)) {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<float>(bindVal.second.second));
            }
            else if (std::holds_alternative<int>(bindVal.second.second)) {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<int>(bindVal.second.second));
            }
            else {
               rc = sqlite3_bind_double(stmt, bindIndex, std::get<double>(bindVal.second.second));
            }
         }
         else if (colType == "blob") {
#if BUILD_ARMORY
            const auto& blob = std::get<BinaryData>(bindVal.second.second);
            rc = sqlite3_bind_blob(stmt, bindIndex, blob.getPtr(), (int)blob.getSize(), SQLITE_TRANSIENT);
#else
            const auto& blob = std::get<convo::ser::BinaryData>(bindVal.second.second);
            rc = sqlite3_bind_blob(stmt, bindIndex, (char *)blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
#endif
         }
         else {
            throw Error("unknown column type " + std::string{ table->columns().at(colIdx).type() }
               + " in " + std::string{ table->name() });
         }
      }
      catch (const std::exception& e) {
         throw Error("failed to bind " + colType + " to " + bindName + " ("
            + std::to_string(bindIndex) + "): " + e.what());
      }
      if (rc != SQLITE_OK) {
         throw Error("[SQLite] failed to bind " + colType + " to " + bindName + " ("
            + std::to_string(bindIndex) + "): " + std::to_string(rc) + " "
            + sqlite3_errmsg(db_));
      }
   }
   return true;
}

bool SqliteEngine::prepare(const Query* query, sqlite3_stmt** stmt)
{
   const auto& str = query->text();
   const int rc = sqlite3_prepare_v2(db_, str.c_str(), int(str.size() + 1), stmt, nullptr);
   if (rc != SQLITE_OK) {
      lastError_ = sqlite3_errmsg(db_);
      return false;
   }
   return true;
}
#endif //BUILD_SQLITE
