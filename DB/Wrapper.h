#ifndef __CONVO_DB_WRAPPER_H__
#define __CONVO_DB_WRAPPER_H__

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include "base/SerDeser.h"

namespace convo {
   namespace db {

      class Error : public std::runtime_error
      {
      public:
         Error(const std::string& text) : std::runtime_error(text) {}
      };
      namespace error {
         class EngineNotOpen : public Error
         {
         public:
            EngineNotOpen() : Error("engine not opened") {}
         };

         class EngineNoQuery : public Error
         {
         public:
            EngineNoQuery() : Error("no query was executed") {}
         };

         class Syntax : public Error
         {
         public:
            Syntax(const std::string& desc) : Error("syntax error " + desc) {}
         };

         class SQLWhereSyntax : public Syntax
         {
         public:
            SQLWhereSyntax(const std::string& desc) : Syntax("in WHERE clause: " + desc) {}
         };

         class SQLSelectValidation : public Error
         {
         public:
            SQLSelectValidation(const std::string& desc) : Error("invalid SELECT: " + desc) {}
         };
      }

      class ColumnId
      {
      public:
         ColumnId() {}
         ColumnId(const char* colName) : colName_{ colName }, id_(colName) {}
         ColumnId(const std::string_view& colName) : colName_{ colName }, id_(colName) {}
         ColumnId(const std::string& colName) : colName_{ colName }, id_(colName) {}
         ColumnId(const std::string& tableName, const std::string& colName)
            : tableName_(tableName), colName_(colName), id_(tableName + "." + colName) {}
         ColumnId(const std::string_view& tableName, const std::string_view& colName)
            : tableName_(tableName), colName_(colName)
            , id_(std::string{ tableName } + "." + std::string{ colName })
         {}

         std::string table() const { return tableName_; }
         void setTable(const std::string& t) { tableName_ = t; }
         std::string column() const { return colName_; }
         std::string alias() const { return alias_; }
         /*virtual*/ std::string id() const { return id_; }
         std::string operator()() const { return id(); }
         bool operator==(const ColumnId& other) const
         {
            return (id() == other());
         }

         bool empty() const { return colName_.empty(); }
         void clear() { tableName_.clear(), colName_.clear(); }

      protected:
         std::string tableName_;
         std::string colName_;
         std::string id_;
         std::string alias_;
      };

      struct ColHash
      {
         std::size_t operator()(const ColumnId& k) const
         {
            return std::hash<std::string>()(k.id());
         }
      };

      class ColumnFunc : public ColumnId
      {
      public:
         ColumnFunc(const char* func, const char* colName)
            : ColumnId(colName), func_{ func }
         {
            id_ = func_ + "(" + colName_ + ")";
         }

      private:
         const std::string func_;
      };

      class ColumnSum : public ColumnFunc
      {
      public:
         ColumnSum(const char* colName) : ColumnFunc("SUM", colName) {}
      };
      class ColumnMax : public ColumnFunc
      {
      public:
         ColumnMax(const char* colName) : ColumnFunc("MAX", colName) {}
      };
      class ColumnMin : public ColumnFunc
      {
      public:
         ColumnMin(const char* colName) : ColumnFunc("MIN", colName) {}
      };
      class ColumnCount : public ColumnFunc
      {
      public:
         ColumnCount(const char* colName = "*")
            : ColumnFunc("COUNT", colName) {}
      };

      class ColumnAs : public ColumnId
      {
      public:
         ColumnAs(const ColumnId& col, const char* alias)
            : ColumnId(col)
         {
            alias_ = alias;
            id_ = col() + " AS " + alias_;
            colName_ = col.column();
         }
      };

      using Columns = std::vector<ColumnId>;
      class Table;
      using Tables = std::unordered_map<std::string, std::shared_ptr<Table>>;
      using BindDataMap = std::map<std::string, std::pair<ColumnId, ser::Type>>;

      class DataMap : public std::unordered_map<ColumnId, ser::Type, ColHash>
      {
      public:
         DataMap() {}
         DataMap(const DataMap& other) { *this = other; }

         DataMap(const ser::Struct& other)
         {
            for (const auto& field : other) {
               insert({field.first, field.second});
            }
         }

         bool has(const ColumnId& key) const
         {
            return (find(key) != end());
         }
      };

      bool has(const Columns&, const ColumnId&);

      template <typename T>
      T get(const DataMap& m, const ColumnId& key)
      {  //only ordinary types, complex ones are supported via partial specialization
         try {
            const auto& v = m.at(key);
            if (std::holds_alternative<uint8_t>(v)) {
               return static_cast<T>(std::get<uint8_t>(v));
            } else if (std::holds_alternative<int32_t>(v)) {
               return static_cast<T>(std::get<int32_t>(v));
            } else if (std::holds_alternative<uint32_t>(v)) {
               return static_cast<T>(std::get<uint32_t>(v));
            } else if (std::holds_alternative<int16_t>(v)) {
               return static_cast<T>(std::get<int16_t>(v));
            } else if (std::holds_alternative<uint16_t>(v)) {
               return static_cast<T>(std::get<uint16_t>(v));
            } else if (std::is_same<T, bool>::value && std::holds_alternative<bool>(v)) {
               return (T)std::get<bool>(v);
            } else if (std::holds_alternative<int64_t>(v)) {
               return static_cast<T>(std::get<int64_t>(v));
            } else if (std::holds_alternative<uint64_t>(v)) {
               return static_cast<T>(std::get<uint64_t>(v));
            } else if ((std::is_same<T, float>::value || std::is_same<T, double>::value) && std::holds_alternative<float>(v)) {
               return static_cast<T>(std::get<float>(v));
            } else if ((std::is_same<T, float>::value || std::is_same<T, double>::value) && std::holds_alternative<double>(v)) {
               return static_cast<T>(std::get<double>(v));
            } else if (std::is_same<T, FixedDecimalValue>::value ||
               std::is_same<T, bs::ser::BinaryData>::value) {
               return T{}; //unable to retrive compound type
            }
            else {
               throw Error("unknown type");
            }
         }
         catch (const Error& e) {
            throw Error("get " + key() + " failure: " + e.what());
         }
         catch (const std::exception&) {}
         return T{};
      }

      std::string get(const DataMap&, const ColumnId&);
      template<> std::string get(const DataMap&, const ColumnId&);

      std::string toString(const DataMap&);
      std::string toString(const ser::Type&);
      ser::Struct toStruct(const DataMap&);

      using Data = std::vector<ser::Struct>;

      enum class Prop : uint8_t
      {
         Primary,
         NotNullable,
         Unique,
         AutoIncrement
      };

      class Column
      {
      public:
         Column() : name_{}, type_{}, flags_{} {}
         Column(const std::string_view& name, const std::string& type, const std::set<Prop>& flags = {})
            : name_(name), type_(type), flags_(flags) {}
         Column& operator=(const Column& c)
         {
            name_ = c.name_;
            type_ = c.type_;
            flags_ = c.flags_;
            return *this;
         }
         std::string_view name() const { return name_; }
         std::string type() const { return type_; }
         bool has(Prop p) const { return (flags_.find(p) != flags_.end()); }
         bool empty() const { return (type_.empty() || name_.empty()); }
         size_t maxLength() const { return maxLength_; }

      protected:
         size_t   maxLength_{ SIZE_MAX };

      private:
         std::string_view name_;
         std::string type_;
         std::set<Prop> flags_;
      };

      namespace column {
         class Int : public Column
         {
         public:
            Int(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "Int", flags)
            { maxLength_ = sizeof(int); }
         };

         class UInt : public Column
         {
         public:
            UInt(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "UInt", flags)
            { maxLength_ = sizeof(unsigned); }
         };

         class BigInt : public Column
         {
         public:
            BigInt(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "BigInt", flags)
            { maxLength_ = sizeof(long long); }
         };

         class Bool : public Column
         {
         public:
            Bool(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "Bool", flags)
            { maxLength_ = sizeof(bool); }
         };

         class Timestamp : public Column
         {
         public:
            Timestamp(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "BigUInt", flags)
            { maxLength_ = sizeof(unsigned long long); }
         };

         class Float : public Column
         {
         public:
            Float(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "Float", flags)
            { maxLength_ = sizeof(float); }
         };

         class Double : public Column
         {
         public:
            Double(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "Double", flags)
            { maxLength_ = sizeof(double); }
         };

         class String : public Column
         {
         public:
            String(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "String", flags) {}
         };

         class VarChar : public Column
         {
         public:
            VarChar(const std::string_view& name, unsigned len, const std::set<Prop>& flags = {})
               : Column(name, fmt::format("VARCHAR({})", len), flags)
            { maxLength_ = len; }
         };

         class BLOB : public Column
         {
         public:
            BLOB(const std::string_view& name, const std::set<Prop>& flags = {})
               : Column(name, "BLOB", flags) {}
         };
      }

      class Constraint
      {
      public:
         Constraint(const std::string_view& name) : name_(name) {}
         virtual ~Constraint() = default;

         virtual std::string text() const
         {
            return "CONSTRAINT " + std::string{ name_ };
         }

      protected:
         const std::string_view name_;
      };


      class Table
      {
      public:
         Table(const std::string& name) : name_(name) {}
         virtual ~Table() = default;
         Table(const Table&) = default;
         std::string name() const { return name_; }

         virtual std::vector<Column> columns() const = 0;
         virtual std::vector<Constraint> constraints() const { return {}; }

         int findColIndex(const std::string& colName) const;
         bool empty() const { return (columns().empty() || name_.empty()); }

         Table& operator=(const Table& t) {
            name_ = t.name_;
            return *this;
         }

      private:
         std::string name_;
      };

      class VersionTable : public Table   //pre-defined alembic version table to track schema
      {
      public:
         VersionTable() : bs::db::Table("alembic_version") {}
         std::vector<Column> columns() const override
         {
            return {
               column::String("version_num")
            };
         }
      };

      class ForeignKey : public Constraint
      {
      public:
         ForeignKey(const std::string_view& name, const ColumnId& ownCol
            , const Table& t, const ColumnId& foreignCol)
            : Constraint(name), ownCol_(ownCol), foreignCol_(foreignCol)
            , foreignTableName_(t.name()) {}

         virtual std::string text() const
         {
            return Constraint::text() + " FOREIGN KEY (" + ownCol_.column() +
               ") REFERENCES " + std::string{ foreignTableName_ } + " (" +
               foreignCol_.column() + ")";
         }

      private:
         const ColumnId ownCol_, foreignCol_;
         const std::string_view  foreignTableName_;
      };

      class Lock
      {
      public:
         virtual ~Lock() = default;
         virtual bool commit() = 0;
         virtual bool rollback() = 0;
      };

      enum class Feature : int
      {
         unknown,
         transactions,
         BLOB,
         batch_ops,
         simple_locking
      };

      class Query;
      class Engine
      {
      public:
         virtual bool isOpen() const = 0;
         virtual bool open() = 0;
         virtual bool close() = 0;
         virtual std::unordered_set<std::string> tables() const = 0;

         virtual bool execute(const Query*) = 0;
         virtual bool execute(const std::string&) = 0;
         virtual Data fetch(const Query*) = 0;
         virtual std::string lastError() const = 0;
         virtual std::unique_ptr<Lock> lock() = 0;
         virtual bool hasFeature(const Feature) const = 0;

      protected:
         static std::pair<int, std::shared_ptr<Table>> findColumn(const ColumnId&, const Tables&);
      };

      class SqlLock : public Lock
      {
      public:
         SqlLock(Engine* engine) : engine_(engine)
         {
            if (!engine_) {
               throw Error("invalid DB engine");
            }
            if (!beginTX()) {
               throw Error("can't start TX");
            }
         }
         ~SqlLock() override { rollback(); }

         bool commit() override
         {
            if (!completed_) {
               return (completed_ = doCommit());
            }
            return false;
         }

         bool rollback() override
         {
            if (!completed_) {
               return (completed_ = doRollback());
            }
            return true;
         }

      private:
         bool beginTX();
         bool doCommit();
         bool doRollback();

      private:
         Engine* engine_{ nullptr };
         bool  completed_{ false };
      };


      class Query
      {
      public:
         Query() = default;
         virtual std::string text() const = 0;
         virtual BindDataMap bindVals() const = 0;
         virtual Columns columns() const = 0;
         virtual Tables tables() const = 0;
         virtual bool fetchable() const = 0;

         enum class FilterResult : uint8_t
         {
            pass,
            ignore,
            aggregate
         };
         virtual FilterResult filter(ser::Struct&) const = 0;
      };

      class QueryRawSQL : public Query
      {
      public:
         explicit QueryRawSQL(const std::string& query, bool fetchable = false)
            : query_(query), isSelect_(fetchable) {}
         QueryRawSQL() = default;
         
         std::string text() const override { return query_; }
         FilterResult filter(ser::Struct&) const override { return FilterResult::pass; }
         BindDataMap bindVals() const override { return {}; }
         Columns columns() const override { return {}; }
         Tables tables() const override { return { {{}, nullptr} }; }
         bool fetchable() const override { return isSelect_; }

      protected:
         std::string query_;
         bool isSelect_{ false };
      };

      class QuerySQL : public QueryRawSQL
      {
      public:
         QuerySQL(const std::shared_ptr<Table>& table);
         QuerySQL(const QuerySQL&, const std::string_view& alias);
         QuerySQL() : whereStarted_(true) {}
         QuerySQL(const QuerySQL& q) = default;

         QuerySQL& select(const std::vector<ColumnId>&);
         QuerySQL& where();
         QuerySQL& where(const ColumnId&);
         QuerySQL& isTrue();
         QuerySQL& True();
         QuerySQL& like(const std::string&);
         QuerySQL& eq(const ser::Type&);
         QuerySQL& eqCol(const ColumnId&);
         QuerySQL& eqVal(int64_t);
         QuerySQL& eqVal(int);
         QuerySQL& eqVal(const std::string&);
         QuerySQL& eqVal(bool);
         QuerySQL& isTrue(bool);
         QuerySQL& ne(const ser::Type&);

         template<typename C>
         QuerySQL& in(const C& c, const std::string& op = "IN")
         {
            std::string val = "(";
            for (auto item : c) {
               if constexpr (std::is_same<decltype(item), std::string>::value)
                  val += "\"" + item + "\", ";
               else if constexpr (std::is_same<decltype(item), const char*>::value)
                  val += "\"" + std::string(item) + "\", ";
               else if constexpr (std::is_arithmetic<decltype(item)>::value)
                  val += std::to_string(item) + ", ";
               else val += "???, ";
            }
            if (!c.empty()) {
               val.pop_back(), val.pop_back();
            }
            val += ")";
            return compare(val, op);
         }

         template<typename C>
         QuerySQL& notIn(const C& c)
         {
            return in(c, "NOT IN");
         }
         QuerySQL& neVal(int64_t);
         QuerySQL& ge(const ser::Type&);
         QuerySQL& gt(const ser::Type&);
         QuerySQL& gtVal(int64_t);
         QuerySQL& le(const ser::Type&);
         QuerySQL& lt(const ser::Type&);
         QuerySQL& And();
         QuerySQL& And(const ColumnId&);
         QuerySQL& Or();
         QuerySQL& Or(const ColumnId&);
         QuerySQL& orderBy(const std::vector<ColumnId>&, bool ascending = true);
         QuerySQL& groupBy(const std::vector<ColumnId>&);
         QuerySQL& limit(size_t);
         QuerySQL& offset(size_t);
         QuerySQL& column(const ColumnId&);
         QuerySQL& subExpression(const QuerySQL&);
         QuerySQL& join(const std::shared_ptr<Table>&, const std::string& op = "JOIN");
         QuerySQL& innerJoin(const std::shared_ptr<Table>& t) { return join(t, "INNER JOIN"); }
         QuerySQL& leftJoin(const std::shared_ptr<Table>&);
         QuerySQL& join(const QuerySQL&, const std::string_view& alias = {}
         , const std::string& op = "JOIN");
         QuerySQL& leftJoin(const QuerySQL&, const std::string_view& alias = {});
         QuerySQL& on(const ColumnId& col1, const ColumnId& col2 = {});
         void insert(const DataMap&, const std::string& op = "INSERT");
         void insertOrIgnore(const DataMap& dm) { return insert(dm, "INSERT OR IGNORE"); }
         void insertOrReplace(const DataMap& dm) { return insert(dm, "INSERT OR REPLACE"); }
         QuerySQL& update(const DataMap&);
         QuerySQL& Delete();
         void create(const std::string& op = "CREATE TABLE");
         void createTable(const std::shared_ptr<Table>&, const std::string& op = "CREATE TABLE");
         void createIfNotExists(const std::shared_ptr<Table>& t)
         {
            createTable(t, "CREATE TABLE IF NOT EXISTS");
         }
         void drop();

         BindDataMap bindVals() const override { return bindVals_; }
         Columns columns() const override { return columns_; }
         Tables tables() const override { return tables_; }

      protected:
         QuerySQL& conj(const std::string& word);
         QuerySQL& conj(const ColumnId&, const std::string& word);
         QuerySQL& opVal(const std::string& op, const std::string& val);
         QuerySQL& compare(const ser::Type&, const std::string& op, bool isNumeric = true);
         QuerySQL& compare(const std::string& val, const std::string& op);
         QuerySQL& compare(const ColumnId&, const std::string& op);
         std::string setBindVal(const std::pair<ColumnId, ser::Type>& bindVal
            , const std::shared_ptr<Table>&);
         std::string boundText(const QuerySQL&, const std::string& alias);
         void reset();

      protected:
         Tables tables_;
         std::string subQuery_;
         BindDataMap bindVals_{};
         Columns  columns_{};
         ColumnId currentCol_{};
         std::unordered_map<std::string_view, std::unordered_map<std::string, std::string>> colAliases_;
         bool whereStarted_{ false };
         bool onStarted_{ false };
      };

   }
}

#endif	//__CONVO_DB_WRAPPER_H__
