#include "Wrapper.h"
#include <algorithm>
#include <cstring>

using namespace convo::db;
using namespace convo::ser;

namespace {
   const std::unordered_set<std::string>  kSQLReservedWords{ "on", "ON" };
}

namespace convo {
   namespace db {
      std::string get(const DataMap& m, const ColumnId& key)
      {
         try {
            const auto& v = m.at(key);
            if (std::holds_alternative<std::string>(v)) {
               return std::get<std::string>(v);
            }
            else if (std::holds_alternative<int>(v)) {
               return std::to_string(std::get<int>(v));
            }
            else if (std::holds_alternative<BinaryData>(v)) {
               const auto& binData = std::get<BinaryData>(v);
               return std::string((const char*)binData.data(), binData.size());
            }
            else {
               throw Error("incompatible type");
            }
         }
         catch (const std::exception&) {
            throw Error(key() + " not found");
         }
         return {};
      }

      template<> BinaryData get(const DataMap& m, const ColumnId& key)
      {
         try {
            const auto& v = m.at(key);
            if (std::holds_alternative<BinaryData>(v)) {
               return std::get<BinaryData>(v);
            }
            else if (std::holds_alternative<std::string>(v)) {
               const auto& s = std::get<std::string>(v);
               return BinaryData(s.cbegin(), s.cend());
            }
            else {
               throw Error("incompatible type");
            }
         }
         catch (const std::exception&) {
            throw Error(key() + " not found");
         }
         return {};
      }

      template<> std::string get(const DataMap& m, const ColumnId& key)
      {
         return get(m, key);
      }

      std::string toString(const DataMap& dm)
      {
         std::string result;
         for (const auto& d : dm) {
            result += d.first() + "=" + convo::ser::toString(d.second) + " ";
         }
         result.pop_back();
         return result;
      }

      ser::Struct toStruct(const DataMap& dm)
      {
         ser::Struct st;
         for (const auto& d : dm) {
            st.push_back({ d.first.column(), d.second });
         }
         return st;
      }

      bool has(const Columns& colIds, const ColumnId& colId)
      {
         for (const auto& c : colIds) {
            if (c == colId) {
               return true;
            }
         }
         return false;
      }

   }
}


int Table::findColIndex(const std::string& colName) const
{
   if (colName == "*") {
      return INT_MAX;
   }
   const auto& cols = columns();
   for (int i = 0; i < cols.size(); ++i) {
      if (colName == cols.at(i).name()) {
         return i;
      }
   }
   return -1;
}


QuerySQL::QuerySQL(const std::shared_ptr<Table>& table)
{
   if (table) {
      if (table->columns().empty()) {
         throw Error("empty table " + std::string{ table->name() });
      }
      tables_[table->name()] = table;
   }
}

std::string QuerySQL::boundText(const QuerySQL& subQuery, const std::string& alias)
{
   auto subBindVals = subQuery.bindVals();
   auto subQueryText = subQuery.text();
   auto table = subQuery.tables().cbegin()->second;
   for (const auto& pair : subBindVals) {
      auto colPair = pair.second;
      colPair.first.setTable(alias.empty() ? std::string{ table->name() } : alias);
      const auto& bindName = setBindVal(colPair, table);
      if (bindName != pair.first) {
         const auto it = subQueryText.find(":" + pair.first);
         subQueryText.replace(it, pair.first.size() + 1, ":" + bindName);
      }
   }
   return subQueryText;
}

QuerySQL::QuerySQL(const QuerySQL& subQuery, const std::string_view& alias)
{
   const auto& tables = subQuery.tables();
   if (tables.size() != 1) {
      throw Error("invalid table[s] in sub-query");
   }

   subQuery_ = "(" + boundText(subQuery, std::string{ alias }) + ")";
   if (alias.empty()) {
      tables_ = tables;
   }
   else {
      subQuery_ += " AS " + std::string{ alias };
      tables_[std::string{alias.data()}] = tables.cbegin()->second;
   }

   for (const auto& col : subQuery.columns()) {
      if (!col.alias().empty()) {
         colAliases_[tables.cbegin()->first][col.alias()] = col.column();
      }
   }
}

QuerySQL& QuerySQL::select(const std::vector<ColumnId>& columns)
{
   if (!columns_.empty()) {
      throw error::Syntax("another query was started previously");
   }
   if (tables_.empty()) {
      throw error::SQLSelectValidation("no table associated");
   }
   if (tables_.size() > 1) {
      throw error::SQLSelectValidation(std::to_string(tables_.size()) + " tables associated");
   }
   isSelect_ = true;
   const auto& tableName = tables_.cbegin()->first;
   columns_ = columns;
   query_ = "SELECT ";
   for (int i = 0; i < columns.size(); ++i) {
      auto col = columns.at(i);
      if (kSQLReservedWords.find(col()) == kSQLReservedWords.end()) {
         query_ += col();
      }
      else {
         query_ += "\"" + col() + "\"";
      }
      if (i < (columns.size() - 1)) {
         query_ += ", ";
      }
   }
   query_ += " FROM " + (subQuery_.empty() ? std::string{ tableName } : subQuery_);
   return *this;
}

QuerySQL& QuerySQL::where()
{
   if (whereStarted_) {
      throw error::SQLWhereSyntax("already started");
   }
   whereStarted_ = true;
   query_.append(" WHERE ");
   return *this;
}

QuerySQL& QuerySQL::where(const ColumnId& col)
{
   if (whereStarted_) {
      throw error::SQLWhereSyntax("already started");
   }
   if (tables_.empty()) {
      throw error::SQLWhereSyntax("no table associated");
   }
   whereStarted_ = true;
   query_.append(" WHERE ");
   return column(col);
}

QuerySQL& QuerySQL::isTrue()
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   if (currentCol_.empty()) {
      throw error::SQLWhereSyntax("no LVal");
   }
   const auto& itTable = currentCol_.table().empty() ? tables_.cbegin() : tables_.find(currentCol_.table());
   auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
   if (table) {
      const auto col = table->columns().at(table->findColIndex(currentCol_.column()));
      if (col.type() != "Bool") {
         throw error::Syntax(currentCol_() + " should be boolean, but it's " + std::string{ col.type() });
      }
   }
   currentCol_.clear();
   return *this;
}

QuerySQL& QuerySQL::True()
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   query_ += " true";
}

QuerySQL& QuerySQL::like(const std::string& expr)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   query_ += " like " + expr.empty() ? "%" : expr;
   return *this;
}

QuerySQL& QuerySQL::eq(const Type& value)
{
   return compare(value, "=", false);
}

QuerySQL& QuerySQL::eqCol(const ColumnId& col)
{
   return compare(col, "=");
}

QuerySQL& QuerySQL::opVal(const std::string& op, const std::string& val)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   if (currentCol_.empty()) {
      throw error::SQLWhereSyntax("no LVal");
   }
   query_ += " " + op + " " + val;
   currentCol_.clear();
   return *this;
}

QuerySQL& QuerySQL::eqVal(int64_t val)
{
   return opVal("=", std::to_string(val));
}

QuerySQL& QuerySQL::eqVal(int val)
{
   return opVal("=", std::to_string(val));
}

QuerySQL& QuerySQL::eqVal(const std::string& val)
{
   return opVal("=", "\"" + val + "\"");
}

QuerySQL& QuerySQL::eqVal(bool val)
{
   return opVal("=", val ? "TRUE" : "FALSE");
}

QuerySQL& QuerySQL::isTrue(bool val)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   query_ += val ? "TRUE" : "FALSE";
   return *this;
}

QuerySQL& QuerySQL::ne(const Type& value)
{
   return compare(value, "!=", false);
}

QuerySQL& QuerySQL::neVal(int64_t val)
{
   return opVal("<>", std::to_string(val));
}

QuerySQL& QuerySQL::compare(const Type& value, const std::string& op, bool isNumeric)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   if (currentCol_.empty()) {
      throw error::SQLWhereSyntax("no LVal");
   }
   const auto& itTable = currentCol_.table().empty() ? tables_.cbegin() : tables_.find(currentCol_.table());
   auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
   if (isNumeric && table) {
      const int colIndex = table->findColIndex(currentCol_.column());
      if (colIndex < 0) {
         throw error::SQLSelectValidation("[compare] unknown column " + currentCol_());
      }
      const auto col = table->columns().at(colIndex);
      if ((col.type() == "String") || (col.type() == "BLOB")) {
         throw error::Syntax("can't compare non-numeric type " + std::string{ col.type() }
            + " by " + op);
      }
   }
   const auto& bindName = setBindVal({ currentCol_, value}, table);
   query_ += " " + op + " :" + bindName;
   currentCol_.clear();
   return *this;
}

QuerySQL& QuerySQL::compare(const std::string& val, const std::string& op)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   if (currentCol_.empty()) {
      throw error::SQLWhereSyntax("no LVal");
   }
   query_ += " " + op + " " + val;
   currentCol_.clear();
   return *this;
}

QuerySQL& QuerySQL::compare(const ColumnId& col, const std::string& op)
{
   if (!onStarted_) {
      throw Error("ON is not started (" + col() + ")");
   }
   if (currentCol_.empty()) {
      throw error::SQLWhereSyntax("no LVal");
   }
   query_ += " " + op + " " + col();
   currentCol_.clear();
   return *this;
}

QuerySQL& QuerySQL::ge(const Type& value)
{
   return compare(value, ">=");
}

QuerySQL& QuerySQL::gt(const Type& value)
{
   return compare(value, ">");
}

QuerySQL& QuerySQL::gtVal(int64_t val)
{
   return opVal(">", std::to_string(val));
}

QuerySQL& QuerySQL::le(const Type& value)
{
   return compare(value, "<=");
}

QuerySQL& QuerySQL::lt(const Type& value)
{
   return compare(value, "<");
}

QuerySQL& QuerySQL::conj(const std::string& word)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   if (!currentCol_.empty()) {
      throw error::Syntax("can't follow the column (" + currentCol_() + ")");
   }
   query_ += " " + word + " ";
   return *this;
}

QuerySQL& QuerySQL::conj(const ColumnId& col, const std::string& word)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started (" + col() + ")");
   }
   if (!currentCol_.empty()) {
      throw error::SQLWhereSyntax("column " + currentCol_() + " already set");
   }
   const auto& itTable = col.table().empty() ? tables_.cbegin() : tables_.find(col.table());
   auto table = (itTable == tables_.end()) ? nullptr : itTable->second;

   if (table) {
      if (table->findColIndex(col.column()) < 0) {
         throw error::SQLWhereSyntax("[conj] unknown column " + col() + " in " +
            std::string{ table->name() });
      }
   }
   else if (!tables_.empty()) {
      throw Error("invalid table " + col.table());
   }
   currentCol_ = col;
   query_ += " " + word + " " + col();
   return *this;
}

QuerySQL& QuerySQL:: And()
{
   return conj("AND");
}

QuerySQL& QuerySQL::And(const ColumnId& col)
{
   return conj(col, "AND");
}

QuerySQL& QuerySQL:: Or()
{
   return conj("OR");
}

QuerySQL& QuerySQL::Or(const ColumnId& col)
{
   return conj(col, "OR");
}

QuerySQL& QuerySQL::orderBy(const std::vector<ColumnId>& columns, bool ascending)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (columns.empty()) {
      throw Error("empty columns list");
   }
   query_ += " ORDER BY ";
   for (const auto& col : columns) {
      const auto& itTable = col.table().empty() ? tables_.cbegin() : tables_.find(col.table());
      auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
      if (!table) {
         throw Error("unknown table " + col.table());
      }
      if (table->findColIndex(col.column()) < 0) {
         throw Error("unknown order by column " + col() + " in " + std::string{ table->name() });
      }
      query_ += col() + ", ";
   }
   query_.pop_back(), query_.pop_back();
   if (ascending) {
      query_ += " ASC";
   }
   else {
      query_ += " DESC";
   }
   return *this;
}

QuerySQL& QuerySQL::groupBy(const std::vector<ColumnId>& columns)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (columns.empty()) {
      throw Error("empty columns list");
   }
   query_ += " GROUP BY ";
   for (const auto& col : columns) {
      const auto& itTable = col.table().empty() ? tables_.cbegin() : tables_.find(col.table());
      auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
      if (!table) {
         throw Error("unknown table " + col.table());
      }
      if (table->findColIndex(col.column()) < 0) {
         throw Error("unknown group by column " + col() + " in " + std::string{ table->name() });
      }
      query_ += col() + ", ";
   }
   query_.pop_back(), query_.pop_back();
   return *this;
}

QuerySQL& QuerySQL::limit(size_t n)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (n == 0) {
      throw Error("invalid limit value");
   }
   query_ += " LIMIT " + std::to_string(n);
   return *this;
}

QuerySQL& QuerySQL::offset(size_t n)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   query_ += " OFFSET " + std::to_string(n);
   return *this;
}

QuerySQL& QuerySQL::column(const ColumnId& col)
{
   if (!whereStarted_ && !onStarted_) {
      throw Error("WHERE or ON not started (" + col() + ")");
   }
   if (!currentCol_.empty()) {
      throw error::SQLWhereSyntax("column " + currentCol_() + " already set");
   }
   const auto& itTable = col.table().empty() ? tables_.cbegin() : tables_.find(col.table());
   auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
   if (!table && !tables_.empty()) {
      throw error::SQLWhereSyntax("unknown table " + col.table());
   }
   if (table && (table->findColIndex(col.column()) < 0) && !colAliases_.empty()) {
      try {
         const auto& origCol = colAliases_.at(table->name()).at(col.column());
         if (table->findColIndex(origCol) < 0) {
            throw Error("alias col not found");
         }
      }
      catch (const std::exception&) {  //column might be renamed in generated table
/*         throw error::SQLWhereSyntax("[col] unknown column <" + col() + "> in "
            + std::string{ table->name() });*/
      }
   }
   currentCol_ = col;
   if (kSQLReservedWords.find(col()) == kSQLReservedWords.end()) {
      query_ += col();
   }
   else {
      query_ += "\"" + col() + "\"";
   }
   return *this;
}

std::string QuerySQL::setBindVal(const std::pair<ColumnId, Type>& bindVal, const std::shared_ptr<Table>& table)
{
   if (table && (table->findColIndex(bindVal.first.column()) < 0)) {
      throw Error("unknown bind column " + bindVal.first() + " in " + std::string{ table->name() });
   }
   auto bindName = bindVal.first.column();
   while (bindVals_.find(bindName) != bindVals_.end()) {
      bindName += "_";
   }
   bindVals_[bindName] = bindVal;
   return bindName;
}

QuerySQL& QuerySQL::subExpression(const QuerySQL& subQuery)
{
   if (!whereStarted_) {
      throw error::SQLWhereSyntax("not started");
   }
   const auto& subBindVals = subQuery.bindVals();
   auto subQueryText = subQuery.text();
   for (const auto& bindVal : subBindVals) {
      const auto& col = bindVal.second.first;
      const auto& itTable = col.table().empty() ? tables_.cbegin() : tables_.find(col.table());
      auto table = (itTable == tables_.end()) ? nullptr : itTable->second;
      if (table && (table->findColIndex(col.column()) < 0)) {
         throw error::SQLWhereSyntax("[sub] unknown column " + col() + " in " +
            std::string{ table->name() });
      }
      const auto& bindName = setBindVal(bindVal.second, table);
      if (bindName != bindVal.first) {
         const auto it = subQueryText.find(":" + bindVal.first);
         subQueryText.replace(it, bindVal.first.size() + 1, ":" + bindName);
      }
   }
   query_ += "(" + subQueryText + ")";
   return *this;
}

QuerySQL& QuerySQL::join(const std::shared_ptr<Table>& table, const std::string& op)
{
   if (!table || table->empty()) {
      throw Error("can't join on empty table");
   }
   if (tables_.size() > 1) {
      throw Error(std::to_string(tables_.size()) + " tables already assigned");
   }
   tables_[table->name()] = table;
   query_ += " " + op + " " + std::string{ table->name() };
   return *this;
}

QuerySQL& QuerySQL::leftJoin(const std::shared_ptr<Table>& table)
{
   return join(table, "LEFT JOIN");
}

QuerySQL& QuerySQL::join(const QuerySQL& query
   , const std::string_view& alias, const std::string& op)
{
   if (tables_.empty() || query.tables().empty()) {
      throw Error("can't join on empty table");
   }
   const auto& tables = query.tables();
   if ((tables.size() == 1) && !alias.empty()) {
      tables_[std::string{alias.data()}] = tables.cbegin()->second;
   }
   else {
      tables_.insert(tables.cbegin(), tables.cend());
   }
   auto table = query.tables().cbegin()->second;
   query_ += " " + op + " (" + boundText(query, std::string{ alias }) + ") AS "
      + (alias.empty() ? std::string{ table->name() } : std::string{ alias });
   return *this;
}

QuerySQL& QuerySQL::leftJoin(const QuerySQL& query
   , const std::string_view& alias)
{
   return join(query, alias, "LEFT JOIN");
}

QuerySQL& QuerySQL::on(const ColumnId& col1, const ColumnId& col2)
{
   onStarted_ = true;
   query_ += " ON ";
   if (col2.empty()) {
      return column(col1);
   }
   query_ += col1() + " = " + col2();
   return *this;
}

void QuerySQL::reset()
{
   columns_.clear();
   bindVals_.clear();
   currentCol_ = {};
   isSelect_ = false;
   whereStarted_ = false;
   onStarted_ = false;
}

void QuerySQL::insert(const DataMap& values, const std::string& op)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (tables_.size() > 1) {
      throw Error("ambiguous table");
   }
   reset();
   auto table = tables_.cbegin()->second;
   query_ = op + " INTO " + std::string{ table->name() } + " (";
   std::string valsQuery;
   for (const auto& val : values) {
      if (table->findColIndex(val.first.column()) < 0) {
         throw Error("invalid column " + val.first());
      }
      if (kSQLReservedWords.find(val.first.column()) == kSQLReservedWords.end()) {
         query_ += val.first.column() + ", ";
      }
      else {
         query_ += "\"" + val.first.column() + "\", ";
      }
      valsQuery += ":" + val.first.column() + ", ";
   }
   query_.pop_back(), query_.pop_back();  // last ", " is not needed
   valsQuery.pop_back(), valsQuery.pop_back();
   query_ += ") VALUES(" + valsQuery + ")";
   for (const auto& val : values) {
      bindVals_[val.first.column()] = val;
   }
}

QuerySQL& QuerySQL::update(const DataMap& vals)
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (tables_.size() > 1) {
      throw Error("ambiguous table");
   }
   reset();
   auto table = tables_.cbegin()->second;
   query_ = "UPDATE " + std::string{ table->name() } + " SET ";
   for (const auto& val : vals) {
      const auto& colName = val.first.column();
      if (table->findColIndex(colName) < 0) {
         throw error::Syntax("invalid column name: " + colName);
      }
      const auto& bindName = setBindVal(val, table);
      if (kSQLReservedWords.find(colName) == kSQLReservedWords.end()) {
         query_ += colName + " = :" + bindName + ", ";
      }
      else {
         query_ += "\"" + colName + "\" = :" + bindName + ", ";
      }
   }
   query_.pop_back(), query_.pop_back();  // last ", " is not needed
   return *this;
}

QuerySQL& QuerySQL::Delete()
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (tables_.size() > 1) {
      throw Error("ambiguous table");
   }
   reset();
   query_ = "DELETE FROM " + std::string{ tables_.cbegin()->first };
   return *this;
}

static std::string sqlType(const std::string_view& t)
{
   auto type = std::string{ t };
   std::transform(type.cbegin(), type.cend(), type.begin()
      , [](unsigned char c) { return std::tolower(c); });
   if ((type == "int") || (type == "integer") || (type == "uint")) {
      return "INTEGER";
   }
   if ((type == "bigint") || (type == "biguint")) {
      return "BIGINT";
   }
   if (type == "float") {
      return "FLOAT";
   }
   if (type == "double") {
      return "DOUBLE";
   }
   if ((type == "string") || (type == "text")) {
      return "TEXT";
   }
   if (type == "blob") {
      return "BLOB";
   }
   if (type == "bool") {
      return "BOOLEAN";
   }
   return std::string{ t };
}

void QuerySQL::create(const std::string& op)
{
   if (tables_.empty()) {
      throw Error("no table assigned");
   }
   if (tables_.size() > 1) {
      throw Error("more than one table results in ambiguity");
   }
   const auto t = tables_.cbegin()->second;
   if (!t) {
      throw Error("can't create null table");
   }
   createTable(t, op);
}

void QuerySQL::createTable(const std::shared_ptr<Table>& t, const std::string& op)
{
   if (!t) {
      throw std::invalid_argument("table is null");
   }
   query_ = op + " " + std::string{ t->name() } + " (";
   for (const auto& col : t->columns()) {
      query_ += std::string{ col.name() } + " " + sqlType(col.type());
      if (col.has(Prop::Primary)) {
         query_ += " PRIMARY KEY";
      }
      if (col.has(Prop::NotNullable)) {
         query_ += " NOT NULL";
      }
      if (col.has(Prop::Unique)) {
         query_ += " UNIQUE";
      }
      query_ += ", ";
   }
   for (const auto& constraint : t->constraints()) {
      query_ += constraint.text() + ", ";
   }
   query_.pop_back(), query_.pop_back();
   query_ += ")";

   tables_[t->name()] = t;
}

void QuerySQL::drop()
{
   if (tables_.empty()) {
      throw Error("no table associated");
   }
   if (tables_.size() > 1) {
      throw Error("ambiguous table");
   }
   reset();
   query_ = "DROP TABLE " + std::string{ tables_.cbegin()->first };
}


std::pair<int, std::shared_ptr<Table>> Engine::findColumn(const ColumnId& column, const Tables& tables)
{
   int colIndex = -1;
   std::shared_ptr<Table> table;
   if (column.table().empty() && (tables.size() == 1)) {
      table = tables.cbegin()->second;
      if (table) {
         colIndex = table->findColIndex(column.column());
      }
   }
   else {
      const auto& itTable = tables.find(column.table());
      if (itTable == tables.end()) {
         throw Error("table " + column.table() + " not found");
      }
      colIndex = itTable->second->findColIndex(column.column());
      table = itTable->second;
   }
   if ((colIndex < 0) || !table) {
      throw Error("column " + column() + " not found");
   }
   if ((colIndex != INT_MAX) && (colIndex >= table->columns().size())) {
      throw Error("invalid " + std::string{ table->name() } + " column index "
         + std::to_string(colIndex));
   }
   return { colIndex, table };
}


bool SqlLock::beginTX()
{
   return engine_->execute("BEGIN TRANSACTION");
}

bool SqlLock::doCommit()
{
   return engine_->execute("COMMIT");
}

bool SqlLock::doRollback()
{
   return engine_->execute("ROLLBACK");
}
