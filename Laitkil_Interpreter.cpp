// =============================================================================
//  Laitkil — интерпретатор в одном файле (C++17)
//
//    1. Ошибки, типы, значения      5. Байткод и таблица встроенных функций
//    2. Лексер                      6. Компилятор AST -> регистровый байткод
//    3. AST                         7. Виртуальная машина + стандартная библиотека
//    4. Парсер                      8. Дизассемблер
//    9. LKLBridge - API для встраивания (см. LKLBridge.h)       10. main()
// =============================================================================
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LKLBridge.h"
#include "LKLBridge.hpp"

// ============================================================================
// 1. Ошибки, типы, значения
// ============================================================================
struct LkError { std::string kind; int line; std::string msg; };
[[noreturn]] static void fail(const char* kind, int line, const std::string& msg) {
  throw LkError{kind, line, msg};
}
struct LkExit { int code; };
struct RtErr { std::string msg; explicit RtErr(std::string m) : msg(std::move(m)) {} };

using Type = std::string;
static bool isArr(const Type& t)    { return t.size() > 2 && t[0] == 'a' && t[1] == '_'; }
static bool isDict(const Type& t)   { return t.size() > 2 && t[0] == 'd' && t[1] == '_'; }
static Type elemOf(const Type& t)   { return t.substr(2); }
static bool isScalar(const Type& t) { return t == "num" || t == "bool" || t == "str"; }

enum class VT : uint8_t { Nil, Num, Bool, Str, Arr, Dict, Obj };

// Пул: освобождённые объекты кучи (строки/массивы/словари/экземпляры) возвращаются в свободные списки
// по размерным классам по 16 байт и переиспользуются без обращения к malloc.
static thread_local void* gHeapFree[9] = {};
struct Heap {
  int rc = 0;
  virtual ~Heap() = default;
  static void* operator new(size_t n) {
    size_t c = (n + 15) / 16;
    if (c < 9) {
      if (void* p = gHeapFree[c]) { gHeapFree[c] = *(void**)p; return p; }
      return ::operator new(c * 16);
    }
    return ::operator new(n);
  }
  static void operator delete(void* p, size_t n) {
    size_t c = (n + 15) / 16;
    if (c < 9) { *(void**)p = gHeapFree[c]; gHeapFree[c] = p; }
    else ::operator delete(p);
  }
};
struct StrObj; struct ArrObj; struct DictObj; struct InstObj;

struct Value {
  VT t = VT::Nil;
  union { double n; bool b; Heap* h; uint64_t bits; };
  Value() : bits(0) {}
  Value(const Value& o) : t(o.t), bits(o.bits) { if (heap()) h->rc++; }
  Value(Value&& o) noexcept : t(o.t), bits(o.bits) { o.t = VT::Nil; }
  __attribute__((always_inline)) Value& operator=(const Value& o) {
    VT nt = o.t; uint64_t nb = o.bits;
    if (o.heap()) o.h->rc++;
    if (heap()) release();          // быстрый путь: число/bool поверх числа - без вызова
    t = nt; bits = nb;
    return *this;
  }
  __attribute__((always_inline)) Value& operator=(Value&& o) noexcept {
    if (this == &o) return *this;
    VT nt = o.t; uint64_t nb = o.bits; o.t = VT::Nil;
    if (heap()) release();
    t = nt; bits = nb;
    return *this;
  }
  ~Value() { release(); }
  bool heap() const { return t >= VT::Str; }
  __attribute__((always_inline)) void release() { if (heap() && --h->rc == 0) delete h; }
  void setNum(double d) { release(); t = VT::Num; n = d; }
  void setBool(bool v) { release(); t = VT::Bool; bits = 0; b = v; }
  static Value num(double d) { Value v; v.t = VT::Num; v.n = d; return v; }
  static Value boolean(bool x) { Value v; v.t = VT::Bool; v.bits = 0; v.b = x; return v; }
  StrObj* str() const; ArrObj* arr() const; DictObj* dict() const; InstObj* inst() const;
};

struct StrObj  : Heap { std::string s; };
struct ArrObj  : Heap { std::vector<Value> v; };
struct InstObj : Heap { int bp = 0; std::vector<Value> f; };
struct DictObj : Heap {
  std::vector<std::pair<std::string, Value>> items;
  std::unordered_map<std::string, size_t> index;
  Value* find(const std::string& k) {
    auto it = index.find(k);
    return it == index.end() ? nullptr : &items[it->second].second;
  }
  void set(const std::string& k, const Value& v) {
    auto it = index.find(k);
    if (it != index.end()) items[it->second].second = v;
    else { index.emplace(k, items.size()); items.emplace_back(k, v); }
  }
  bool remove(const std::string& k) {
    auto it = index.find(k);
    if (it == index.end()) return false;
    size_t pos = it->second;
    items.erase(items.begin() + (std::ptrdiff_t)pos);
    index.erase(it);
    for (size_t i = pos; i < items.size(); i++) index[items[i].first] = i;
    return true;
  }
  void clear() { items.clear(); index.clear(); }
};

inline StrObj*  Value::str()  const { return static_cast<StrObj*>(h); }
inline ArrObj*  Value::arr()  const { return static_cast<ArrObj*>(h); }
inline DictObj* Value::dict() const { return static_cast<DictObj*>(h); }
inline InstObj* Value::inst() const { return static_cast<InstObj*>(h); }

static Value wrapHeap(VT t, Heap* o) { o->rc = 1; Value v; v.t = t; v.h = o; return v; }
static Value mkStr(std::string s) { auto* o = new StrObj; o->s = std::move(s); return wrapHeap(VT::Str, o); }
static Value mkArr()  { return wrapHeap(VT::Arr, new ArrObj); }
static Value mkDict() { return wrapHeap(VT::Dict, new DictObj); }
static Value mkInst(int bp) { auto* o = new InstObj; o->bp = bp; return wrapHeap(VT::Obj, o); }

struct Program;

static Value defaultValue(const Type& t, const Program* P = nullptr);

// ============================================================================
// 2. Лексер
// ============================================================================
enum class Tk { Num, Str, Id, Kw, Sym, Eof };
struct Token {
  Tk k = Tk::Eof; std::string s; double n = 0; int line = 0;
  bool nl = false;
};

static const std::unordered_set<std::string> KEYWORDS = {
  "if", "elif", "else", "while", "from", "to", "iter_a", "iter_d", "func", "give", "next", "quit",
  "blueprint", "access", "unaccess", "new", "equals", "remove_a", "remove_d", "clear_a", "clear_d",
  "add", "append", "prepend",
  "and", "or", "not", "true", "false", "paste",
  "states"};

static constexpr int MAX_PASTE_DEPTH = 64;

static std::string resolvePaste(const std::string& fname, const std::string& baseDir) {
  std::filesystem::path p(fname);
  if (!p.is_absolute() && !baseDir.empty()) {
    std::filesystem::path candidate = std::filesystem::path(baseDir) / p;
    std::ifstream f(candidate.string().c_str(), std::ios::binary);
    if (f) return candidate.string();
  }
  return p.string();
}

static void lexInto(const std::string& s, const std::string& baseDir,
                    std::vector<Token>& out, int& line, bool& nl,
                    std::unordered_set<std::string>& active, int depth) {
  size_t i = 0, n = s.size();
  if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
  auto push = [&](Tk k, std::string text, double num = 0) {
    Token t; t.k = k; t.s = std::move(text); t.n = num; t.line = line; t.nl = nl;
    out.push_back(std::move(t)); nl = false;
  };
  auto scanStr = [&](size_t start, size_t& after, std::string& text) -> bool {
    text.clear();
    size_t j = start + 1;
    while (j < n) {
      char d = s[j];
      if (d == '"') { after = j + 1; return true; }
      if (d == '\n') return false;
      if (d == '\\' && j + 1 < n) {
        char e = s[j + 1]; j += 2;
        switch (e) {
          case 'n': text += '\n'; break;
          case 't': text += '\t'; break;
          case 'r': text += '\r'; break;
          case '0': text += '\0'; break;
          case '"': text += '"'; break;
          case '\\': text += '\\'; break;
          default: fail("Syntax", line, std::string("unknown escape sequence '\\") + e + "'");
        }
        continue;
      }
      text += d; j++;
    }
    return false;
  };
  static const std::unordered_set<std::string> two = {
    "==", "!=", "<=", ">=", "++", "+=", "-=", "*=", "/=", "%=", "->"};
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n') { line++; nl = true; i++; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == '#') {
      if (i + 1 < n && s[i + 1] == '#') {
        int start = line; i += 2;
        while (i < n && !(s[i] == '#' && i + 1 < n && s[i + 1] == '#')) {
          if (s[i] == '\n') { line++; nl = true; }
          i++;
        }
        if (i >= n) fail("Syntax", start, "unterminated block comment (## ... ##)");
        i += 2;
      } else {
        while (i < n && s[i] != '\n') i++;
      }
      continue;
    }
    if (isdigit(c)) {
      size_t j = i;
      while (j < n && isdigit((unsigned char)s[j])) j++;
      if (j + 1 < n && s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
        j++;
        while (j < n && isdigit((unsigned char)s[j])) j++;
      }
      if (j < n && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) k++;
        if (k < n && isdigit((unsigned char)s[k])) {
          while (k < n && isdigit((unsigned char)s[k])) k++;
          j = k;
        }
      }
      std::string text = s.substr(i, j - i);
      push(Tk::Num, text, strtod(text.c_str(), nullptr));
      i = j; continue;
    }
    if (isalpha(c) || c == '_' || c >= 0x80) {
      size_t j = i;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || (unsigned char)s[j] >= 0x80)) j++;
      std::string text = s.substr(i, j - i);

      if (text == "paste") {
        size_t k = j;
        while (k < n && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r')) k++;
        if (k >= n || s[k] != '"')
          fail("Syntax", line, "'paste' must be followed by a file name in double quotes: paste \"file.lkl\"");
        size_t after = 0; std::string fname;
        if (!scanStr(k, after, fname))
          fail("Syntax", line, "unterminated string literal");
        if (fname.empty()) fail("Syntax", line, "paste: empty file name");
        if (depth >= MAX_PASTE_DEPTH)
          fail("Syntax", line, "paste: nesting is too deep (limit " + std::to_string(MAX_PASTE_DEPTH) + ")");

        std::string resolved = resolvePaste(fname, baseDir);
        std::error_code ec;
        std::string canon = std::filesystem::weakly_canonical(std::filesystem::path(resolved), ec).string();
        if (ec || canon.empty()) canon = resolved;
        if (active.count(canon))
          fail("Syntax", line, "paste: circular paste, '" + fname + "' is already being pasted");

        std::ifstream f(resolved.c_str(), std::ios::binary);
        if (!f) fail("File", line, "paste: cannot open '" + fname + "'");
        std::stringstream ss; ss << f.rdbuf();
        std::string sub = ss.str();

        std::string newBase = std::filesystem::path(canon).parent_path().string();
        active.insert(canon);
        int subLine = 1;
        lexInto(sub, newBase, out, subLine, nl, active, depth + 1);
        active.erase(canon);
        i = after; continue;
      }
      push(KEYWORDS.count(text) ? Tk::Kw : Tk::Id, text);
      i = j; continue;
    }
    if (c == '"') {
      size_t after = 0; std::string text;
      if (!scanStr(i, after, text)) fail("Syntax", line, "unterminated string literal");
      push(Tk::Str, text);
      i = after; continue;
    }
    if (s.compare(i, 3, "++=") == 0) { push(Tk::Sym, "++="); i += 3; continue; }
    if (i + 1 < n && two.count(s.substr(i, 2))) { push(Tk::Sym, s.substr(i, 2)); i += 2; continue; }
    if (c != 0 && strchr("(){}[],:.=<>+-*/%", c)) { push(Tk::Sym, std::string(1, (char)c)); i++; continue; }
    fail("Syntax", line, std::string("unexpected character '") + (char)c + "'");
  }
}

static std::vector<Token> lex(const std::string& s, const std::string& baseDir = "") {
  std::vector<Token> out;
  int line = 1; bool nl = false;
  std::unordered_set<std::string> active;
  lexInto(s, baseDir, out, line, nl, active, 0);
  Token eof; eof.k = Tk::Eof; eof.line = line; eof.nl = nl;
  out.push_back(std::move(eof));
  return out;
}

// ============================================================================
// 3. AST
// ============================================================================
enum class EK { Num, Str, Bool, Ident, ArrLit, DictLit, Unary, Binary, Index, Member, Call, MCall };
struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Expr {
  EK k = EK::Num; int line = 0;
  double num = 0;
  std::string s;
  std::vector<ExprP> kids;
};

enum class SK { Decl, New, Assign, ExprS, If, Equals, While, From, IterA, IterD, Give, Next, Quit,
                RemoveA, RemoveD, ClearA, ClearD,
                Add, Append, Prepend,
                Func, Blueprint, States };
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
using Block = std::vector<StmtP>;
struct Param { Type type; std::string name; };
struct Case { std::vector<ExprP> values; Block body; };
struct Field { Type type; std::string name; bool priv; int line; };
struct Stmt {
  SK k = SK::ExprS; int line = 0;
  Type type;
  std::string name, name2;
  ExprP e1, e2, e3;
  std::vector<ExprP> conds; std::vector<Block> blocks;
  Block body; bool hasElse = false;
  std::vector<Case> cases;
  std::vector<Param> params; bool priv = false;
  std::vector<Field> fields; std::vector<StmtP> methods;
  StmtP ctor;
  std::vector<ExprP> args; bool hasArgs = false;
  std::vector<std::string> variants;
};

// ============================================================================
// 4. Парсер (рекурсивный спуск)
// ============================================================================
class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t(std::move(toks)) {}
  Block parseProgram() {
    Block prog;
    while (cur().k != Tk::Eof) prog.push_back(parseStmt(true));
    return prog;
  }

 private:
  std::vector<Token> t; size_t p = 0;

  const Token& cur() const { return t[p]; }
  const Token& at(size_t k) const { return t[std::min(p + k, t.size() - 1)]; }
  void next() { if (p + 1 < t.size()) p++; }
  bool sym(const char* s) const { return cur().k == Tk::Sym && cur().s == s; }
  bool kw(const char* s) const { return cur().k == Tk::Kw && cur().s == s; }
  bool acceptSym(const char* s) { if (sym(s)) { next(); return true; } return false; }
  bool acceptKw(const char* s) { if (kw(s)) { next(); return true; } return false; }
  [[noreturn]] void err(const std::string& m) { fail("Syntax", cur().line, m); }
  std::string describe() const {
    switch (cur().k) {
      case Tk::Eof: return "end of file";
      case Tk::Str: return "a string";
      case Tk::Num: return "a number";
      default: return "'" + cur().s + "'";
    }
  }
  void expectSym(const char* s) {
    if (!acceptSym(s)) err(std::string("expected '") + s + "', found " + describe());
  }
  std::string expectId(const char* what) {
    if (cur().k != Tk::Id) err(std::string("expected ") + what + ", found " + describe());
    std::string s = cur().s; next(); return s;
  }
  StmtP mk(SK k) { auto s = std::make_unique<Stmt>(); s->k = k; s->line = cur().line; return s; }
  static ExprP mkE(EK k, int line) { auto e = std::make_unique<Expr>(); e->k = k; e->line = line; return e; }

  Block parseBlock() {
    expectSym("{");
    Block b;
    while (!sym("}")) {
      if (cur().k == Tk::Eof) err("unexpected end of file: missing '}'");
      b.push_back(parseStmt(false));
    }
    expectSym("}");
    return b;
  }

  StmtP parseStmt(bool top) {
    if (cur().k == Tk::Kw) {
      const std::string k = cur().s;
      if (k == "if") return parseIf();
      if (k == "while") {
        auto s = mk(SK::While); next();
        s->e1 = parseExpr(); s->body = parseBlock(); return s;
      }
      if (k == "from") {
        auto s = mk(SK::From); next();
        expectSym("("); s->name = expectId("loop variable"); expectSym("=");
        s->e1 = parseExpr(); expectSym(")");
        if (!acceptKw("to")) err("expected 'to' after 'from (...)'");
        expectSym("(");
        s->e2 = parseExpr();
        if (!acceptSym(",")) err("'from' loop needs both an end value and a step: to (end, step)");
        s->e3 = parseExpr(); expectSym(")");
        s->body = parseBlock(); return s;
      }
      if (k == "iter_a") {
        auto s = mk(SK::IterA); next();
        expectSym("("); s->name = expectId("element name"); expectSym(",");
        s->e1 = parseExpr(); expectSym(")"); s->body = parseBlock(); return s;
      }
      if (k == "iter_d") {
        auto s = mk(SK::IterD); next();
        expectSym("("); s->name = expectId("key name"); expectSym(",");
        s->name2 = expectId("element name"); expectSym(",");
        s->e1 = parseExpr(); expectSym(")"); s->body = parseBlock(); return s;
      }
      if (k == "func") {
        if (!top) err("functions can only be declared at the top level");
        return parseFunc(false);
      }
      if (k == "blueprint") {
        if (!top) err("blueprints can only be declared at the top level");
        return parseBlueprint();
      }
      if (k == "states") {
        if (!top) err("states can only be declared at the top level");
        return parseStates();
      }
      if (k == "give") {
        auto s = mk(SK::Give); next();
        if (!cur().nl && !sym("}") && cur().k != Tk::Eof) s->e1 = parseExpr();
        return s;
      }
      if (k == "next") { auto s = mk(SK::Next); next(); return s; }
      if (k == "quit") { auto s = mk(SK::Quit); next(); return s; }
      if (k == "new") {
        auto s = mk(SK::New); next();
        s->type = expectId("blueprint name"); s->name = expectId("variable name");
        if (sym("(") && !cur().nl) {
          next(); s->hasArgs = true;
          if (!sym(")")) {
            do { s->args.push_back(parseExpr()); } while (acceptSym(","));
          }
          expectSym(")");
        } else if (sym("=")) {
          next();
          s->e1 = parseExpr();
        }
        return s;
      }
      if (k == "add" || k == "append" || k == "prepend") {
        auto s = mk(k == "add" ? SK::Add : k == "append" ? SK::Append : SK::Prepend);
        next();
        s->e1 = parseExpr();
        s->e2 = parseExpr();
        return s;
      }
      if (k == "remove_a" || k == "remove_d" || k == "clear_a" || k == "clear_d") {
        auto s = mk(k == "remove_a" ? SK::RemoveA : k == "remove_d" ? SK::RemoveD
                    : k == "clear_a" ? SK::ClearA : SK::ClearD);
        next(); s->e1 = parseExpr(); return s;
      }
      err("unexpected keyword '" + k + "'");
    }
    if (cur().k == Tk::Id && at(1).k == Tk::Id && !at(1).nl) {
      auto s = mk(SK::Decl);
      s->type = cur().s; next(); s->name = cur().s; next();
      if (acceptSym("=")) s->e1 = parseExpr();
      return s;
    }
    auto s = mk(SK::ExprS);
    ExprP e = parseExpr();
    static const std::set<std::string> assignOps = {"=", "+=", "-=", "*=", "/=", "%=", "++="};
    if (cur().k == Tk::Sym && assignOps.count(cur().s)) {
      if (e->k != EK::Ident && e->k != EK::Index && e->k != EK::Member)
        fail("Syntax", e->line, "invalid assignment target");
      s->k = SK::Assign; s->name2 = cur().s; next();
      s->e1 = std::move(e); s->e2 = parseExpr();
    } else {
      if (e->k != EK::Call && e->k != EK::MCall)
        fail("Syntax", e->line, "expression has no effect: expected a function call or an assignment");
      s->e1 = std::move(e);
    }
    return s;
  }

  StmtP parseIf() {
    auto s = mk(SK::If); next();
    ExprP cond = parseExpr();
    if (acceptKw("equals")) {
      s->k = SK::Equals; s->e1 = std::move(cond);
      expectSym("{");
      while (!sym("}")) {
        if (cur().k == Tk::Eof) err("unexpected end of file: missing '}'");
        if (acceptKw("else")) { s->hasElse = true; s->body = parseBlock(); continue; }
        Case c;
        c.values.push_back(parseConcat());
        while (acceptKw("or")) c.values.push_back(parseConcat());
        c.body = parseBlock();
        s->cases.push_back(std::move(c));
      }
      expectSym("}");
      return s;
    }
    s->conds.push_back(std::move(cond)); s->blocks.push_back(parseBlock());
    while (kw("elif")) {
      next();
      s->conds.push_back(parseExpr()); s->blocks.push_back(parseBlock());
    }
    if (acceptKw("else")) { s->hasElse = true; s->body = parseBlock(); }
    return s;
  }

  StmtP parseFunc(bool priv) {
    auto s = mk(SK::Func); next();
    s->type = "void";
    s->name = expectId("function name");
    expectSym("(");
    if (!sym(")")) {
      do {
        Param pr; pr.type = expectId("parameter type"); pr.name = expectId("parameter name");
        s->params.push_back(pr);
      } while (acceptSym(","));
    }
    expectSym(")");
    if (acceptSym("->")) s->type = expectId("return type");
    s->priv = priv; s->body = parseBlock();
    return s;
  }

  StmtP parseStates() {
    auto s = mk(SK::States); next();
    s->name = expectId("states name");
    expectSym("{");
    if (!sym("}")) {
      do {
        s->variants.push_back(expectId("variant name"));
      } while (acceptSym(",") && !sym("}"));
    }
    expectSym("}");
    if (s->variants.empty())
      fail("Syntax", s->line, "states '" + s->name + "' must have at least one variant");
    return s;
  }

  bool atInit() const { return cur().k == Tk::Id && cur().s == "init" && at(1).k == Tk::Sym && at(1).s == "("; }
  void parseInit(Stmt* bp) {
    if (bp->ctor) err("blueprint '" + bp->name + "' already has an init");
    auto c = mk(SK::Func); next();
    c->name = "init"; c->type = "void"; c->priv = false;
    expectSym("(");
    if (sym(")")) err("init must take at least one argument: init(type name, ...)");
    do {
      Param pr; pr.type = expectId("parameter type"); pr.name = expectId("parameter name");
      c->params.push_back(pr);
    } while (acceptSym(","));
    expectSym(")");
    c->body = parseBlock();
    bp->ctor = std::move(c);
  }

  StmtP parseBlueprint() {
    auto s = mk(SK::Blueprint); next();
    s->name = expectId("blueprint name");
    expectSym("{");
    while (!sym("}")) {
      if (cur().k == Tk::Eof) err("unexpected end of file: missing '}'");
      if (atInit()) { parseInit(s.get()); continue; }
      bool priv = false;
      if (acceptKw("access")) priv = false;
      else if (acceptKw("unaccess")) priv = true;
      else err("expected 'access' or 'unaccess', found " + describe());
      expectSym("{");
      while (!sym("}")) {
        if (cur().k == Tk::Eof) err("unexpected end of file: missing '}'");
        if (kw("func")) { s->methods.push_back(parseFunc(priv)); continue; }
        if (atInit()) {
          if (priv) err("init cannot be declared in 'unaccess': put it in 'access' or directly in the blueprint");
          parseInit(s.get()); continue;
        }
        Field f; f.line = cur().line; f.priv = priv;
        f.type = expectId("field type"); f.name = expectId("field name");
        if (sym("=")) err("field initialisers are not supported; assign values in a method");
        s->fields.push_back(f);
      }
      expectSym("}");
    }
    expectSym("}");
    return s;
  }

  ExprP bin(const std::string& op, ExprP l, ExprP r, int line) {
    auto e = mkE(EK::Binary, line); e->s = op;
    e->kids.push_back(std::move(l)); e->kids.push_back(std::move(r));
    return e;
  }
  ExprP parseExpr() { return parseOr(); }
  ExprP parseOr() {
    ExprP l = parseAnd();
    while (kw("or")) { int ln = cur().line; next(); ExprP r = parseAnd(); l = bin("or", std::move(l), std::move(r), ln); }
    return l;
  }
  ExprP parseAnd() {
    ExprP l = parseNot();
    while (kw("and")) { int ln = cur().line; next(); ExprP r = parseNot(); l = bin("and", std::move(l), std::move(r), ln); }
    return l;
  }
  ExprP parseNot() {
    if (kw("not")) {
      auto e = mkE(EK::Unary, cur().line); e->s = "not"; next();
      e->kids.push_back(parseNot()); return e;
    }
    return parseCmp();
  }
  ExprP parseCmp() {
    ExprP l = parseConcat();
    while (cur().k == Tk::Sym && (cur().s == "==" || cur().s == "!=" || cur().s == "<" ||
                                  cur().s == ">" || cur().s == "<=" || cur().s == ">=")) {
      std::string op = cur().s; int ln = cur().line; next();
      ExprP r = parseConcat(); l = bin(op, std::move(l), std::move(r), ln);
    }
    return l;
  }
  ExprP parseConcat() {
    ExprP l = parseAdd();
    while (sym("++")) { int ln = cur().line; next(); ExprP r = parseAdd(); l = bin("++", std::move(l), std::move(r), ln); }
    return l;
  }
  ExprP parseAdd() {
    ExprP l = parseMul();
    while (sym("+") || sym("-")) {
      std::string op = cur().s; int ln = cur().line; next();
      ExprP r = parseMul(); l = bin(op, std::move(l), std::move(r), ln);
    }
    return l;
  }
  ExprP parseMul() {
    ExprP l = parseUnary();
    while (sym("*") || sym("/") || sym("%")) {
      std::string op = cur().s; int ln = cur().line; next();
      ExprP r = parseUnary(); l = bin(op, std::move(l), std::move(r), ln);
    }
    return l;
  }
  ExprP parseUnary() {
    if (sym("-")) {
      auto e = mkE(EK::Unary, cur().line); e->s = "-"; next();
      e->kids.push_back(parseUnary()); return e;
    }
    return parsePostfix();
  }
  void parseArgs(std::vector<ExprP>& out) {
    expectSym("(");
    if (!sym(")")) { do { out.push_back(parseExpr()); } while (acceptSym(",")); }
    expectSym(")");
  }
  ExprP parseMemberTail(ExprP e) {
    int ln = cur().line; next();
    std::string name = expectId("member name");
    if (sym("(") && !cur().nl) {
      auto x = mkE(EK::MCall, ln); x->s = name; x->kids.push_back(std::move(e));
      parseArgs(x->kids); return x;
    }
    auto x = mkE(EK::Member, ln); x->s = name; x->kids.push_back(std::move(e));
    return x;
  }
  ExprP parsePostfix() {
    ExprP e = parsePrimary();
    for (;;) {
      if (sym(":")) {
        int ln = cur().line; next();
        auto x = mkE(EK::Index, ln);
        x->kids.push_back(std::move(e)); x->kids.push_back(parseIndexAtom());
        e = std::move(x);
      } else if (sym(".")) {
        e = parseMemberTail(std::move(e));
      } else break;
    }
    return e;
  }
  ExprP parseIndexAtom() {
    if (sym("-")) {
      auto e = mkE(EK::Unary, cur().line); e->s = "-"; next();
      e->kids.push_back(parseIndexAtom()); return e;
    }
    ExprP e = parsePrimary();
    while (sym(".")) e = parseMemberTail(std::move(e));
    return e;
  }
  ExprP parsePrimary() {
    const Token& tk = cur();
    int ln = tk.line;
    if (tk.k == Tk::Num) { auto e = mkE(EK::Num, ln); e->num = tk.n; next(); return e; }
    if (tk.k == Tk::Str) { auto e = mkE(EK::Str, ln); e->s = tk.s; next(); return e; }
    if (kw("true") || kw("false")) { auto e = mkE(EK::Bool, ln); e->num = kw("true") ? 1 : 0; next(); return e; }
    if (tk.k == Tk::Id) {
      std::string name = tk.s; next();
      if (sym("(") && !cur().nl) {
        auto e = mkE(EK::Call, ln); e->s = name; parseArgs(e->kids); return e;
      }
      auto e = mkE(EK::Ident, ln); e->s = name; return e;
    }
    if (sym("(")) { next(); ExprP e = parseExpr(); expectSym(")"); return e; }
    if (sym("[")) {
      next();
      auto e = mkE(EK::ArrLit, ln);
      if (acceptSym("]")) return e;
      ExprP first = parseExpr();
      if (sym("=")) {
        e->k = EK::DictLit;
        next(); e->kids.push_back(std::move(first)); e->kids.push_back(parseExpr());
        while (acceptSym(",")) {
          if (sym("]")) break;
          e->kids.push_back(parseExpr()); expectSym("="); e->kids.push_back(parseExpr());
        }
      } else {
        e->kids.push_back(std::move(first));
        while (acceptSym(",")) {
          if (sym("]")) break;
          e->kids.push_back(parseExpr());
        }
      }
      expectSym("]");
      return e;
    }
    err("unexpected " + describe() + " in expression");
  }
};

// ============================================================================
// 5. Байткод и таблица встроенных функций
// ============================================================================
#define OP_LIST(X)                                                                  \
  X(LOADK) X(LOADNIL) X(MOVE) X(GETG) X(SETG)                                       \
  X(ADD) X(SUB) X(MUL) X(DIV) X(MOD) X(NEG) X(NOT) X(CONCAT)                        \
  X(EQ) X(NE) X(LT) X(LE) X(JMP) X(JMPF) X(JMPT)                                    \
  X(NEWARR) X(NEWDICT) X(APPEND) X(AINSERT) X(DSET) X(GETI) X(SETI) X(GETD) X(SETD) \
  X(AREMOVE) X(DREMOVE) X(ACLEAR) X(DCLEAR) X(ITERA) X(ITERD) X(FORLOOP) X(FORSTEP) \
  X(NEW) X(GETF) X(SETF) X(CALL) X(CALLB) X(CALLH) X(RET) X(RETV) \
  X(LOADK_ADD) X(LT_JMPF) X(MOVE_RET) \
  X(ADD_RR) X(ADD_RK) X(ADD_KR) X(SUB_RR) X(SUB_RK) X(SUB_KR) X(MUL_RR) X(MUL_RK) X(MUL_KR) \
  X(LT_JMPF_RR) X(LT_JMPF_RK) X(LT_JMPF_KR) X(CONCATG) X(NORET)

enum Op : uint8_t {
#define X(n) OP_##n,
  OP_LIST(X)
#undef X
  OP_COUNT
};
static const char* OP_NAMES[] = {
#define X(n) #n,
  OP_LIST(X)
#undef X
};

struct Instr {
  uint8_t op; int32_t a, b, c;
  int32_t d = 0;                  // 4-й операнд супер-инструкций (LT_JMPF: адрес перехода)
};
struct Proto {
  std::string name;
  std::vector<Instr> code;
  std::vector<int> lines;
  std::vector<Value> consts;
  int nparams = 0, maxRegs = 0;
};
struct BpRt { std::string name; std::vector<Type> fieldTypes; mutable std::vector<Value> tpl; mutable bool tplReady = false; };
struct StatesRt { std::string name; std::vector<std::string> variants; };
struct HostFn {
  std::string name; std::vector<Type> params; bool variadic = false; Type ret = "void";
  lkl_CFunction fn = nullptr; void* ud = nullptr; lkl_FreeFn freeUd = nullptr;
};
struct Program {
  std::deque<Proto> protos;
  std::vector<BpRt> bps;
  std::vector<StatesRt> states;
  std::vector<Type> globalTypes;
  std::deque<HostFn> hosts;
  std::unordered_map<std::string, int> hostByName;
};

static Value defaultValue(const Type& t, const Program* P) {
  if (t == "num") return Value::num(0);
  if (t == "bool") return Value::boolean(false);
  if (t == "str") return mkStr("");
  if (t == "json_str") return mkStr("");
  if (isArr(t)) return mkArr();
  if (isDict(t)) return mkDict();
  if (P) {
    for (auto& st : P->states)
      if (st.name == t && !st.variants.empty()) return mkStr(st.variants[0]);
  }
  return Value();
}

#define BUILTIN_LIST(X)                                                              \
  X(PRINT, "print", "any", "void", 1)                                                \
  X(WRITE, "write", "any", "void", 1)                                                \
  X(INPUT_STR, "$input_str", "", "str", 0)                                           \
  X(INPUT_NUM, "$input_num", "", "num", 0)                                           \
  X(INPUT_BOOL, "$input_bool", "", "bool", 0)                                        \
  X(EXIT, "exit", "num", "void", 0)                                                  \
  X(TO_STR, "to_str", "any", "str", 0)                                               \
  X(TO_NUM, "to_num", "str", "num", 0)                                               \
  X(IS_NUM, "is_num", "str", "bool", 0)                                              \
  X(ABS, "abs", "num", "num", 0)                                                     \
  X(FLOOR, "floor", "num", "num", 0)                                                 \
  X(CEIL, "ceil", "num", "num", 0)                                                   \
  X(ROUND, "round", "num", "num", 0)                                                 \
  X(SQRT, "sqrt", "num", "num", 0)                                                   \
  X(SIN, "sin", "num", "num", 0)                                                     \
  X(COS, "cos", "num", "num", 0)                                                     \
  X(TAN, "tan", "num", "num", 0)                                                     \
  X(ATAN, "atan", "num", "num", 0)                                                   \
  X(EXPF, "exp", "num", "num", 0)                                                    \
  X(LOG, "log", "num", "num", 0)                                                     \
  X(POW, "pow", "num num", "num", 0)                                                 \
  X(MIN, "min", "num num", "num", 0)                                                 \
  X(MAX, "max", "num num", "num", 0)                                                 \
  X(RANDOM, "random", "", "num", 0)                                                  \
  X(RANDOM_INT, "random_int", "num num", "num", 0)                                   \
  X(PI, "pi", "", "num", 0)                                                          \
  X(CLOCK, "clock", "", "num", 0)                                                    \
  X(TIME, "get_unix_time", "", "num", 0)                                             \
  X(LEN, "len", "cont", "num", 0)                                                    \
  X(UPPER, "upper", "str", "str", 0)                                                 \
  X(LOWER, "lower", "str", "str", 0)                                                 \
  X(TRIM, "trim", "str", "str", 0)                                                   \
  X(SUBSTR, "substr", "str num num", "str", 0)                                       \
  X(FIND, "find", "str str", "num", 0)                                               \
  X(REPLACE, "replace", "str str str", "str", 0)                                     \
  X(STARTS_WITH, "starts_with", "str str", "bool", 0)                                \
  X(ENDS_WITH, "ends_with", "str str", "bool", 0)                                    \
  X(CONTAINS, "contains", "str str", "bool", 0)                                      \
  X(SPLIT, "split", "str str", "a_str", 0)                                           \
  X(JOIN, "join", "a_str str", "str", 0)                                             \
  X(REPEAT, "repeat", "str num", "str", 0)                                           \
  X(SYMBOL, "symbol", "num", "str", 0)                                               \
  X(CODE_OF, "code_of", "str", "num", 0)                                             \
  X(HAS, "has", "dict str", "bool", 0)                                               \
  X(KEYS, "keys", "dict", "a_str", 0)                                                \
  X(VALUES, "values", "dict", "a_T", 0)                                              \
  X(COPY, "copy", "ad", "arg0", 0)                                                   \
  X(REVERSE, "reverse", "arr", "void", 0)                                            \
  X(SORT, "sort", "sarr", "void", 0)                                                 \
  X(SLICE, "slice", "arr num num", "arg0", 0)                                        \
  X(INDEX_OF, "index_of", "arr T", "num", 0)                                         \
  X(READ_FILE, "read_file", "str", "str", 0)                                         \
  X(WRITE_FILE, "write_file", "str str", "bool", 0)                                  \
  X(APPEND_FILE, "append_file", "str str", "bool", 0)                                \
  X(FILE_EXISTS, "file_exists", "str", "bool", 0)                                    \
  X(REMOVE_FILE, "remove_file", "str", "bool", 0)                                    \
  X(DIR_EXISTS, "dir_exists", "str", "bool", 0)                                      \
  X(MAKE_DIR, "make_dir", "str", "bool", 0)                                          \
  X(REMOVE_DIR, "remove_dir", "str", "bool", 0)                                      \
  X(LIST_DIR, "list_dir", "str", "a_str", 0)                                         \
  X(CURRENT_DIR, "current_dir", "", "str", 0)                                        \
  X(JOIN_PATH, "join_path", "str str", "str", 0)                                     \
  X(FILE_NAME, "file_name", "str", "str", 0)                                         \
  X(FILE_EXT, "file_ext", "str", "str", 0)                                           \
  X(PARENT_PATH, "parent_path", "str", "str", 0)                                     \
  X(GET_DELTA, "get_delta", "", "num", 0)                                            \
  X(RANDOM_CHOICE, "random_choice", "arr", "T", 0)                                   \
  X(SET_SEED, "set_seed", "num", "void", 0)                                          \
  X(RANDOM_SEED, "random_seed", "", "num", 0)                                        \
  X(RANDOM_INT_SEED, "random_int_seed", "num num", "num", 0)                         \
  X(RANDOM_CHOICE_SEED, "random_choice_seed", "arr", "T", 0)                         \
  X(EX_ARRAY, "ex_array", "json_str", "a_str", 0)                                    \
  X(EX_DICT, "ex_dict", "json_str", "d_str", 0)                                      \
  X(EX_STR, "ex_str", "json_str", "str", 0)                                          \
  X(PARSE_JSON, "parse_json", "str", "json_str", 0)

enum BId {
#define X(id, name, params, ret, var) B_##id,
  BUILTIN_LIST(X)
#undef X
  B_COUNT
};
struct BI { const char* name; int id; const char* params; const char* ret; bool variadic; };
static const BI BUILTINS[] = {
#define X(id, name, params, ret, var) {name, B_##id, params, ret, (var) != 0},
  BUILTIN_LIST(X)
#undef X
};
static const BI* findBuiltin(const std::string& n) {
  for (const BI& b : BUILTINS) if (n == b.name) return &b;
  return nullptr;
}

// ============================================================================
// 6. Компилятор: AST -> регистровый байткод со статической проверкой типов
// ============================================================================
class Compiler {
 public:
  explicit Compiler(Program& p) : prog(p), gtypes(p.globalTypes) {}

  int compileChunk(Block& ast, std::deque<Proto>* dump = nullptr) {
    Snap snap{funcs.size(), bps.size(), states.size(), gtypes.size(),
              prog.protos.size(), prog.bps.size(), prog.states.size()};
    std::vector<int> mine;
    try {
      collect(ast, mine);
      for (size_t i = snap.bps; i < bps.size(); i++) {
        BpRt r; r.name = bps[i].name;
        for (auto& f : bps[i].fields) r.fieldTypes.push_back(f.type);
        prog.bps.push_back(std::move(r));
      }
      for (size_t i = snap.states; i < states.size(); i++) {
        StatesRt r; r.name = states[i].name; r.variants = states[i].variants;
        prog.states.push_back(std::move(r));
      }
      for (size_t i = snap.funcs; i < funcs.size(); i++) {
        funcs[i].proto = (int)prog.protos.size(); prog.protos.emplace_back();
      }
      std::vector<Proto> staged(mine.size());
      Proto mainP; mainP.name = "<main>";
      {
        FS m; m.proto = &mainP; m.isMain = true; fs = &m; curLine = 1;
        for (auto& s : ast)
          if (s->k != SK::Func && s->k != SK::Blueprint && s->k != SK::States)
            stmt(s.get());
        emit(OP_RETV); finish();
      }
      for (size_t k = 0; k < mine.size(); k++) {
        const FuncInfo& f = funcs[mine[k]];
        staged[k].name = f.bp >= 0 ? bps[f.bp].name + "." + f.name : f.name;
        compileFunc((size_t)mine[k], &staged[k]);
      }
      if (dump) {
        for (auto& st : staged) dump->push_back(std::move(st));
        dump->push_back(std::move(mainP));
        for (int i : mine) funcs[i].decl = nullptr;
        rollback(snap); fs = nullptr;
        return -1;
      }
      for (size_t k = 0; k < mine.size(); k++) prog.protos[funcs[mine[k]].proto] = std::move(staged[k]);
      prog.protos.push_back(std::move(mainP));
      for (int i : mine) funcs[i].decl = nullptr;
      fs = nullptr;
      for (int i : mine) peephole(prog.protos[(size_t)funcs[i].proto]);
      peephole(prog.protos.back());
      return (int)prog.protos.size() - 1;
    } catch (...) {
      for (int i : mine) funcs[i].decl = nullptr;
      rollback(snap); fs = nullptr;
      throw;
    }
  }

  struct Sig { std::vector<Type> params; Type ret; bool priv = false; int proto = 0; };
  struct FieldRef { int idx = 0; Type type; bool priv = false; };
  bool hasFunc(const std::string& n) const { return funcByName.count(n) != 0; }
  bool lookupFunc(const std::string& n, Sig& out) const {
    auto it = funcByName.find(n);
    if (it == funcByName.end()) return false;
    fill(funcs[it->second], out); return true;
  }
  bool lookupMethod(const Type& bpType, const std::string& n, Sig& out) const {
    int b = bpOf(bpType); if (b < 0) return false;
    auto it = bps[b].methods.find(n);
    if (it == bps[b].methods.end()) return false;
    fill(funcs[it->second], out); return true;
  }
  bool lookupField(const Type& bpType, const std::string& n, FieldRef& out) const {
    int b = bpOf(bpType); if (b < 0) return false;
    auto it = bps[b].fieldIdx.find(n);
    if (it == bps[b].fieldIdx.end()) return false;
    const FieldInfo& f = bps[b].fields[it->second];
    out.idx = it->second; out.type = f.type; out.priv = f.priv; return true;
  }
  int blueprintIndex(const Type& t) const { return bpOf(t); }
  int globalIndex(const std::string& n) const { auto it = gidx.find(n); return it == gidx.end() ? -1 : it->second; }
  int addGlobal(const std::string& n, const Type& t) {
    curLine = 0;
    size_t before = gtypes.size();
    int i = declareGlobal(n, t);
    if (gtypes.size() > before) hostGlobal[(size_t)i] = true;
    return i;
  }
  bool typeKnown(const Type& t) const {
    Type b = t;
    while (isArr(b) || isDict(b)) b = elemOf(b);
    return b == "num" || b == "bool" || b == "str" || b == "json_str"
        || bpByName.count(b) || statesByName.count(b);
  }

 private:
  struct R { int reg; Type type; };
  struct FieldInfo { std::string name; Type type; bool priv; };
  struct FuncInfo {
    std::string name; std::vector<Type> params; Type ret; bool priv = false; int bp = -1; Stmt* decl = nullptr;
    int proto = 0;
  };
  struct Snap { size_t funcs, bps, states, globals, protos, pbps, pstates; };
  struct BpInfo {
    std::string name; std::vector<FieldInfo> fields;
    std::unordered_map<std::string, int> fieldIdx, methods;
    int ctor = -1;
  };
  struct StatesInfo {
    std::string name;
    std::vector<std::string> variants;
    std::unordered_map<std::string, int> variantIdx;
  };
  struct Local { std::string name; Type type; int reg; };
  struct Loop { std::vector<int> breaks; int start; };
  struct FS {
    Proto* proto = nullptr;
    std::vector<Local> locals; std::vector<size_t> scopes; std::vector<int> scopeRegs;
    std::vector<Loop> loops;
    std::unordered_map<std::string, int> kmap;
    int nextReg = 0, maxReg = 0, bp = -1;
    bool isMain = false; Type ret = "void";
  };
  struct VarRef { int kind = 0; int idx = 0; Type type; };
  struct LV { int kind = 0, a = 0, b = 0; Type type; };

  Program& prog; std::vector<Type>& gtypes;
  std::vector<bool> hostGlobal;
  std::vector<BpInfo> bps; std::unordered_map<std::string, int> bpByName;
  std::vector<StatesInfo> states;
  std::unordered_map<std::string, int> statesByName;
  std::vector<FuncInfo> funcs; std::unordered_map<std::string, int> funcByName;
  std::unordered_map<std::string, int> gidx;
  FS* fs = nullptr; int curLine = 1;

  static void fill(const FuncInfo& f, Sig& out) { out.params = f.params; out.ret = f.ret; out.priv = f.priv; out.proto = f.proto; }
  void rollback(const Snap& s) {
    for (auto it = funcByName.begin(); it != funcByName.end();) it = (size_t)it->second >= s.funcs ? funcByName.erase(it) : std::next(it);
    funcs.resize(s.funcs);
    for (auto it = bpByName.begin(); it != bpByName.end();) it = (size_t)it->second >= s.bps ? bpByName.erase(it) : std::next(it);
    bps.resize(s.bps);
    for (auto it = statesByName.begin(); it != statesByName.end();) it = (size_t)it->second >= s.states ? statesByName.erase(it) : std::next(it);
    states.resize(s.states);
    for (auto it = gidx.begin(); it != gidx.end();) it = (size_t)it->second >= s.globals ? gidx.erase(it) : std::next(it);
    gtypes.resize(s.globals); hostGlobal.resize(s.globals);
    prog.protos.resize(s.protos); prog.bps.resize(s.pbps);
    prog.states.resize(s.pstates);
  }

  // Склейка соседних инструкций в супер-инструкции. Безопасность:
  //  - вторую инструкцию пары нельзя клеить, если на неё ведёт переход;
  //  - все переходы (JMP.a, JMPF/JMPT.b, LT_JMPF.d) пересчитываются по таблице старый->новый индекс.
  static void peephole(Proto& pr) {
    auto& code = pr.code;
    const size_t n = code.size();
    std::vector<char> target(n + 1, 0);
    auto mark = [&](int t) { if (t >= 0 && (size_t)t <= n) target[(size_t)t] = 1; };
    for (const Instr& in : code) {
      if (in.op == OP_JMP) mark(in.a);
      else if (in.op == OP_JMPF || in.op == OP_JMPT) mark(in.b);
      else if (in.op == OP_LT_JMPF) mark(in.d);
    }
    std::vector<Instr> nc; std::vector<int> nl; std::vector<int> remap(n + 1, 0);
    nc.reserve(n); nl.reserve(n);
    bool changed = false;
    for (size_t i = 0; i < n;) {
      remap[i] = (int)nc.size();
      if (i + 1 < n && !target[i + 1]) {
        const Instr& a = code[i]; const Instr& b = code[i + 1];
        Instr f = a; bool merged = false;
        if (a.op == OP_LOADK && b.op == OP_ADD && b.b == a.a && b.a == a.a && b.c != a.a) {
          f.op = OP_LOADK_ADD; f.c = b.c; merged = true;           // R[a] = K[b] + RK(c)
        } else if (a.op == OP_LT && b.op == OP_JMPF && b.a == a.a) {
          f.op = OP_LT_JMPF; f.d = b.b; merged = true;             // R[a] = x<y; if !R[a] goto d
        } else if (a.op == OP_MOVE && b.op == OP_RET && b.a == a.a) {
          f.op = OP_MOVE_RET; merged = true;                       // return R[b]
        }
        if (merged) {
          remap[i + 1] = (int)nc.size();
          nc.push_back(f); nl.push_back(pr.lines[i]);
          i += 2; changed = true; continue;
        }
      }
      nc.push_back(code[i]); nl.push_back(pr.lines[i]); i++;
    }
    if (changed) {
      remap[n] = (int)nc.size();
      for (Instr& in : nc) {
        if (in.op == OP_JMP) in.a = remap[(size_t)in.a];
        else if (in.op == OP_JMPF || in.op == OP_JMPT) in.b = remap[(size_t)in.b];
        else if (in.op == OP_LT_JMPF) in.d = remap[(size_t)in.d];
      }
    }
    // Специализация по виду операндов: RR (оба регистры), RK (c - константа), KR (b - константа).
    // Варианты идут подряд в OP_LIST: base, base+1, base+2. Для RK/KR индекс константы хранится как ~x -> x.
    auto spec = [](Instr& in, Op rr) {
      bool bk = in.b < 0, ck = in.c < 0;
      if (bk && ck) return;
      if (!bk && !ck) in.op = (uint8_t)rr;
      else if (ck) { in.op = (uint8_t)(rr + 1); in.c = ~in.c; }
      else { in.op = (uint8_t)(rr + 2); in.b = ~in.b; }
    };
    for (Instr& in : nc) {
      switch (in.op) {
        case OP_ADD: spec(in, OP_ADD_RR); break;
        case OP_SUB: spec(in, OP_SUB_RR); break;
        case OP_MUL: spec(in, OP_MUL_RR); break;
        case OP_LT_JMPF: spec(in, OP_LT_JMPF_RR); break;
        default: break;
      }
    }
    code = std::move(nc); pr.lines = std::move(nl);
  }

  void need(bool ok, const std::string& msg) { if (!ok) fail("Type", curLine, msg); }
  int here() const { return (int)fs->proto->code.size(); }
  int emit(Op op, int a = 0, int b = 0, int c = 0) {
    fs->proto->code.push_back(Instr{(uint8_t)op, a, b, c});
    fs->proto->lines.push_back(curLine);
    return here() - 1;
  }
  void patch(int idx) {
    Instr& i = fs->proto->code[idx];
    if (i.op == OP_JMP) i.a = here(); else i.b = here();
  }
  int alloc() { int r = fs->nextReg++; if (fs->nextReg > fs->maxReg) fs->maxReg = fs->nextReg; return r; }
  void finish() { fs->proto->maxRegs = fs->maxReg; }

  int addK(const std::string& key, Value v) {
    auto it = fs->kmap.find(key);
    if (it != fs->kmap.end()) return it->second;
    int i = (int)fs->proto->consts.size();
    fs->proto->consts.push_back(std::move(v)); fs->kmap[key] = i;
    return i;
  }
  int kNum(double d) {
    uint64_t b; memcpy(&b, &d, 8);
    char buf[32]; snprintf(buf, sizeof buf, "n%016llx", (unsigned long long)b);
    return addK(buf, Value::num(d));
  }
  int kStr(const std::string& s) { return addK("s" + s, mkStr(s)); }
  int kBool(bool b) { return addK(b ? "bt" : "bf", Value::boolean(b)); }

  void emitDefault(int reg, const Type& ty) {
    if (ty == "num") emit(OP_LOADK, reg, kNum(0));
    else if (ty == "bool") emit(OP_LOADK, reg, kBool(false));
    else if (ty == "str" || ty == "json_str") emit(OP_LOADK, reg, kStr(""));
    else if (isArr(ty)) emit(OP_NEWARR, reg);
    else if (isDict(ty)) emit(OP_NEWDICT, reg);
    else {
      auto it = statesByName.find(ty);
      if (it != statesByName.end()) emit(OP_LOADK, reg, kStr(states[it->second].variants[0]));
      else emit(OP_LOADNIL, reg);
    }
  }
  void loadRK(int dst, int rk) {
    if (rk < 0) emit(OP_LOADK, dst, ~rk);
    else if (rk != dst) emit(OP_MOVE, dst, rk);
  }

  int bpOf(const Type& t) const { auto it = bpByName.find(t); return it == bpByName.end() ? -1 : it->second; }
  void checkType(const Type& t, int ln) {
    Type b = t;
    while (isArr(b) || isDict(b)) b = elemOf(b);
    if (b == "num" || b == "bool" || b == "str" || b == "json_str" || bpByName.count(b) || statesByName.count(b)) return;
    fail("Type", ln, "unknown type '" + t + "'");
  }
  bool isEquatable(const Type& t) const {
    return isScalar(t) || bpOf(t) >= 0 || statesByName.count(t) != 0 || t == "json_str";
  }
  void beginScope() { fs->scopes.push_back(fs->locals.size()); fs->scopeRegs.push_back(fs->nextReg); }
  void endScope() {
    fs->locals.resize(fs->scopes.back()); fs->nextReg = fs->scopeRegs.back();
    fs->scopes.pop_back(); fs->scopeRegs.pop_back();
  }
  void block(Block& b) { beginScope(); for (auto& s : b) stmt(s.get()); endScope(); }
  int findInScope(const std::string& n) {
    size_t start = fs->scopes.empty() ? 0 : fs->scopes.back();
    for (size_t i = fs->locals.size(); i-- > start;) if (fs->locals[i].name == n) return (int)i;
    return -1;
  }
  VarRef findVar(const std::string& n) {
    for (size_t i = fs->locals.size(); i-- > 0;)
      if (fs->locals[i].name == n) return VarRef{1, fs->locals[i].reg, fs->locals[i].type};
    if (fs->bp >= 0) {
      auto& b = bps[fs->bp]; auto it = b.fieldIdx.find(n);
      if (it != b.fieldIdx.end()) return VarRef{2, it->second, b.fields[it->second].type};
    }
    auto g = gidx.find(n);
    if (g != gidx.end()) return VarRef{3, g->second, gtypes[g->second]};
    return VarRef{};
  }
  int declareGlobal(const std::string& n, const Type& ty) {
    auto it = gidx.find(n);
    if (it != gidx.end()) {
      need(gtypes[it->second] == ty,
           hostGlobal[it->second] ? "global '" + n + "' was pre-declared by the host as '" + gtypes[it->second] + "'"
                                  : "variable '" + n + "' was already declared with type '" + gtypes[it->second] + "'");
      return it->second;
    }
    int i = (int)gtypes.size(); gtypes.push_back(ty); hostGlobal.push_back(false); gidx[n] = i;
    return i;
  }
  static bool multiStep(const Expr* e) {
    return e->k == EK::ArrLit || e->k == EK::DictLit ||
           (e->k == EK::Binary && (e->s == "and" || e->s == "or"));
  }
  static bool numLit(const Expr* e, double& v) {
    if (e->k == EK::Num) { v = e->num; return true; }
    if (e->k == EK::Unary && e->s == "-" && numLit(e->kids[0].get(), v)) { v = -v; return true; }
    return false;
  }

  FuncInfo makeFunc(Stmt* s, int bp) {
    FuncInfo f; f.name = s->name; f.ret = s->type; f.priv = s->priv; f.bp = bp; f.decl = s;
    if (f.ret != "void") checkType(f.ret, s->line);
    for (auto& p : s->params) { checkType(p.type, s->line); f.params.push_back(p.type); }
    return f;
  }
  void collect(Block& ast, std::vector<int>& mine) {
    std::unordered_set<std::string> seen;
    for (auto& s : ast) {
      if (s->k != SK::Blueprint && s->k != SK::States) continue;
      const std::string& n = s->name;
      if (n == "num" || n == "bool" || n == "str" || n == "json_str" || n == "void" || isArr(n) || isDict(n))
        fail("Type", s->line, "'" + n + "' cannot be used as a " + (s->k == SK::States ? "states" : "blueprint") + " name");
      if (s->k == SK::States) {
        if (statesByName.count(n)) fail("Type", s->line, "states '" + n + "' is already defined");
        if (bpByName.count(n))     fail("Type", s->line, "states name '" + n + "' conflicts with a blueprint");
        StatesInfo si; si.name = n;
        for (auto& v : s->variants) {
          for (auto& other : states)
            if (other.variantIdx.count(v))
              fail("Type", s->line, "variant '" + v + "' is already used by states '" + other.name + "'");
          if (si.variantIdx.count(v))
            fail("Type", s->line, "duplicate variant '" + v + "' in '" + n + "'");
          si.variantIdx[v] = (int)si.variants.size();
          si.variants.push_back(v);
        }
        statesByName[n] = (int)states.size();
        states.push_back(std::move(si));
      } else {
        if (bpByName.count(n))      fail("Type", s->line, "blueprint '" + n + "' is already defined");
        if (statesByName.count(n))  fail("Type", s->line, "blueprint name '" + n + "' conflicts with a states");
        bpByName[n] = (int)bps.size();
        BpInfo b; b.name = n; bps.push_back(b);
      }
    }
    for (auto& s : ast) {
      if (s->k == SK::Blueprint) {
        int bi = bpByName[s->name];
        for (auto& f : s->fields) {
          checkType(f.type, f.line);
          if (bps[bi].fieldIdx.count(f.name)) fail("Type", f.line, "duplicate field '" + f.name + "' in '" + s->name + "'");
          bps[bi].fieldIdx[f.name] = (int)bps[bi].fields.size();
          bps[bi].fields.push_back(FieldInfo{f.name, f.type, f.priv});
        }
        for (auto& m : s->methods) {
          if (bps[bi].methods.count(m->name) || bps[bi].fieldIdx.count(m->name))
            fail("Type", m->line, "duplicate member '" + m->name + "' in '" + s->name + "'");
          FuncInfo fi = makeFunc(m.get(), bi);
          bps[bi].methods[m->name] = (int)funcs.size();
          mine.push_back((int)funcs.size());
          funcs.push_back(fi);
        }
        if (s->ctor) {
          if (bps[bi].methods.count("init") || bps[bi].fieldIdx.count("init"))
            fail("Type", s->ctor->line, "'init' is the constructor name and cannot also be a field or method of '" + s->name + "'");
          FuncInfo fi = makeFunc(s->ctor.get(), bi);
          bps[bi].ctor = (int)funcs.size();
          mine.push_back((int)funcs.size());
          funcs.push_back(fi);
        }
      } else if (s->k == SK::Func) {
        if (!seen.insert(s->name).second) fail("Type", s->line, "function '" + s->name + "' is already defined");
        if (prog.hostByName.count(s->name)) fail("Type", s->line, "function '" + s->name + "' conflicts with a host function");
        FuncInfo fi = makeFunc(s.get(), -1);
        auto old = funcByName.find(s->name);
        if (old != funcByName.end()) {
          FuncInfo& o = funcs[old->second];
          if (o.params != fi.params || o.ret != fi.ret)
            fail("Type", s->line, "function '" + s->name + "' was already defined with a different signature");
          o.decl = s.get(); mine.push_back(old->second);
        } else {
          funcByName[s->name] = (int)funcs.size();
          mine.push_back((int)funcs.size());
          funcs.push_back(fi);
        }
      }
    }
  }

  void compileFunc(size_t fi, Proto* target) {
    FuncInfo& f = funcs[fi];
    FS st; st.proto = target; st.bp = f.bp; st.ret = f.ret; fs = &st;
    curLine = f.decl->line;
    if (f.bp >= 0) { int r = alloc(); st.locals.push_back(Local{"self", bps[f.bp].name, r}); }
    for (size_t i = 0; i < f.params.size(); i++) {
      const std::string& pn = f.decl->params[i].name;
      for (auto& l : st.locals)
        if (l.name == pn) fail("Type", f.decl->line, "duplicate parameter '" + pn + "'");
      int r = alloc(); st.locals.push_back(Local{pn, f.params[i], r});
    }
    st.proto->nparams = st.nextReg;
    for (auto& s : f.decl->body) stmt(s.get());
    emit(f.ret == "void" ? OP_RETV : OP_NORET);
    finish();
  }

  R rk(Expr* e, const Type& hint = Type()) {
    double v;
    if (e->k == EK::Num || (e->k == EK::Unary && e->s == "-" && numLit(e, v))) {
      numLit(e, v); return {~kNum(v), "num"};
    }
    if (e->k == EK::Str) {
      Type rt = (hint == "json_str") ? "json_str" : "str";
      return {~kStr(e->s), rt};
    }
    if (e->k == EK::Bool) return {~kBool(e->num != 0), "bool"};
    return exp(e, -1, hint);
  }

  R identExp(Expr* e, int want) {
    VarRef v = findVar(e->s);
    switch (v.kind) {
      case 1:
        if (want >= 0 && want != v.idx) { emit(OP_MOVE, want, v.idx); return {want, v.type}; }
        return {v.idx, v.type};
      case 2: { int d = want >= 0 ? want : alloc(); emit(OP_GETF, d, 0, v.idx); return {d, v.type}; }
      case 3: { int d = want >= 0 ? want : alloc(); emit(OP_GETG, d, v.idx); return {d, v.type}; }
      default: break;
    }
    for (auto& st : states) {
      if (st.variantIdx.count(e->s)) {
        int d = want >= 0 ? want : alloc();
        emit(OP_LOADK, d, kStr(e->s));
        return {d, st.name};
      }
    }
    fail("Type", e->line, "undefined variable '" + e->s + "'");
  }

  R binop(const std::string& op, R l, R r, int want, int ln) {
    curLine = ln;
    Op o = OP_ADD; Type res = "num"; bool swap = false;
    auto bad = [&]() {
      fail("Type", ln, "operator '" + op + "' cannot be applied to '" + l.type + "' and '" + r.type + "'");
    };
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
      if (l.type != "num" || r.type != "num") bad();
      o = op == "+" ? OP_ADD : op == "-" ? OP_SUB : op == "*" ? OP_MUL : op == "/" ? OP_DIV : OP_MOD;
    } else if (op == "++") {
      if (l.type != "str" || r.type != "str") bad();
      o = OP_CONCAT; res = "str";
    } else if (op == "<" || op == "<=" || op == ">" || op == ">=") {
      if (l.type != r.type || (l.type != "num" && l.type != "str")) bad();
      o = (op == "<" || op == ">") ? OP_LT : OP_LE; swap = op[0] == '>'; res = "bool";
    } else {
      if (l.type != r.type || !isEquatable(l.type)) bad();
      o = op == "==" ? OP_EQ : OP_NE; res = "bool";
    }
    int dst = want >= 0 ? want : alloc();
    if (swap) emit(o, dst, r.reg, l.reg); else emit(o, dst, l.reg, r.reg);
    return {dst, res};
  }

  R logic(Expr* e, int want) {
    bool isAnd = e->s == "and";
    int dst = want >= 0 ? want : alloc();
    R l = exp(e->kids[0].get(), dst);
    curLine = e->line;
    need(l.type == "bool", "operator '" + e->s + "' needs bool operands, got '" + l.type + "'");
    int j = emit(isAnd ? OP_JMPF : OP_JMPT, dst, 0);
    int save = fs->nextReg;
    R r = exp(e->kids[1].get(), dst);
    curLine = e->line;
    need(r.type == "bool", "operator '" + e->s + "' needs bool operands, got '" + r.type + "'");
    fs->nextReg = save;
    patch(j);
    return {dst, "bool"};
  }

  R emitBuiltin(int id, size_t argc, int base, int want, const Type& rt, Op op = OP_CALLB) {
    emit(op, base, id, (int)argc);
    fs->nextReg = base;
    int r0 = alloc();
    if (want >= 0) {
      if (want != r0) emit(OP_MOVE, want, r0);
      fs->nextReg = base;
      return {want, rt};
    }
    return {r0, rt};
  }

  R inputExp(Expr* e, int want) {
    int id = B_INPUT_STR; Type rt = "str";
    need(e->kids.size() <= 1, "input() takes at most one argument: a type name");
    if (e->kids.size() == 1) {
      Expr* a = e->kids[0].get();
      need(a->k == EK::Ident && isScalar(a->s), "input() takes a type name: input(str), input(num) or input(bool)");
      if (a->s == "num") { id = B_INPUT_NUM; rt = "num"; }
      else if (a->s == "bool") { id = B_INPUT_BOOL; rt = "bool"; }
    }
    return emitBuiltin(id, 0, fs->nextReg, want, rt);
  }

  static std::string describePat(const std::string& p) {
    if (p == "arr") return "an array";
    if (p == "dict") return "a dictionary";
    if (p == "ad") return "an array or a dictionary";
    if (p == "sarr") return "an array of num or str";
    if (p == "cont") return "str, an array or a dictionary";
    if (p == "T") return "the element type of the first argument";
    if (p == "any") return "a value";
    if (p == "json_str") return "a json_str";
    return "'" + p + "'";
  }
  Type checkBuiltin(const BI& b, const std::vector<Type>& at) {
    std::vector<std::string> pats;
    { std::istringstream ss(b.params); std::string w; while (ss >> w) pats.push_back(w); }
    std::string nm = b.name;
    Type T;
    if (b.variadic) {
      for (auto& t : at) need(t != "void", "'" + nm + "': cannot use a value of type 'void'");
    } else {
      need(at.size() == pats.size(), "'" + nm + "' expects " + std::to_string(pats.size()) +
                                         " argument(s), got " + std::to_string(at.size()));
      for (size_t i = 0; i < pats.size(); i++) {
        const std::string& pt = pats[i]; const Type& ty = at[i];
        bool ok = false;
        if (pt == "num" || pt == "str" || pt == "bool" || pt == "a_str") ok = ty == pt;
        else if (pt == "any") ok = ty != "void";
        else if (pt == "cont") ok = ty == "str" || isArr(ty) || isDict(ty) || ty == "json_str";
        else if (pt == "arr") { ok = isArr(ty); if (ok) T = elemOf(ty); }
        else if (pt == "dict") { ok = isDict(ty); if (ok) T = elemOf(ty); }
        else if (pt == "ad") ok = isArr(ty) || isDict(ty);
        else if (pt == "sarr") { ok = isArr(ty) && (elemOf(ty) == "num" || elemOf(ty) == "str"); if (ok) T = elemOf(ty); }
        else if (pt == "T") ok = ty == T;
        else if (pt == "json_str") ok = ty == "json_str" || ty == "str" || ty == "a_str" || ty == "d_str";
        need(ok, "argument " + std::to_string(i + 1) + " of '" + nm + "': expected " + describePat(pt) +
                     ", got '" + ty + "'");
      }
    }
    std::string r = b.ret;
    if (r == "arg0") return at[0];
    if (r == "a_T") return "a_" + T;
    if (r == "T") return T;
    return r;
  }

  R callExp(Expr* e, int want) {
    int save = fs->nextReg;
    bool isM = e->k == EK::MCall;
    const std::string& name = e->s;
    size_t firstArg = isM ? 1 : 0;
    size_t nargs = e->kids.size() - firstArg;
    int fidx = -1, base = save;
    if (isM) {
      base = alloc();
      R recv = exp(e->kids[0].get(), base);
      fs->nextReg = base + 1;
      curLine = e->line;
      int bpi = bpOf(recv.type);
      need(bpi >= 0, "type '" + recv.type + "' has no methods");
      auto it = bps[bpi].methods.find(name);
      need(it != bps[bpi].methods.end(), "blueprint '" + bps[bpi].name + "' has no method '" + name + "'");
      fidx = it->second;
      need(!(funcs[fidx].priv && fs->bp != bpi),
           "method '" + name + "' of '" + bps[bpi].name + "' is private (unaccess)");
    } else {
      if (fs->bp >= 0) {
        auto it = bps[fs->bp].methods.find(name);
        if (it != bps[fs->bp].methods.end()) fidx = it->second;
      }
      if (fidx < 0) { auto it = funcByName.find(name); if (it != funcByName.end()) fidx = it->second; }
      if (fidx >= 0 && funcs[fidx].bp >= 0) { base = alloc(); emit(OP_MOVE, base, 0); }
    }
    if (fidx >= 0) {
      const FuncInfo& fi = funcs[fidx];
      need(nargs == fi.params.size(), "'" + name + "' expects " + std::to_string(fi.params.size()) +
                                          " argument(s), got " + std::to_string(nargs));
      for (size_t i = 0; i < nargs; i++) {
        int slot = alloc();
        R a = exp(e->kids[firstArg + i].get(), slot, fi.params[i]);
        need(a.type == fi.params[i], "argument " + std::to_string(i + 1) + " of '" + name +
                                         "': expected '" + fi.params[i] + "', got '" + a.type + "'");
        fs->nextReg = slot + 1;
      }
      fs->nextReg = save;
      curLine = e->line;
      int dst = want >= 0 ? want : alloc();
      emit(OP_CALL, dst, fi.proto, base);
      return {dst, fi.ret};
    }
    auto hit = prog.hostByName.find(name);
    if (hit != prog.hostByName.end()) {
      const HostFn& h = prog.hosts[hit->second];
      curLine = e->line;
      for (auto& pt : h.params) checkType(pt, e->line);
      if (h.ret != "void") checkType(h.ret, e->line);
      size_t np = h.params.size();
      need(h.variadic ? nargs >= np : nargs == np, "'" + name + "' expects " + (h.variadic ? "at least " : "") +
                                                      std::to_string(np) + " argument(s), got " + std::to_string(nargs));
      for (size_t i = 0; i < nargs; i++) {
        int slot = alloc();
        R a = exp(e->kids[i].get(), slot, i < np ? h.params[i] : Type());
        curLine = e->line;
        if (i < np)
          need(a.type == h.params[i], "argument " + std::to_string(i + 1) + " of '" + name + "': expected '" +
                                          h.params[i] + "', got '" + a.type + "'");
        else
          need(a.type == "num" || a.type == "str" || a.type == "bool",
               "extra arguments of '" + name + "' must be num, str or bool, got '" + a.type + "'");
        fs->nextReg = slot + 1;
      }
      curLine = e->line;
      return emitBuiltin(hit->second, nargs, save, want, h.ret, OP_CALLH);
    }
    if (name == "input") return inputExp(e, want);
    const BI* bi = findBuiltin(name);
    need(bi != nullptr, "undefined function '" + name + "'");
    std::vector<Type> types;
    for (size_t i = 0; i < nargs; i++) {
      int slot = alloc();
      R a = exp(e->kids[i].get(), slot);
      types.push_back(a.type);
      fs->nextReg = slot + 1;
    }
    curLine = e->line;
    Type rt = checkBuiltin(*bi, types);
    return emitBuiltin(bi->id, nargs, save, want, rt);
  }

  R exp(Expr* e, int want = -1, const Type& hint = Type()) {
    curLine = e->line;
    switch (e->k) {
      case EK::Num: case EK::Str: case EK::Bool: {
        int k = e->k == EK::Num ? kNum(e->num) : e->k == EK::Str ? kStr(e->s) : kBool(e->num != 0);
        int dst = want >= 0 ? want : alloc();
        emit(OP_LOADK, dst, k);
        Type rt = e->k == EK::Num ? "num" : e->k == EK::Bool ? "bool"
                 : (hint == "json_str") ? "json_str" : "str";
        return {dst, rt};
      }
      case EK::Ident: return identExp(e, want);
      case EK::Unary: {
        double v;
        if (e->s == "-" && numLit(e, v)) {
          int dst = want >= 0 ? want : alloc();
          emit(OP_LOADK, dst, kNum(v));
          return {dst, "num"};
        }
        int save = fs->nextReg;
        R o = rk(e->kids[0].get());
        fs->nextReg = save;
        curLine = e->line;
        if (e->s == "-") need(o.type == "num", "operator '-' needs a num, got '" + o.type + "'");
        else need(o.type == "bool", "operator 'not' needs a bool, got '" + o.type + "'");
        int dst = want >= 0 ? want : alloc();
        emit(e->s == "-" ? OP_NEG : OP_NOT, dst, o.reg);
        return {dst, e->s == "-" ? "num" : "bool"};
      }
      case EK::Binary: {
        if (e->s == "and" || e->s == "or") return logic(e, want);
        int save = fs->nextReg;
        R l = rk(e->kids[0].get()); R r = rk(e->kids[1].get());
        fs->nextReg = save;
        return binop(e->s, l, r, want, e->line);
      }
      case EK::ArrLit: {
        int dst = want >= 0 ? want : alloc();
        if (hint == "json_str") {
          emit(OP_NEWARR, dst);
          for (auto& k : e->kids) {
            int sv = fs->nextReg;
            R v = rk(k.get(), "json_str");
            curLine = k->line;
            need(v.type == "json_str", "json_str array element must be json_str, got '" + v.type + "'");
            emit(OP_APPEND, dst, v.reg);
            fs->nextReg = sv;
          }
          return {dst, "json_str"};
        }
        if (e->kids.empty()) {
          if (isArr(hint)) { emit(OP_NEWARR, dst); return {dst, hint}; }
          if (isDict(hint)) { emit(OP_NEWDICT, dst); return {dst, hint}; }
          fail("Type", e->line, "cannot infer the type of an empty literal []; declare it with a type, e.g. a_num xs = []");
        }
        emit(OP_NEWARR, dst);
        Type et = isArr(hint) ? elemOf(hint) : Type();
        for (auto& k : e->kids) {
          int sv = fs->nextReg;
          R v = rk(k.get(), et);
          curLine = k->line;
          if (et.empty()) { need(v.type != "void", "an array element cannot be void"); et = v.type; }
          need(v.type == et, "array elements must share one type: expected '" + et + "', got '" + v.type + "'");
          emit(OP_APPEND, dst, v.reg);
          fs->nextReg = sv;
        }
        return {dst, "a_" + et};
      }
      case EK::DictLit: {
        int dst = want >= 0 ? want : alloc();
        if (hint == "json_str") {
          emit(OP_NEWDICT, dst);
          for (size_t i = 0; i + 1 < e->kids.size(); i += 2) {
            int sv = fs->nextReg;
            R k = rk(e->kids[i].get(), "str");
            curLine = e->kids[i]->line;
            need(k.type == "str", "dictionary keys must be str, got '" + k.type + "'");
            R v = rk(e->kids[i + 1].get(), "json_str");
            curLine = e->kids[i + 1]->line;
            need(v.type == "json_str", "json_str dictionary value must be json_str, got '" + v.type + "'");
            emit(OP_DSET, dst, k.reg, v.reg);
            fs->nextReg = sv;
          }
          return {dst, "json_str"};
        }
        emit(OP_NEWDICT, dst);
        Type vt = isDict(hint) ? elemOf(hint) : Type();
        for (size_t i = 0; i + 1 < e->kids.size(); i += 2) {
          int sv = fs->nextReg;
          R k = rk(e->kids[i].get(), "str");
          curLine = e->kids[i]->line;
          need(k.type == "str", "dictionary keys must be str, got '" + k.type + "'");
          R v = rk(e->kids[i + 1].get(), vt);
          curLine = e->kids[i + 1]->line;
          if (vt.empty()) { need(v.type != "void", "a dictionary value cannot be void"); vt = v.type; }
          need(v.type == vt, "dictionary values must share one type: expected '" + vt + "', got '" + v.type + "'");
          emit(OP_DSET, dst, k.reg, v.reg);
          fs->nextReg = sv;
        }
        return {dst, "d_" + vt};
      }
      case EK::Index: {
        int save = fs->nextReg;
        R b = exp(e->kids[0].get());
        R i = rk(e->kids[1].get());
        fs->nextReg = save;
        curLine = e->line;
        Op op = OP_GETI; Type rt;
        if (isArr(b.type)) { need(i.type == "num", "array index must be num, got '" + i.type + "'"); rt = elemOf(b.type); }
        else if (isDict(b.type)) { need(i.type == "str", "dictionary key must be str, got '" + i.type + "'"); rt = elemOf(b.type); op = OP_GETD; }
        else if (b.type == "str") { need(i.type == "num", "string index must be num, got '" + i.type + "'"); rt = "str"; }
        else if (b.type == "json_str") {
          if (i.type == "num") { rt = "json_str"; op = OP_GETI; }
          else if (i.type == "str") { rt = "json_str"; op = OP_GETD; }
          else fail("Type", e->line, "json_str index must be num or str, got '" + i.type + "'");
        }
        else fail("Type", e->line, "cannot index a value of type '" + b.type + "'");
        int dst = want >= 0 ? want : alloc();
        emit(op, dst, b.reg, i.reg);
        return {dst, rt};
      }
      case EK::Member: {
        int save = fs->nextReg;
        R o = exp(e->kids[0].get());
        fs->nextReg = save;
        curLine = e->line;
        int bpi = bpOf(o.type);
        need(bpi >= 0, "type '" + o.type + "' has no fields");
        auto it = bps[bpi].fieldIdx.find(e->s);
        need(it != bps[bpi].fieldIdx.end(), "blueprint '" + bps[bpi].name + "' has no field '" + e->s + "'");
        const FieldInfo& f = bps[bpi].fields[it->second];
        need(!(f.priv && fs->bp != bpi), "field '" + e->s + "' of '" + bps[bpi].name + "' is private (unaccess)");
        int dst = want >= 0 ? want : alloc();
        emit(OP_GETF, dst, o.reg, it->second);
        return {dst, f.type};
      }
      case EK::Call: case EK::MCall: return callExp(e, want);
    }
    fail("Type", e->line, "internal error: unknown expression");
  }

  LV lvalue(Expr* e) {
    curLine = e->line;
    if (e->k == EK::Ident) {
      VarRef v = findVar(e->s);
      need(v.kind != 0, "undefined variable '" + e->s + "'");
      if (v.kind == 1) return LV{0, v.idx, 0, v.type};
      if (v.kind == 3) return LV{1, v.idx, 0, v.type};
      return LV{2, 0, v.idx, v.type};
    }
    if (e->k == EK::Index) {
      R b = exp(e->kids[0].get());
      R i = rk(e->kids[1].get());
      curLine = e->line;
      if (isArr(b.type)) { need(i.type == "num", "array index must be num, got '" + i.type + "'"); return LV{3, b.reg, i.reg, elemOf(b.type)}; }
      if (isDict(b.type)) { need(i.type == "str", "dictionary key must be str, got '" + i.type + "'"); return LV{4, b.reg, i.reg, elemOf(b.type)}; }
      if (b.type == "json_str") {
        if (i.type == "num") return LV{3, b.reg, i.reg, "json_str"};
        if (i.type == "str") return LV{4, b.reg, i.reg, "json_str"};
        fail("Type", e->line, "json_str index must be num or str, got '" + i.type + "'");
      }
      fail("Type", e->line, b.type == "str" ? "strings are immutable: cannot assign to a character"
                                            : "cannot index a value of type '" + b.type + "'");
    }
    R o = exp(e->kids[0].get());
    curLine = e->line;
    int bpi = bpOf(o.type);
    need(bpi >= 0, "type '" + o.type + "' has no fields");
    auto it = bps[bpi].fieldIdx.find(e->s);
    need(it != bps[bpi].fieldIdx.end(), "blueprint '" + bps[bpi].name + "' has no field '" + e->s + "'");
    const FieldInfo& f = bps[bpi].fields[it->second];
    need(!(f.priv && fs->bp != bpi), "field '" + e->s + "' of '" + bps[bpi].name + "' is private (unaccess)");
    return LV{2, o.reg, it->second, f.type};
  }
  R lvLoad(const LV& lv, int want) {
    if (lv.kind == 0) {
      if (want >= 0 && want != lv.a) { emit(OP_MOVE, want, lv.a); return {want, lv.type}; }
      return {lv.a, lv.type};
    }
    int d = want >= 0 ? want : alloc();
    switch (lv.kind) {
      case 1: emit(OP_GETG, d, lv.a); break;
      case 2: emit(OP_GETF, d, lv.a, lv.b); break;
      case 3: emit(OP_GETI, d, lv.a, lv.b); break;
      default: emit(OP_GETD, d, lv.a, lv.b); break;
    }
    return {d, lv.type};
  }
  void lvStore(const LV& lv, int src) {
    switch (lv.kind) {
      case 0: loadRK(lv.a, src); break;
      case 1: emit(OP_SETG, lv.a, src); break;
      case 2: emit(OP_SETF, lv.a, lv.b, src); break;
      case 3: emit(OP_SETI, lv.a, lv.b, src); break;
      default: emit(OP_SETD, lv.a, lv.b, src); break;
    }
  }
  static std::string mismatch(const Type& got, const Type& want) {
    return "cannot assign a value of type '" + got + "' to a variable of type '" + want + "'";
  }
  void assignStmt(Stmt* s) {
    int save = fs->nextReg;
    LV lv = lvalue(s->e1.get());
    curLine = s->line;
    const std::string& op = s->name2;
    if (op == "=") {
      Expr* v = s->e2.get();
      if (lv.kind == 0 && !multiStep(v)) {
        R r = exp(v, lv.a, lv.type);
        need(r.type == lv.type, mismatch(r.type, lv.type));
      } else {
        R r = rk(v, lv.type);
        curLine = s->line;
        need(r.type == lv.type, mismatch(r.type, lv.type));
        lvStore(lv, r.reg);
      }
    } else if (op == "++=" && lv.kind == 1 && lv.type == "str") {
      R rhs = rk(s->e2.get()); curLine = s->line;
      need(rhs.type == "str", "operator '++' cannot be applied to 'str' and '" + rhs.type + "'");
      emit(OP_CONCATG, lv.a, rhs.reg);                         // global ++= x: дописывание на месте
    } else {
      std::string bop = op.substr(0, op.size() - 1);
      R cur = lvLoad(lv, -1);
      R rhs = rk(s->e2.get());
      R res = binop(bop, cur, rhs, lv.kind == 0 ? lv.a : -1, s->line);
      need(res.type == lv.type, mismatch(res.type, lv.type));
      if (lv.kind != 0) lvStore(lv, res.reg);
    }
    fs->nextReg = save;
  }

  void emitNewWithInit(int dst, int bpi, std::vector<ExprP>& args, int ln) {
    int save = fs->nextReg;
    const std::string bn = bps[bpi].name;
    curLine = ln;
    need(bps[bpi].ctor >= 0, "blueprint '" + bn + "' has no init");
    const FuncInfo& fi = funcs[bps[bpi].ctor];
    need(args.size() == fi.params.size(), "init of '" + bn + "' expects " + std::to_string(fi.params.size()) +
                                              " argument(s), got " + std::to_string(args.size()));
    int obj = alloc();
    emit(OP_NEW, obj, bpi);
    int base = alloc();
    emit(OP_MOVE, base, obj);
    for (size_t i = 0; i < args.size(); i++) {
      int slot = alloc();
      R a = exp(args[i].get(), slot, fi.params[i]);
      curLine = ln;
      need(a.type == fi.params[i], "argument " + std::to_string(i + 1) + " of init of '" + bn +
                                       "': expected '" + fi.params[i] + "', got '" + a.type + "'");
      fs->nextReg = slot + 1;
    }
    fs->nextReg = base + 1;
    curLine = ln;
    emit(OP_CALL, base, fi.proto, base);
    emit(OP_MOVE, dst, obj);
    fs->nextReg = save;
  }

  void declareVar(const std::string& name, const Type& ty, Expr* init, int newBp, int ln,
                  std::vector<ExprP>* ctorArgs = nullptr) {
    curLine = ln;
    bool global = fs->isMain && fs->scopes.empty();
    auto initInto = [&](int dst, bool direct) {
      if (newBp >= 0) {
        if (ctorArgs) emitNewWithInit(dst, newBp, *ctorArgs, ln); else emit(OP_NEW, dst, newBp);
        return;
      }
      if (!init) { emitDefault(dst, ty); return; }
      std::string msg = "cannot initialise '" + ty + "' variable '" + name + "' with a value of type '";
      if (direct && !multiStep(init)) {
        R v = exp(init, dst, ty);
        curLine = ln; need(v.type == ty, msg + v.type + "'");
      } else {
        int sv = fs->nextReg;
        R v = rk(init, ty);
        curLine = ln; need(v.type == ty, msg + v.type + "'");
        loadRK(dst, v.reg);
        fs->nextReg = sv;
      }
    };
    if (global) {
      int save = fs->nextReg;
      int t = alloc();
      initInto(t, true);
      int gi = declareGlobal(name, ty);
      emit(OP_SETG, gi, t);
      fs->nextReg = save;
      return;
    }
    int ex = findInScope(name);
    if (ex >= 0) {
      need(fs->locals[ex].type == ty, "variable '" + name + "' was already declared with type '" + fs->locals[ex].type + "'");
      int save = fs->nextReg;
      initInto(fs->locals[ex].reg, false);
      fs->nextReg = save;
      return;
    }
    int reg = alloc();
    initInto(reg, true);
    fs->nextReg = reg + 1;
    fs->locals.push_back(Local{name, ty, reg});
  }

  void addStmt(Stmt* s) {
    Expr* t = s->e1.get();
    need(t->k == EK::Index, "add expects an indexed array element, e.g. add nums:0 55");
    R arr = exp(t->kids[0].get());
    curLine = s->line;
    need(isArr(arr.type), "add expects an array, got '" + arr.type + "'");
    Type et = elemOf(arr.type);
    R idx = rk(t->kids[1].get(), "num");
    curLine = s->line;
    need(idx.type == "num", "add index must be num, got '" + idx.type + "'");
    R val = rk(s->e2.get(), et);
    curLine = s->line;
    need(val.type == et, "add: expected element type '" + et + "', got '" + val.type + "'");
    emit(OP_AINSERT, arr.reg, idx.reg, val.reg);
  }
  void appendStmt(Stmt* s) {
    R arr = exp(s->e1.get());
    curLine = s->line;
    need(isArr(arr.type), "append expects an array, got '" + arr.type + "'");
    Type et = elemOf(arr.type);
    R val = rk(s->e2.get(), et);
    curLine = s->line;
    need(val.type == et, "append: expected element type '" + et + "', got '" + val.type + "'");
    emit(OP_APPEND, arr.reg, val.reg);
  }
  void prependStmt(Stmt* s) {
    R arr = exp(s->e1.get());
    curLine = s->line;
    need(isArr(arr.type), "prepend expects an array, got '" + arr.type + "'");
    Type et = elemOf(arr.type);
    R val = rk(s->e2.get(), et);
    curLine = s->line;
    need(val.type == et, "prepend: expected element type '" + et + "', got '" + val.type + "'");
    int zeroRK = ~kNum(0);
    emit(OP_AINSERT, arr.reg, zeroRK, val.reg);
  }

  void stmt(Stmt* s) {
    curLine = s->line;
    int save = fs->nextReg;
    switch (s->k) {
      case SK::Decl: {
        checkType(s->type, s->line);
        need(bpOf(s->type) < 0, "a variable of blueprint type '" + s->type + "' must be created with 'new'; "
             "write 'new " + s->type + " " + s->name + "' or 'new " + s->type + " " + s->name + " = ...'");
        declareVar(s->name, s->type, s->e1.get(), -1, s->line);
        return;
      }
      case SK::New: {
        int bpi = bpOf(s->type);
        need(bpi >= 0, "'" + s->type + "' is not a blueprint");
        need(!(s->hasArgs && s->e1),
             "'new " + s->type + " " + s->name + "': cannot combine constructor arguments with '= value'");
        if (s->e1) {
          declareVar(s->name, s->type, s->e1.get(), -1, s->line);
        } else {
          if (s->hasArgs)
            need(bps[bpi].ctor >= 0, "blueprint '" + s->type + "' has no init, so 'new " + s->type + " " + s->name +
                                         "(...)' is not allowed; use 'new " + s->type + " " + s->name + "'");
          declareVar(s->name, s->type, nullptr, bpi, s->line, s->hasArgs ? &s->args : nullptr);
        }
        return;
      }
      case SK::Assign: assignStmt(s); break;
      case SK::ExprS: exp(s->e1.get()); break;
      case SK::If: ifStmt(s); break;
      case SK::Equals: equalsStmt(s); break;
      case SK::While: whileStmt(s); break;
      case SK::From: fromStmt(s); break;
      case SK::IterA: iterStmt(s, false); break;
      case SK::IterD: iterStmt(s, true); break;
      case SK::Add: addStmt(s); break;
      case SK::Append: appendStmt(s); break;
      case SK::Prepend: prependStmt(s); break;
      case SK::Give: {
        need(!fs->isMain, "'give' can only be used inside a function");
        if (fs->ret == "void") {
          need(!s->e1, "a void function cannot give a value");
          emit(OP_RETV);
        } else {
          need((bool)s->e1, "'give' needs a value of type '" + fs->ret + "'");
          R v = rk(s->e1.get(), fs->ret);
          curLine = s->line;
          need(v.type == fs->ret, "function must give '" + fs->ret + "', got '" + v.type + "'");
          emit(OP_RET, v.reg);
        }
        break;
      }
      case SK::Next:
        need(!fs->loops.empty(), "'next' can only be used inside a loop");
        emit(OP_JMP, fs->loops.back().start);
        break;
      case SK::Quit:
        need(!fs->loops.empty(), "'quit' can only be used inside a loop");
        fs->loops.back().breaks.push_back(emit(OP_JMP, 0));
        break;
      case SK::RemoveA: case SK::RemoveD: {
        bool isA = s->k == SK::RemoveA;
        Expr* t = s->e1.get();
        need(t->k == EK::Index, isA ? "remove_a expects an array element, e.g. remove_a array:3"
                                    : "remove_d expects a dictionary element, e.g. remove_d dict:\"key\"");
        R b = exp(t->kids[0].get());
        R i = rk(t->kids[1].get());
        curLine = s->line;
        if (isA) need(isArr(b.type) && i.type == "num", "remove_a expects an array and a num index");
        else need(isDict(b.type) && i.type == "str", "remove_d expects a dictionary and a str key");
        emit(isA ? OP_AREMOVE : OP_DREMOVE, b.reg, i.reg);
        break;
      }
      case SK::ClearA: case SK::ClearD: {
        bool isA = s->k == SK::ClearA;
        R b = exp(s->e1.get());
        curLine = s->line;
        need(isA ? isArr(b.type) : isDict(b.type),
             std::string(isA ? "clear_a" : "clear_d") + " expects " + (isA ? "an array" : "a dictionary") + ", got '" + b.type + "'");
        emit(isA ? OP_ACLEAR : OP_DCLEAR, b.reg);
        break;
      }
      case SK::Func: case SK::Blueprint: case SK::States: break;
    }
    fs->nextReg = save;
  }

  void ifStmt(Stmt* s) {
    std::vector<int> ends;
    for (size_t i = 0; i < s->conds.size(); i++) {
      int save = fs->nextReg;
      R c = exp(s->conds[i].get());
      need(c.type == "bool", "condition must be bool, got '" + c.type + "'");
      int jf = emit(OP_JMPF, c.reg, 0);
      fs->nextReg = save;
      block(s->blocks[i]);
      if (i + 1 < s->conds.size() || s->hasElse) ends.push_back(emit(OP_JMP, 0));
      patch(jf);
    }
    if (s->hasElse) block(s->body);
    for (int j : ends) patch(j);
  }

  void equalsStmt(Stmt* s) {
    int save = fs->nextReg;
    int rs = alloc();
    R subj = exp(s->e1.get(), rs);
    curLine = s->line;
    need(isScalar(subj.type) || statesByName.count(subj.type) != 0,
         "'equals' works with num, str, bool or a states type, got '" + subj.type + "'");
    std::vector<std::vector<int>> toBody(s->cases.size());
    for (size_t ci = 0; ci < s->cases.size(); ci++) {
      for (auto& v : s->cases[ci].values) {
        int sv = fs->nextReg;
        R val = rk(v.get(), subj.type);
        curLine = v->line;
        need(val.type == subj.type, "case value of type '" + val.type + "' cannot be compared with '" + subj.type + "'");
        int t = alloc();
        emit(OP_EQ, t, rs, val.reg);
        toBody[ci].push_back(emit(OP_JMPT, t, 0));
        fs->nextReg = sv;
      }
    }
    int jElse = emit(OP_JMP, 0);
    std::vector<int> ends;
    for (size_t ci = 0; ci < s->cases.size(); ci++) {
      for (int j : toBody[ci]) patch(j);
      block(s->cases[ci].body);
      ends.push_back(emit(OP_JMP, 0));
    }
    patch(jElse);
    if (s->hasElse) block(s->body);
    for (int j : ends) patch(j);
    fs->nextReg = save;
  }

  void closeLoop(int exitJump) {
    patch(exitJump);
    for (int b : fs->loops.back().breaks) patch(b);
    fs->loops.pop_back();
  }

  void whileStmt(Stmt* s) {
    int start = here();
    int save = fs->nextReg;
    R c = exp(s->e1.get());
    need(c.type == "bool", "'while' condition must be bool, got '" + c.type + "'");
    int jf = emit(OP_JMPF, c.reg, 0);
    fs->nextReg = save;
    fs->loops.push_back(Loop{{}, start});
    block(s->body);
    emit(OP_JMP, start);
    closeLoop(jf);
  }

  void fromStmt(Stmt* s) {
    beginScope();
    int r = alloc();
    R a = exp(s->e1.get(), r, "num");
    curLine = s->line; need(a.type == "num", "'from' start value must be num, got '" + a.type + "'");
    fs->nextReg = r + 1;
    int re = alloc();
    R b = exp(s->e2.get(), re, "num");
    curLine = s->line; need(b.type == "num", "'from' end value must be num, got '" + b.type + "'");
    fs->nextReg = re + 1;
    int rs = alloc();
    R c = exp(s->e3.get(), rs, "num");
    curLine = s->line; need(c.type == "num", "'from' step must be num, got '" + c.type + "'");
    fs->nextReg = rs + 1;
    fs->locals.push_back(Local{s->name, "num", r});
    int jChk = emit(OP_JMP, 0);
    int start = emit(OP_FORSTEP, r);
    patch(jChk);
    emit(OP_FORLOOP, r);
    int jx = emit(OP_JMP, 0);
    fs->loops.push_back(Loop{{}, start});
    block(s->body);
    emit(OP_JMP, start);
    closeLoop(jx);
    endScope();
  }

  void iterStmt(Stmt* s, bool dict) {
    beginScope();
    int ra = alloc();
    R c = exp(s->e1.get(), ra);
    curLine = s->line;
    need(dict ? isDict(c.type) : isArr(c.type),
         std::string(dict ? "iter_d expects a dictionary" : "iter_a expects an array") + ", got '" + c.type + "'");
    fs->nextReg = ra + 1;
    int ri = alloc();
    emit(OP_LOADK, ri, kNum(0));
    int start;
    if (dict) {
      int rkey = alloc(), rval = alloc();
      fs->locals.push_back(Local{s->name, "str", rkey});
      fs->locals.push_back(Local{s->name2, elemOf(c.type), rval});
      start = emit(OP_ITERD, rkey, ra, ri);
    } else {
      int re = alloc();
      fs->locals.push_back(Local{s->name, elemOf(c.type), re});
      start = emit(OP_ITERA, re, ra, ri);
    }
    int jx = emit(OP_JMP, 0);
    fs->loops.push_back(Loop{{}, start});
    block(s->body);
    emit(OP_JMP, start);
    closeLoop(jx);
    endScope();
  }
};

// ============================================================================
// 7. Виртуальная машина и стандартная библиотека
// ============================================================================
static thread_local const Program* gProg = nullptr;
static void emitOut(lkl_State* L, const char* data, size_t len);
static Value callHost(lkl_State* L, int hostIdx, Value* args, int argc);

static std::string fmtNum(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
  char b[64];
  if (d == std::floor(d) && std::fabs(d) < 1e15) {        // целые: быстрый путь без printf
    auto r = std::to_chars(b, b + sizeof b, (long long)d);
    return std::string(b, r.ptr);                          // -0.0 -> "0" (целое 0)
  }
  snprintf(b, sizeof b, "%.15g", d);
  return b;
}

static void repr(const Value& v, std::string& out, bool quote, int depth) {
  switch (v.t) {
    case VT::Nil: out += "nil"; break;
    case VT::Num: out += fmtNum(v.n); break;
    case VT::Bool: out += v.b ? "true" : "false"; break;
    case VT::Str:
      if (quote) { out += '"'; out += v.str()->s; out += '"'; } else out += v.str()->s;
      break;
    case VT::Arr: {
      if (depth > 16) { out += "[...]"; break; }
      out += '[';
      bool first = true;
      for (auto& x : v.arr()->v) { if (!first) out += ", "; first = false; repr(x, out, true, depth + 1); }
      out += ']';
      break;
    }
    case VT::Dict: {
      if (depth > 16) { out += "[...]"; break; }
      out += '[';
      bool first = true;
      for (auto& kv : v.dict()->items) {
        if (!first) out += ", ";
        first = false;
        out += '"'; out += kv.first; out += "\"=";
        repr(kv.second, out, true, depth + 1);
      }
      out += ']';
      break;
    }
    case VT::Obj: out += "<" + (gProg ? gProg->bps[v.inst()->bp].name : std::string("object")) + ">"; break;
  }
}

static bool veq(const Value& a, const Value& b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case VT::Nil: return true;
    case VT::Num: return a.n == b.n;
    case VT::Bool: return a.b == b.b;
    case VT::Str: return a.str()->s == b.str()->s;
    default: return a.h == b.h;
  }
}

static std::string trimStr(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isspace((unsigned char)s[b])) b++;
  while (e > b && isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}
static bool parseNum(const std::string& s0, double& out) {
  std::string s = trimStr(s0);
  if (s.empty()) return false;
  for (char c : s)
    if (!(isdigit((unsigned char)c) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) return false;
  char* end = nullptr;
  out = strtod(s.c_str(), &end);
  return *end == 0;
}
static size_t u8len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }
static size_t u8step(const std::string& s, size_t pos) { return std::min(u8len((unsigned char)s[pos]), s.size() - pos); }
static size_t u8count(const std::string& s, size_t from, size_t to) {
  size_t n = 0;
  for (size_t i = from; i < to; n++) i += std::min(u8step(s, i), to - i);
  return n;
}
static size_t u8skip(const std::string& s, size_t from, size_t nChars) {
  size_t i = from;
  for (; nChars > 0 && i < s.size(); nChars--) i += u8step(s, i);
  return i;
}
static size_t clampCount(double d, size_t mx) {
  if (!(d > 0)) return 0;
  return d >= (double)mx ? mx : (size_t)d;
}
static constexpr size_t MAX_STR_BYTES = (size_t)1 << 28;
static size_t toIndex(double d, size_t size, bool allowEnd, const char* what) {
  if (d != std::floor(d)) throw RtErr(std::string(what) + " index must be an integer, got " + fmtNum(d));
  if (d < 0 || d > (double)size || (!allowEnd && d >= (double)size))
    throw RtErr(std::string(what) + " index out of range: " + fmtNum(d) + " (size " + std::to_string(size) + ")");
  return (size_t)d;
}

static std::mt19937_64& rng() {
  static thread_local std::mt19937_64 g(
      (uint64_t)std::time(nullptr) ^ 0x9E3779B97F4A7C15ULL ^
      ((uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id()) * 0xBF58476D1CE4E5B9ULL) ^
      (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
  return g;
}

static std::mt19937_64& seededRng() {
  static thread_local std::mt19937_64 g(
      (uint64_t)std::time(nullptr) ^
      ((uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id()) * 0xBF58476D1CE4E5B9ULL) ^
      (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
  return g;
}

static double getDelta() {
  static thread_local auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - last).count();
  last = now;
  return dt;
}

static const char* vtName(VT t) {
  switch (t) {
    case VT::Nil: return "nil";
    case VT::Num: return "num";
    case VT::Bool: return "bool";
    case VT::Str: return "str";
    case VT::Arr: return "array";
    case VT::Dict: return "dictionary";
    case VT::Obj: return "object";
  }
  return "value";
}

// ============================================================================
// Мини-парсер Laitkil-литералов для parse_json():  ["k"="v", [a, b, c]]
// ============================================================================
static constexpr int MAX_JSON_DEPTH = 64;

static void jsonSkipWS(const std::string& s, size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

static Value jsonParseString(const std::string& s, size_t& i) {
  i++; // открывающая кавычка
  std::string out;
  while (i < s.size() && s[i] != '"') {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char e = s[i + 1]; i += 2;
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        case '"': out += '"';  break;
        case '\\': out += '\\'; break;
        default: throw RtErr(std::string("parse_json: unknown escape sequence '\\") + e + "'");
      }
    } else {
      out += s[i++];
    }
  }
  if (i >= s.size()) throw RtErr("parse_json: unterminated string literal");
  i++; // закрывающая кавычка
  return mkStr(out);
}

static Value jsonParseValue(const std::string& s, size_t& i, int depth);
// Я крутой, я мощный, я убийца Lua
static Value jsonParseArrayOrDict(const std::string& s, size_t& i, int depth) {
  if (depth > MAX_JSON_DEPTH)
    throw RtErr("parse_json: nesting is too deep (limit " + std::to_string(MAX_JSON_DEPTH) + ")");
  i++; // '['
  jsonSkipWS(s, i);
  if (i >= s.size()) throw RtErr("parse_json: unexpected end of input (expected ']')");
  if (s[i] == ']') { i++; return mkArr(); }

  // Определяем, словарь это или массив: ищем `"..." =`
  bool isDict = false;
  if (s[i] == '"') {
    size_t j = i + 1;
    while (j < s.size() && s[j] != '"') {
      if (s[j] == '\\' && j + 1 < s.size()) j += 2; else j++;
    }
    if (j < s.size()) j++;
    jsonSkipWS(s, j);
    if (j < s.size() && s[j] == '=') isDict = true;
  }

  if (isDict) {
    Value dict = mkDict();
    for (;;) {
      jsonSkipWS(s, i);
      if (i >= s.size() || s[i] != '"')
        throw RtErr("parse_json: expected a string key in dictionary");
      Value k = jsonParseString(s, i);
      jsonSkipWS(s, i);
      if (i >= s.size() || s[i] != '=')
        throw RtErr("parse_json: expected '=' after dictionary key");
      i++;
      jsonSkipWS(s, i);
      Value v = jsonParseValue(s, i, depth + 1);
      dict.dict()->set(k.str()->s, v);
      jsonSkipWS(s, i);
      if (i >= s.size()) throw RtErr("parse_json: unexpected end of input (expected ',' or ']')");
      if (s[i] == ']') { i++; break; }
      if (s[i] != ',')
        throw RtErr(std::string("parse_json: expected ',' or ']', got '") + s[i] + "'");
      i++;
    }
    return dict;
  }

  Value arr = mkArr();
  for (;;) {
    Value v = jsonParseValue(s, i, depth + 1);
    arr.arr()->v.push_back(v);
    jsonSkipWS(s, i);
    if (i >= s.size()) throw RtErr("parse_json: unexpected end of input (expected ',' or ']')");
    if (s[i] == ']') { i++; break; }
    if (s[i] != ',')
      throw RtErr(std::string("parse_json: expected ',' or ']', got '") + s[i] + "'");
    i++;
  }
  return arr;
}

static Value jsonParseValue(const std::string& s, size_t& i, int depth) {
  jsonSkipWS(s, i);
  if (i >= s.size()) throw RtErr("parse_json: unexpected end of input");
  char c = s[i];
  if (c == '"') return jsonParseString(s, i);
  if (c == '[') return jsonParseArrayOrDict(s, i, depth);
  if (s.compare(i, 4, "true") == 0)
    throw RtErr("parse_json: booleans are not supported by json_str (only strings, arrays, dictionaries)");
  if (s.compare(i, 5, "false") == 0)
    throw RtErr("parse_json: booleans are not supported by json_str (only strings, arrays, dictionaries)");
  if (c == '-' || (c >= '0' && c <= '9'))
    throw RtErr("parse_json: numbers are not supported by json_str (only strings, arrays, dictionaries)");
  throw RtErr(std::string("parse_json: unexpected character '") + c + "'");
}

static Value parseJsonTop(const std::string& s) {
  size_t i = 0;
  Value v = jsonParseValue(s, i, 0);
  jsonSkipWS(s, i);
  if (i != s.size()) throw RtErr("parse_json: trailing characters after the top-level value");
  return v;
}

static Value callBuiltin(int id, Value* a, int argc, lkl_State* L) {
  namespace fs = std::filesystem;
  auto N = [&](int i) { return a[i].n; };
  auto S = [&](int i) -> const std::string& { return a[i].str()->s; };
  switch (id) {
    case B_PRINT: case B_WRITE: {
      std::string out;
      for (int i = 0; i < argc; i++) { if (i) out += ' '; repr(a[i], out, false, 0); }
      if (id == B_PRINT) out += '\n';
      emitOut(L, out.data(), out.size());
      return Value();
    }
    case B_INPUT_STR: case B_INPUT_NUM: case B_INPUT_BOOL: {
      fflush(stdout);
      std::string line;
      if (!std::getline(std::cin, line)) line.clear();
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (id == B_INPUT_STR) return mkStr(line);
      if (id == B_INPUT_NUM) {
        double d;
        if (!parseNum(line, d)) throw RtErr("input: expected a num, got \"" + line + "\"");
        return Value::num(d);
      }
      std::string t = trimStr(line);
      std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)tolower(c); });
      if (t == "true" || t == "yes" || t == "1") return Value::boolean(true);
      if (t == "false" || t == "no" || t == "0") return Value::boolean(false);
      throw RtErr("input: expected a bool (true/false), got \"" + line + "\"");
    }
    case B_EXIT: {
      double d = N(0);
      if (!std::isfinite(d)) throw RtErr("exit: code must be a finite number");
      throw LkExit{d >= 2147483647.0 ? INT_MAX : d <= -2147483648.0 ? INT_MIN : (int)d};
    }
    case B_TO_STR: { std::string o; repr(a[0], o, false, 0); return mkStr(o); }
    case B_TO_NUM: {
      double d;
      if (!parseNum(S(0), d)) throw RtErr("to_num: cannot convert \"" + S(0) + "\" to a number");
      return Value::num(d);
    }
    case B_IS_NUM: { double d; return Value::boolean(parseNum(S(0), d)); }
    case B_ABS: return Value::num(std::fabs(N(0)));
    case B_FLOOR: return Value::num(std::floor(N(0)));
    case B_CEIL: return Value::num(std::ceil(N(0)));
    case B_ROUND: return Value::num(std::round(N(0)));
    case B_SQRT:
      if (N(0) < 0) throw RtErr("sqrt: negative argument");
      return Value::num(std::sqrt(N(0)));
    case B_SIN: return Value::num(std::sin(N(0)));
    case B_COS: return Value::num(std::cos(N(0)));
    case B_TAN: return Value::num(std::tan(N(0)));
    case B_ATAN: return Value::num(std::atan(N(0)));
    case B_EXPF: return Value::num(std::exp(N(0)));
    case B_LOG:
      if (N(0) <= 0) throw RtErr("log: argument must be positive");
      return Value::num(std::log(N(0)));
    case B_POW: return Value::num(std::pow(N(0), N(1)));
    case B_MIN: return Value::num(std::min(N(0), N(1)));
    case B_MAX: return Value::num(std::max(N(0), N(1)));
    case B_RANDOM: return Value::num(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
    case B_RANDOM_INT: {
      double lo = std::ceil(N(0)), hi = std::floor(N(1));
      if (!std::isfinite(lo) || !std::isfinite(hi) || std::fabs(lo) > 9e18 || std::fabs(hi) > 9e18)
        throw RtErr("random_int: bounds must be finite numbers within +-9e18");
      if (lo > hi) throw RtErr("random_int: empty range");
      std::uniform_int_distribution<long long> d((long long)lo, (long long)hi);
      return Value::num((double)d(rng()));
    }
    case B_PI: return Value::num(3.14159265358979323846);
    case B_CLOCK: return Value::num((double)std::clock() / (double)CLOCKS_PER_SEC);
    case B_GET_UNIX_TIME: return Value::num((double)std::time(nullptr));
    case B_LEN: {
      const Value& v = a[0];
      size_t n = v.t == VT::Str ? u8count(v.str()->s, 0, v.str()->s.size()) : v.t == VT::Arr ? v.arr()->v.size() : v.dict()->items.size();
      return Value::num((double)n);
    }
    case B_UPPER: case B_LOWER: {
      std::string r = S(0);
      for (auto& c : r) c = (char)(id == B_UPPER ? toupper((unsigned char)c) : tolower((unsigned char)c));
      return mkStr(r);
    }
    case B_TRIM: return mkStr(trimStr(S(0)));
    case B_SUBSTR: {
      const std::string& s = S(0);
      size_t nch = u8count(s, 0, s.size());
      size_t b = clampCount(N(1), nch), c = clampCount(N(2), nch - b);
      size_t bb = u8skip(s, 0, b), be = u8skip(s, bb, c);
      return mkStr(s.substr(bb, be - bb));
    }
    case B_FIND: {
      size_t p = S(0).find(S(1));
      return Value::num(p == std::string::npos ? -1.0 : (double)u8count(S(0), 0, p));
    }
    case B_REPLACE: {
      const std::string &s = S(0), &from = S(1), &to = S(2);
      if (from.empty()) return a[0];
      std::string r; size_t pos = 0;
      for (;;) {
        size_t q = s.find(from, pos);
        if (q == std::string::npos) { r.append(s, pos, std::string::npos); break; }
        r.append(s, pos, q - pos); r += to; pos = q + from.size();
      }
      return mkStr(r);
    }
    case B_STARTS_WITH: return Value::boolean(S(0).compare(0, S(1).size(), S(1)) == 0);
    case B_ENDS_WITH: {
      const std::string &s = S(0), &t = S(1);
      return Value::boolean(s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0);
    }
    case B_CONTAINS: return Value::boolean(S(0).find(S(1)) != std::string::npos);
    case B_SPLIT: {
      Value arr = mkArr(); auto& v = arr.arr()->v;
      const std::string &s = S(0), &sep = S(1);
      if (sep.empty()) {
        for (size_t i = 0; i < s.size();) {
          size_t l = std::min(u8len((unsigned char)s[i]), s.size() - i);
          v.push_back(mkStr(s.substr(i, l))); i += l;
        }
      } else {
        size_t pos = 0;
        for (;;) {
          size_t q = s.find(sep, pos);
          if (q == std::string::npos) { v.push_back(mkStr(s.substr(pos))); break; }
          v.push_back(mkStr(s.substr(pos, q - pos))); pos = q + sep.size();
        }
      }
      return arr;
    }
    case B_JOIN: {
      std::string r; bool first = true;
      for (auto& x : a[0].arr()->v) { if (!first) r += S(1); first = false; r += x.str()->s; }
      return mkStr(r);
    }
    case B_REPEAT: {
      double cnt = N(1);
      if (!(cnt >= 0) || cnt != std::floor(cnt)) throw RtErr("repeat: count must be a non-negative integer");
      const std::string& s = S(0);
      if (s.empty() || cnt == 0) return mkStr(std::string());
      if (cnt > (double)MAX_STR_BYTES / (double)s.size()) throw RtErr("repeat: result is too large (limit 256 MB)");
      size_t n = (size_t)cnt;
      std::string r; r.reserve(s.size() * n);
      for (size_t i = 0; i < n; i++) r += s;
      return mkStr(r);
    }
    case B_SYMBOL: {
      double d = N(0);
      if (d != std::floor(d) || d < 0 || d > 0x10FFFF) throw RtErr("symbol: invalid code point");
      if (d >= 0xD800 && d <= 0xDFFF) throw RtErr("symbol: surrogate code point");
      unsigned cp = (unsigned)d; std::string o;
      if (cp < 0x80) o += (char)cp;
      else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
      else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
      return mkStr(o);
    }
    case B_CODE_OF: {
      const std::string& s = S(0);
      if (s.empty()) throw RtErr("code_of: empty string");
      unsigned char c = (unsigned char)s[0];
      size_t l = u8len(c);
      if (l > s.size()) l = 1;
      unsigned cp = c;
      if (l > 1) {
        cp = c & (0xFFu >> (l + 1));
        for (size_t i = 1; i < l; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
      }
      return Value::num((double)cp);
    }
    case B_HAS: return Value::boolean(a[0].dict()->find(S(1)) != nullptr);
    case B_KEYS: {
      Value arr = mkArr();
      for (auto& kv : a[0].dict()->items) arr.arr()->v.push_back(mkStr(kv.first));
      return arr;
    }
    case B_VALUES: {
      Value arr = mkArr();
      for (auto& kv : a[0].dict()->items) arr.arr()->v.push_back(kv.second);
      return arr;
    }
    case B_COPY: {
      if (a[0].t == VT::Arr) { Value r = mkArr(); r.arr()->v = a[0].arr()->v; return r; }
      Value r = mkDict();
      r.dict()->items = a[0].dict()->items; r.dict()->index = a[0].dict()->index;
      return r;
    }
    case B_REVERSE: { auto& v = a[0].arr()->v; std::reverse(v.begin(), v.end()); return Value(); }
    case B_SORT: {
      auto& v = a[0].arr()->v;
      if (v.empty()) return Value();
      VT t = v[0].t;
      if (t != VT::Num && t != VT::Str) throw RtErr("sort: array must contain only num or str");
      for (auto& x : v) if (x.t != t) throw RtErr("sort: array elements must all have the same type");
      if (t == VT::Num)
        std::sort(v.begin(), v.end(), [](const Value& x, const Value& y) {
          return std::isnan(x.n) ? false : std::isnan(y.n) ? true : x.n < y.n; });
      else
        std::sort(v.begin(), v.end(), [](const Value& x, const Value& y) { return x.str()->s < y.str()->s; });
      return Value();
    }
    case B_SLICE: {
      auto& v = a[0].arr()->v;
      double size = (double)v.size();
      double b = std::max(0.0, std::min(N(1), size));
      double c = std::max(0.0, std::min(N(2), size - b));
      Value r = mkArr();
      for (size_t i = (size_t)b; i < (size_t)(b + c); i++) r.arr()->v.push_back(v[i]);
      return r;
    }
    case B_INDEX_OF: {
      auto& v = a[0].arr()->v;
      for (size_t i = 0; i < v.size(); i++) if (veq(v[i], a[1])) return Value::num((double)i);
      return Value::num(-1);
    }
    case B_READ_FILE: {
      std::error_code ec;
      if (!fs::is_regular_file(S(0), ec))
        throw RtErr("read_file: \"" + S(0) + "\" is not a regular file");
      std::ifstream f(S(0), std::ios::binary);
      if (!f) throw RtErr("read_file: cannot open \"" + S(0) + "\"");
      std::stringstream ss; ss << f.rdbuf();
      return mkStr(ss.str());
    }
    case B_WRITE_FILE: {
      std::ofstream f(S(0), std::ios::binary | std::ios::trunc);
      if (!f) return Value::boolean(false);
      f.write(S(1).data(), (std::streamsize)S(1).size());
      f.close();
      return Value::boolean(!f.fail());
    }
    case B_APPEND_FILE: {
      std::ofstream f(S(0), std::ios::binary | std::ios::app);
      if (!f) return Value::boolean(false);
      f.write(S(1).data(), (std::streamsize)S(1).size());
      f.close();
      return Value::boolean(!f.fail());
    }
    case B_FILE_EXISTS: {
      std::error_code ec;
      return Value::boolean(fs::is_regular_file(S(0), ec));
    }
    case B_REMOVE_FILE: {
      std::error_code ec;
      bool ok = fs::remove(S(0), ec);
      return Value::boolean(ok && !ec);
    }
    case B_DIR_EXISTS: {
      std::error_code ec;
      return Value::boolean(fs::is_directory(S(0), ec));
    }
    case B_MAKE_DIR: {
      std::error_code ec;
      fs::create_directories(S(0), ec);
      if (ec) return Value::boolean(false);
      std::error_code ec2;
      return Value::boolean(fs::is_directory(S(0), ec2));
    }
    case B_REMOVE_DIR: {
      std::error_code ec;
      fs::remove_all(S(0), ec);
      if (ec) return Value::boolean(false);
      std::error_code ec2;
      return Value::boolean(!fs::exists(S(0), ec2));
    }
    case B_LIST_DIR: {
      std::error_code ec;
      if (!fs::is_directory(S(0), ec))
        throw RtErr("list_dir: \"" + S(0) + "\" is not a directory");
      std::vector<std::string> names;
      try {
        for (const auto& e : fs::directory_iterator(S(0)))
          names.push_back(e.path().filename().string());
      } catch (const std::exception& e) {
        throw RtErr(std::string("list_dir: ") + e.what());
      }
      std::sort(names.begin(), names.end());
      Value arr = mkArr();
      for (auto& n : names) arr.arr()->v.push_back(mkStr(n));
      return arr;
    }
    case B_CURRENT_DIR: {
      try { return mkStr(fs::current_path().string()); }
      catch (const std::exception& e) { throw RtErr(std::string("current_dir: ") + e.what()); }
    }
    case B_JOIN_PATH:   return mkStr((fs::path(S(0)) / fs::path(S(1))).string());
    case B_FILE_NAME:   return mkStr(fs::path(S(0)).filename().string());
    case B_FILE_EXT:    return mkStr(fs::path(S(0)).extension().string());
    case B_PARENT_PATH: return mkStr(fs::path(S(0)).parent_path().string());

    case B_GET_DELTA: return Value::num(getDelta());
    case B_RANDOM_CHOICE: {
      auto& v = a[0].arr()->v;
      if (v.empty()) throw RtErr("random_choice: empty array");
      std::uniform_int_distribution<size_t> d(0, v.size() - 1);
      return v[d(rng())];
    }
    case B_SET_SEED: {
      double s = N(0);
      if (!std::isfinite(s)) throw RtErr("set_seed: seed must be a finite number");
      seededRng().seed((uint64_t)(int64_t)s);
      return Value();
    }
    case B_RANDOM_SEED: {
      std::uniform_int_distribution<int> d(0, 1);
      return Value::num((double)d(seededRng()));
    }
    case B_RANDOM_INT_SEED: {
      double lo = std::ceil(N(0)), hi = std::floor(N(1));
      if (!std::isfinite(lo) || !std::isfinite(hi) || std::fabs(lo) > 9e18 || std::fabs(hi) > 9e18)
        throw RtErr("random_int_seed: bounds must be finite numbers within +-9e18");
      if (lo > hi) throw RtErr("random_int_seed: empty range");
      std::uniform_int_distribution<long long> d((long long)lo, (long long)hi);
      return Value::num((double)d(seededRng()));
    }
    case B_RANDOM_CHOICE_SEED: {
      auto& v = a[0].arr()->v;
      if (v.empty()) throw RtErr("random_choice_seed: empty array");
      std::uniform_int_distribution<size_t> d(0, v.size() - 1);
      return v[d(seededRng())];
    }
    case B_PARSE_JSON: return parseJsonTop(S(0));
    case B_EX_ARRAY: {
      if (a[0].t != VT::Arr) throw RtErr(std::string("ex_array: expected an array, got ") + vtName(a[0].t));
      for (auto& v : a[0].arr()->v)
        if (v.t != VT::Str) throw RtErr("ex_array: array contains non-string elements");
      return a[0];
    }
    case B_EX_DICT: {
      if (a[0].t != VT::Dict) throw RtErr(std::string("ex_dict: expected a dictionary, got ") + vtName(a[0].t));
      for (auto& kv : a[0].dict()->items)
        if (kv.second.t != VT::Str) throw RtErr("ex_dict: dictionary contains non-string values");
      return a[0];
    }
    case B_EX_STR: {
      if (a[0].t != VT::Str) throw RtErr(std::string("ex_str: expected a string, got ") + vtName(a[0].t));
      return a[0];
    }
  }
  throw RtErr("unknown builtin");
}

class VM {
 public:
  VM(const Program& p, lkl_State* st, size_t stackSlots) : P(p), L(st), stack(stackSlots) {}

  void syncGlobals() {
    while (globals.size() < P.globalTypes.size())
      globals.push_back(defaultValue(P.globalTypes[globals.size()], &P));
  }

  Value exec(int protoIdx, size_t entryBase) {
    struct Guard {
      VM& vm; size_t depth, top; const Program* prevG;
      ~Guard() { while (vm.frames.size() > depth) vm.frames.pop(); vm.hostTop = top; gProg = prevG; }
    } guard{*this, frames.size(), hostTop, gProg};
    gProg = &P;
    const size_t entryDepth = frames.size();
    if (protoIx.size() != P.protos.size()) { protoIx.clear(); for (const Proto& pr : P.protos) protoIx.push_back(&pr); }
    const Proto* p = protoIx[(size_t)protoIdx];
    const Instr* code = p->code.data();
    const Instr* pc = code;
    const Value* K = p->consts.data();
    size_t base = entryBase;
    Value* R = stack.data() + base;
#define RKV(x) ((x) < 0 ? K[~(x)] : R[(x)])
    const Instr* ip = pc;
#define I (*ip)
#define NEXT() do { ip = pc++; goto *dispatch[ip->op]; } while (0)
    try {
      if (base + (size_t)p->maxRegs > stack.size()) throw RtErr("stack overflow: recursion is too deep");
      static const void* const dispatch[] = {
#define X(n) &&L_##n,
        OP_LIST(X)
#undef X
      };
      NEXT();
      L_LOADK:    R[I.a] = K[I.b];                                    NEXT();
      L_LOADK_ADD: R[I.a].setNum(K[I.b].n + RKV(I.c).n);              NEXT();
      L_LT_JMPF:  { const Value& x = RKV(I.b); const Value& y = RKV(I.c);
                    bool r = x.t == VT::Num ? x.n < y.n : x.str()->s < y.str()->s;
                    R[I.a].setBool(r); if (!r) pc = code + I.d; NEXT(); }
      L_MOVE_RET: { Value rv = R[I.b]; if (frames.size() == entryDepth) return rv;
                    Frame f = frames.back(); frames.pop(); p = f.p; code = p->code.data(); K = p->consts.data(); pc = f.pc;
                    base = f.base; R = stack.data() + base; R[f.dest] = std::move(rv); NEXT(); }
      L_ADD_RR: R[I.a].setNum(R[I.b].n + R[I.c].n);               NEXT();
      L_ADD_RK: R[I.a].setNum(R[I.b].n + K[I.c].n);               NEXT();
      L_ADD_KR: R[I.a].setNum(K[I.b].n + R[I.c].n);               NEXT();
      L_SUB_RR: R[I.a].setNum(R[I.b].n - R[I.c].n);               NEXT();
      L_SUB_RK: R[I.a].setNum(R[I.b].n - K[I.c].n);               NEXT();
      L_SUB_KR: R[I.a].setNum(K[I.b].n - R[I.c].n);               NEXT();
      L_MUL_RR: R[I.a].setNum(R[I.b].n * R[I.c].n);               NEXT();
      L_MUL_RK: R[I.a].setNum(R[I.b].n * K[I.c].n);               NEXT();
      L_MUL_KR: R[I.a].setNum(K[I.b].n * R[I.c].n);               NEXT();
      L_LT_JMPF_RR: { const Value& x = R[I.b]; const Value& y = R[I.c];
                    bool r = x.t == VT::Num ? x.n < y.n : x.str()->s < y.str()->s;
                    R[I.a].setBool(r); if (!r) pc = code + I.d; NEXT(); }
      L_LT_JMPF_RK: { const Value& x = R[I.b]; const Value& y = K[I.c];
                    bool r = x.t == VT::Num ? x.n < y.n : x.str()->s < y.str()->s;
                    R[I.a].setBool(r); if (!r) pc = code + I.d; NEXT(); }
      L_LT_JMPF_KR: { const Value& x = K[I.b]; const Value& y = R[I.c];
                    bool r = x.t == VT::Num ? x.n < y.n : x.str()->s < y.str()->s;
                    R[I.a].setBool(r); if (!r) pc = code + I.d; NEXT(); }
      L_CONCATG:  { Value& g = globals[I.a]; const Value& x = RKV(I.b);
                    if (g.h->rc == 1 && g.h != x.h) g.str()->s += x.str()->s;
                    else g = mkStr(g.str()->s + x.str()->s);
                    NEXT(); }
      L_LOADNIL:  R[I.a] = Value();                                   NEXT();
      L_MOVE:     R[I.a] = R[I.b];                                    NEXT();
      L_GETG:     R[I.a] = globals[I.b];                              NEXT();
      L_SETG:     globals[I.a] = RKV(I.b);                            NEXT();
      L_ADD:      R[I.a].setNum(RKV(I.b).n + RKV(I.c).n);             NEXT();
      L_SUB:      R[I.a].setNum(RKV(I.b).n - RKV(I.c).n);             NEXT();
      L_MUL:      R[I.a].setNum(RKV(I.b).n * RKV(I.c).n);             NEXT();
      L_DIV:      { double y = RKV(I.c).n; if (y == 0) throw RtErr("division by zero"); R[I.a].setNum(RKV(I.b).n / y); NEXT(); }
      L_MOD:      { double y = RKV(I.c).n; if (y == 0) throw RtErr("modulo by zero"); R[I.a].setNum(std::fmod(RKV(I.b).n, y)); NEXT(); }
      L_NEG:      R[I.a].setNum(-RKV(I.b).n);                         NEXT();
      L_NOT:      R[I.a].setBool(!RKV(I.b).b);                        NEXT();
      L_CONCAT:   { Value& d = R[I.a];
                    if (I.a == I.b && I.a != I.c && d.t == VT::Str && d.h->rc == 1) d.str()->s += RKV(I.c).str()->s;   // s ++= x: дописываем на месте
                    else d = mkStr(RKV(I.b).str()->s + RKV(I.c).str()->s);
                    NEXT(); }
      L_EQ:       R[I.a].setBool(veq(RKV(I.b), RKV(I.c)));            NEXT();
      L_NE:       R[I.a].setBool(!veq(RKV(I.b), RKV(I.c)));           NEXT();
      L_LT:       { const Value& x = RKV(I.b); const Value& y = RKV(I.c); R[I.a].setBool(x.t == VT::Num ? x.n < y.n : x.str()->s < y.str()->s); NEXT(); }
      L_LE:       { const Value& x = RKV(I.b); const Value& y = RKV(I.c); R[I.a].setBool(x.t == VT::Num ? x.n <= y.n : x.str()->s <= y.str()->s); NEXT(); }
      L_JMP:      pc = code + I.a;                                    NEXT();
      L_JMPF:     if (!R[I.a].b) pc = code + I.b;                     NEXT();
      L_JMPT:     if (R[I.a].b)  pc = code + I.b;                     NEXT();
      L_NEWARR:   R[I.a] = mkArr();                                   NEXT();
      L_NEWDICT:  R[I.a] = mkDict();                                  NEXT();
      L_APPEND:   R[I.a].arr()->v.push_back(RKV(I.b));                NEXT();
      L_AINSERT:  { auto& v = R[I.a].arr()->v; size_t i = toIndex(RKV(I.b).n, v.size(), true, "array"); Value val = RKV(I.c); v.insert(v.begin() + (std::ptrdiff_t)i, std::move(val)); NEXT(); }
      L_DSET:     R[I.a].dict()->set(RKV(I.b).str()->s, RKV(I.c));    NEXT();
      L_GETI:     { const Value& b = R[I.b]; double d = RKV(I.c).n;
                    if (b.t == VT::Arr) { auto& v = b.arr()->v; size_t i = 0;
                      if (d >= 0 && d < (double)v.size() && d == (double)(i = (size_t)d)) {} else i = toIndex(d, v.size(), false, "array");
                      R[I.a] = v[i]; }
                    else if (b.t == VT::Str) {
                      const std::string& s = b.str()->s; if (d != std::floor(d)) throw RtErr("string index must be an integer, got " + fmtNum(d));
                      size_t pos = 0, chars = 0; bool found = false;
                      for (; pos < s.size(); chars++) { size_t l = std::min(u8len((unsigned char)s[pos]), s.size() - pos);
                        if ((double)chars == d) { R[I.a] = mkStr(s.substr(pos, l)); found = true; break; } pos += l; }
                      if (!found) throw RtErr("string index out of range: " + fmtNum(d) + " (size " + std::to_string(chars) + ")");
                    }
                    else throw RtErr("cannot index a value of this type with a number");
                    NEXT(); }
      L_SETI:     { if (R[I.a].t != VT::Arr) throw RtErr("cannot assign to a non-array with a number index");
                    auto& v = R[I.a].arr()->v; size_t i = toIndex(RKV(I.b).n, v.size(), true, "array");
                    if (i == v.size()) v.push_back(RKV(I.c)); else v[i] = RKV(I.c); NEXT(); }
      L_GETD:     { if (R[I.b].t != VT::Dict) throw RtErr("cannot index a non-dictionary with a string key");
                    DictObj* d = R[I.b].dict(); const std::string& k = RKV(I.c).str()->s; Value* f = d->find(k);
                    if (!f) throw RtErr("key not found: \"" + k + "\""); R[I.a] = *f; NEXT(); }
      L_SETD:     { if (R[I.a].t != VT::Dict) throw RtErr("cannot assign to a non-dictionary with a string key");
                    R[I.a].dict()->set(RKV(I.b).str()->s, RKV(I.c)); NEXT(); }
      L_AREMOVE:  { auto& v = R[I.a].arr()->v; size_t i = toIndex(RKV(I.b).n, v.size(), false, "array"); v.erase(v.begin() + (std::ptrdiff_t)i); NEXT(); }
      L_DREMOVE:  R[I.a].dict()->remove(RKV(I.b).str()->s);           NEXT();
      L_ACLEAR:   R[I.a].arr()->v.clear();                            NEXT();
      L_DCLEAR:   R[I.a].dict()->clear();                             NEXT();
      L_ITERA:    { auto& v = R[I.b].arr()->v; size_t i = (size_t)R[I.c].n;
                    if (i < v.size()) { R[I.a] = v[i]; R[I.c].n = (double)(i + 1); pc++; } NEXT(); }
      L_ITERD:    { auto& items = R[I.b].dict()->items; size_t i = (size_t)R[I.c].n;
                    if (i < items.size()) { R[I.a] = mkStr(items[i].first); R[I.a + 1] = items[i].second; R[I.c].n = (double)(i + 1); pc++; } NEXT(); }
      L_FORSTEP:  R[I.a].n += R[I.a + 2].n;                           NEXT();
      L_FORLOOP:  { double step = R[I.a + 2].n, v = R[I.a].n; if (step == 0) throw RtErr("'from' loop: step must not be zero");
                    if (step > 0 ? v <= R[I.a + 1].n : v >= R[I.a + 1].n) pc++; NEXT(); }
      L_NEW:      { const BpRt& bp = P.bps[I.b];
                    if (!bp.tplReady) { for (auto& ft : bp.fieldTypes) bp.tpl.push_back(defaultValue(ft, &P)); bp.tplReady = true; }
                    Value o = mkInst(I.b); auto& fl = o.inst()->f; fl.reserve(bp.tpl.size());
                    for (const Value& tv : bp.tpl) fl.push_back(tv.t == VT::Arr ? mkArr() : tv.t == VT::Dict ? mkDict() : tv);
                    R[I.a] = std::move(o); NEXT(); }
      L_GETF:     { const Value& o = R[I.b]; if (o.t != VT::Obj) throw RtErr("object is not initialised (use 'new')"); R[I.a] = o.inst()->f[I.c]; NEXT(); }
      L_SETF:     { const Value& o = R[I.a]; if (o.t != VT::Obj) throw RtErr("object is not initialised (use 'new')"); o.inst()->f[I.b] = RKV(I.c); NEXT(); }
      L_CALL:     { const Proto* callee = protoIx[(size_t)I.b]; size_t nb = base + (size_t)I.c;
                    if (nb + (size_t)callee->maxRegs > stack.size() || frames.size() > 200000) throw RtErr("stack overflow: recursion is too deep");
                    frames.push(Frame{p, pc, base, I.a}); base = nb; R = stack.data() + base;
                    p = callee; code = p->code.data(); K = p->consts.data(); pc = code; NEXT(); }
      L_CALLB:    {
                    // Быстрый путь для числовых встроенных функций: без общего switch и без Value-временных.
                    const Value* a0 = &R[I.a];
                    if (a0[0].t == VT::Num && (I.c == 1 || (I.c == 2 && a0[1].t == VT::Num))) {
                      double x = a0[0].n, y = I.c == 2 ? a0[1].n : 0.0;
                      switch (I.b) {
                        case B_ABS:   R[I.a].setNum(std::fabs(x)); NEXT();
                        case B_FLOOR: R[I.a].setNum(std::floor(x)); NEXT();
                        case B_CEIL:  R[I.a].setNum(std::ceil(x)); NEXT();
                        case B_SQRT:  if (x >= 0) { R[I.a].setNum(std::sqrt(x)); NEXT(); } break;
                        case B_MIN:   R[I.a].setNum(std::min(x, y)); NEXT();
                        case B_MAX:   R[I.a].setNum(std::max(x, y)); NEXT();
                        default: break;
                      }
                    }
                    Value r = callBuiltin(I.b, &R[I.a], I.c, L); R[I.a] = std::move(r); NEXT(); }
      L_CALLH:    { hostTop = base + (size_t)p->maxRegs; Value r = callHost(L, I.b, &R[I.a], I.c); R[I.a] = std::move(r); NEXT(); }
      L_RET:      { Value rv = RKV(I.a); if (frames.size() == entryDepth) return rv;
                    Frame f = frames.back(); frames.pop(); p = f.p; code = p->code.data(); K = p->consts.data(); pc = f.pc;
                    base = f.base; R = stack.data() + base; R[f.dest] = std::move(rv); NEXT(); }
      L_RETV:     { if (frames.size() == entryDepth) return Value();
                    Frame f = frames.back(); frames.pop(); p = f.p; code = p->code.data(); K = p->consts.data(); pc = f.pc;
                    base = f.base; R = stack.data() + base; NEXT(); }
      L_NORET:    throw RtErr("function '" + p->name + "' finished without 'give'");
      throw RtErr("bad opcode");
    } catch (RtErr& e) {
      size_t idx = pc > code ? (size_t)(pc - code) - 1 : 0;
      int line = p->lines.empty() ? 0 : p->lines[std::min(idx, p->lines.size() - 1)];
      std::string where = p->name == "<main>" ? "" : " (in " + p->name + ")";
      throw LkError{"Runtime", line, e.msg + where};
    }
#undef NEXT
#undef I
#undef RKV
  }

 private:
  struct Frame { const Proto* p; const Instr* pc; size_t base; int dest; };
  const Program& P;
  lkl_State* L;
  // Стек фреймов вызовов: простой массив с ручным ростом (вместо std::vector на горячем пути CALL/RET).
  struct FrameStack {
    Frame* d = nullptr; size_t n = 0, cap = 0;
    FrameStack() = default;
    FrameStack(const FrameStack&) = delete; FrameStack& operator=(const FrameStack&) = delete;
    ~FrameStack() { std::free(d); }
    void push(const Frame& f) {
      if (n == cap) { size_t nc = cap ? cap * 2 : 256; d = static_cast<Frame*>(std::realloc(d, nc * sizeof(Frame))); if (!d) throw std::bad_alloc(); cap = nc; }
      d[n++] = f;
    }
    void pop() { --n; }
    Frame& back() { return d[n - 1]; }
    size_t size() const { return n; }
  };
  FrameStack frames;
  std::vector<const Proto*> protoIx;   // плоский индекс протов (быстрее, чем deque::operator[])

 public:
  std::vector<Value> stack, globals;
  size_t hostTop = 0;
};

// ============================================================================
// 9. LKLBridge: реализация C API
// ============================================================================
struct Slot { Value v; Type t; };

struct lkl_State {
  Program prog;
  Compiler comp;
  VM vm;
  std::vector<Slot> stack;
  size_t frameBase = 0;
  lkl_WriteFn writer = nullptr; void* writerUd = nullptr;
  void* curUd = nullptr;
  int execDepth = 0;
  std::string pending;
  bool exitPending = false;
  std::string errText; int errLine = 0, exitCode = 0;
  std::string baseDir;
  explicit lkl_State(size_t slots) : comp(prog), vm(prog, this, slots) {}
  ~lkl_State() { for (auto& h : prog.hosts) if (h.freeUd) h.freeUd(h.ud); }
};

static void emitOut(lkl_State* L, const char* data, size_t len) {
  if (L && L->writer) L->writer(data, len, L->writerUd);
  else fwrite(data, 1, len, stdout);
}

static int setErr(lkl_State* L, int code, const char* kind, int line, const std::string& msg) {
  L->errLine = line;
  L->errText = std::string(kind) + " error" + (line > 0 ? " (line " + std::to_string(line) + ")" : "") + ": " + msg;
  L->pending = msg;
  return code;
}
static int apiErr(lkl_State* L, const std::string& msg) { return setErr(L, LKL_ERR_API, "API", 0, msg); }

static int errFromException(lkl_State* L) {
  try { throw; }
  catch (const LkError& e) {
    int code = e.kind == "Syntax" ? LKL_ERR_SYNTAX
              : e.kind == "Type"   ? LKL_ERR_TYPE
              : e.kind == "File"   ? LKL_ERR_FILE
              : LKL_ERR_RUNTIME;
    return setErr(L, code, e.kind.c_str(), e.line, e.msg);
  }
  catch (const LkExit& e) {
    L->exitCode = e.code; L->exitPending = true; L->errLine = 0; L->errText = "exit(" + std::to_string(e.code) + ")";
    return LKL_EXIT;
  }
  catch (const std::bad_alloc&) { return apiErr(L, "out of memory"); }
  catch (const std::exception& e) { return apiErr(L, e.what()); }
  catch (...) { return apiErr(L, "unknown exception"); }
}

static const char* scalarTypeOf(const Value& v) {
  switch (v.t) { case VT::Num: return "num"; case VT::Bool: return "bool"; default: return "str"; }
}

static Value callHost(lkl_State* L, int hostIdx, Value* args, int argc) {
  const HostFn& h = L->prog.hosts[(size_t)hostIdx];
  lkl_CFunction fn = h.fn; void* ud = h.ud;
  size_t newBase = L->stack.size();
  for (int i = 0; i < argc; i++)
    L->stack.push_back(Slot{args[i], (size_t)i < h.params.size() ? h.params[(size_t)i] : Type(scalarTypeOf(args[i]))});
  size_t savedFrame = L->frameBase; void* savedUd = L->curUd;
  L->frameBase = newBase; L->curUd = ud; L->pending.clear(); L->exitPending = false;
  int n;
  try { n = fn(L); }
  catch (const std::exception& e) { n = lkl_error(L, "%s", e.what()); }
  catch (const LkExit& e) { L->exitCode = e.code; L->exitPending = true; n = -1; }
  catch (...) { n = lkl_error(L, "unknown exception in host function '%s'", h.name.c_str()); }
  L->frameBase = savedFrame; L->curUd = savedUd;

  Value result; std::string err;
  if (n < 0) err = L->pending.empty() ? "host function '" + h.name + "' failed" : L->pending;
  else if (h.ret != "void") {
    if (n < 1 || L->stack.size() < newBase + 1) err = "host function '" + h.name + "' must return a value of type '" + h.ret + "'";
    else if (L->stack.back().t != h.ret)
      err = "host function '" + h.name + "' returned '" + L->stack.back().t + "', but its signature says '" + h.ret + "'";
    else result = std::move(L->stack.back().v);
  }
  L->stack.resize(newBase);
  if (!err.empty()) {
    if (L->exitPending) throw LkExit{L->exitCode};
    throw RtErr(err);
  }
  return result;
}

lkl_State* lkl_newstateex(size_t slots) {
  try { return new lkl_State(slots < 256 ? 256 : slots); } catch (...) { return nullptr; }
}
lkl_State* lkl_newstate(void) { return lkl_newstateex(1u << 18); }
void lkl_close(lkl_State* L) { delete L; }

static int runSource(lkl_State* L, const std::string& src, bool disassemble = false);
static void disassemble(const std::deque<Proto>& protos);

int lkl_dobuffer(lkl_State* L, const char* source, size_t len) {
  return runSource(L, std::string(source ? source : "", source ? len : 0));
}
int lkl_dostring(lkl_State* L, const char* source) { return runSource(L, source ? source : ""); }
int lkl_dofile(lkl_State* L, const char* path) {
  std::string p = path ? path : "";
  std::ifstream f(p.c_str(), std::ios::binary);
  if (!f) return setErr(L, LKL_ERR_FILE, "File", 0, "cannot open '" + p + "'");
  std::stringstream ss; ss << f.rdbuf();
  std::string savedBase = L->baseDir;
  L->baseDir = std::filesystem::path(p).parent_path().string();
  int rc = runSource(L, ss.str());
  L->baseDir = savedBase;
  return rc;
}

static int runSource(lkl_State* L, const std::string& src, bool dis) {
  if (!L) return LKL_ERR_API;
  if (L->execDepth > 0) return apiErr(L, "cannot load code while the VM is running (call lkl_call instead)");
  L->errText.clear(); L->errLine = 0; L->exitPending = false;
  try {
    Parser parser(lex(src, L->baseDir));
    Block ast = parser.parseProgram();
    if (dis) {
      std::deque<Proto> dump;
      L->comp.compileChunk(ast, &dump);
      disassemble(dump);
      return LKL_OK;
    }
    int pi = L->comp.compileChunk(ast);
    L->vm.syncGlobals();
    struct Depth { lkl_State* s; Depth(lkl_State* x) : s(x) { s->execDepth++; } ~Depth() { s->execDepth--; } } depth(L);
    L->vm.exec(pi, L->vm.hostTop);
    L->prog.protos.pop_back();
  } catch (...) {
    if (!L->prog.protos.empty() && L->prog.protos.back().name == "<main>") L->prog.protos.pop_back();
    return errFromException(L);
  }
  return LKL_OK;
}

const char* lkl_geterror(lkl_State* L) { return L ? L->errText.c_str() : ""; }
int lkl_errorline(lkl_State* L) { return L ? L->errLine : 0; }
int lkl_exitcode(lkl_State* L) { return L ? L->exitCode : 0; }
int lkl_error(lkl_State* L, const char* fmt, ...) {
  va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  std::string s((size_t)(n < 0 ? 0 : n), '\0');
  if (n > 0) vsnprintf(&s[0], s.size() + 1, fmt, ap2);
  va_end(ap); va_end(ap2);
  L->pending = s;
  return -1;
}

static bool validTypeSyntax(const std::string& t) {
  size_t i = 0;
  while (t.size() > i + 2 && (t[i] == 'a' || t[i] == 'd') && t[i + 1] == '_') i += 2;
  if (i >= t.size()) return false;
  for (size_t k = i; k < t.size(); k++) {
    unsigned char c = (unsigned char)t[k];
    if (!(isalnum(c) || c == '_' || c >= 0x80) || (k == i && isdigit(c))) return false;
  }
  return t != "void";
}
static bool validIdent(const std::string& n) {
  if (n.empty() || isdigit((unsigned char)n[0]) || KEYWORDS.count(n)) return false;
  for (unsigned char c : n) if (!(isalnum(c) || c == '_' || c >= 0x80)) return false;
  return true;
}

int lkl_registerx(lkl_State* L, const char* name, lkl_CFunction fn, const char* sig, void* ud, lkl_FreeFn freeUd) {
  if (!L) return LKL_ERR_API;
  auto bail = [&](const std::string& m) { if (freeUd) freeUd(ud); return apiErr(L, m); };
  std::string nm = name ? name : "";
  if (!fn) return bail("lkl_register: function pointer is NULL");
  if (!validIdent(nm)) return bail("lkl_register: invalid function name '" + nm + "'");
  HostFn h; h.name = nm; h.fn = fn; h.ud = ud; h.freeUd = freeUd;
  std::istringstream ss(sig ? sig : ""); std::string w; bool arrow = false; int rets = 0;
  while (ss >> w) {
    if (w == "->") { if (arrow) return bail("lkl_register '" + nm + "': second '->' in signature"); arrow = true; }
    else if (arrow) { h.ret = w; rets++; }
    else if (w == "...") h.variadic = true;
    else {
      if (h.variadic) return bail("lkl_register '" + nm + "': '...' must be the last parameter");
      if (!validTypeSyntax(w)) return bail("lkl_register '" + nm + "': bad parameter type '" + w + "'");
      h.params.push_back(w);
    }
  }
  if (rets > 1) return bail("lkl_register '" + nm + "': expected at most one result type after '->'");
  if (h.ret != "void" && !validTypeSyntax(h.ret)) return bail("lkl_register '" + nm + "': bad result type '" + h.ret + "'");
  if (L->comp.hasFunc(nm)) return bail("lkl_register: '" + nm + "' is already a function of the script");
  auto it = L->prog.hostByName.find(nm);
  if (it != L->prog.hostByName.end()) {
    HostFn& o = L->prog.hosts[(size_t)it->second];
    if (o.params != h.params || o.ret != h.ret || o.variadic != h.variadic)
      return bail("lkl_register: '" + nm + "' was already registered with a different signature");
    if (o.freeUd) o.freeUd(o.ud);
    o.fn = fn; o.ud = ud; o.freeUd = freeUd;
    return LKL_OK;
  }
  L->prog.hostByName[nm] = (int)L->prog.hosts.size();
  L->prog.hosts.push_back(std::move(h));
  return LKL_OK;
}
int lkl_register(lkl_State* L, const char* name, lkl_CFunction fn, const char* sig) {
  return lkl_registerx(L, name, fn, sig, nullptr, nullptr);
}
void* lkl_getuserdata(lkl_State* L) { return L ? L->curUd : nullptr; }
void lkl_setwriter(lkl_State* L, lkl_WriteFn fn, void* ud) { if (L) { L->writer = fn; L->writerUd = ud; } }

static Slot* at(lkl_State* L, int idx) {
  long long pos = idx > 0 ? (long long)L->frameBase + idx - 1 : idx < 0 ? (long long)L->stack.size() + idx : -1;
  if (pos < (long long)L->frameBase || pos >= (long long)L->stack.size()) return nullptr;
  return &L->stack[(size_t)pos];
}
#define NEED_SLOT(var, idx) Slot* var = at(L, idx); if (!var) return apiErr(L, "invalid stack index " + std::to_string(idx))
static void push(lkl_State* L, Value v, Type t) { L->stack.push_back(Slot{std::move(v), std::move(t)}); }

int lkl_gettop(lkl_State* L) { return (int)(L->stack.size() - L->frameBase); }
int lkl_settop(lkl_State* L, int idx) {
  long long top = (long long)(L->stack.size() - L->frameBase);
  long long n = idx >= 0 ? idx : top + idx + 1;
  if (n < 0 || n > top) return apiErr(L, "lkl_settop: cannot grow the stack or go below its bottom");
  L->stack.resize(L->frameBase + (size_t)n);
  return LKL_OK;
}
int lkl_absindex(lkl_State* L, int idx) {
  if (!at(L, idx)) return 0;
  return idx > 0 ? idx : (int)(L->stack.size() - L->frameBase) + idx + 1;
}
int lkl_pushvalue(lkl_State* L, int idx) { NEED_SLOT(s, idx); Slot c = *s; L->stack.push_back(std::move(c)); return LKL_OK; }

int lkl_type(lkl_State* L, int idx) {
  Slot* s = at(L, idx); if (!s) return LKL_TNONE;
  switch (s->v.t) {
    case VT::Nil: return LKL_TNIL; case VT::Num: return LKL_TNUM; case VT::Bool: return LKL_TBOOL;
    case VT::Str: return LKL_TSTR; case VT::Arr: return LKL_TARRAY; case VT::Dict: return LKL_TDICT; default: return LKL_TOBJECT;
  }
}
const char* lkl_typename(lkl_State* L, int idx) { Slot* s = at(L, idx); return s ? s->t.c_str() : nullptr; }

void lkl_pushnum(lkl_State* L, double v) { push(L, Value::num(v), "num"); }
void lkl_pushbool(lkl_State* L, int v) { push(L, Value::boolean(v != 0), "bool"); }
void lkl_pushstring(lkl_State* L, const char* s) { push(L, mkStr(s ? s : ""), "str"); }
void lkl_pushlstring(lkl_State* L, const char* s, size_t len) { push(L, mkStr(std::string(s ? s : "", s ? len : 0)), "str"); }
static int pushContainer(lkl_State* L, const char* elem, bool arr) {
  std::string e = elem ? elem : "";
  if (!validTypeSyntax(e)) return apiErr(L, "invalid element type '" + e + "'");
  if (!L->comp.typeKnown(e)) return apiErr(L, "unknown element type '" + e + "'");
  push(L, arr ? mkArr() : mkDict(), (arr ? "a_" : "d_") + e);
  return LKL_OK;
}
int lkl_pusharray(lkl_State* L, const char* elem) { return pushContainer(L, elem, true); }
int lkl_pushdict(lkl_State* L, const char* elem) { return pushContainer(L, elem, false); }
int lkl_pushnull(lkl_State* L, const char* bp) {
  if (!bp || L->comp.blueprintIndex(bp) < 0) return apiErr(L, std::string("unknown blueprint '") + (bp ? bp : "") + "'");
  push(L, Value(), bp); return LKL_OK;
}

double lkl_tonum(lkl_State* L, int idx) { Slot* s = at(L, idx); return s && s->v.t == VT::Num ? s->v.n : 0; }
int lkl_tobool(lkl_State* L, int idx) { Slot* s = at(L, idx); return s && s->v.t == VT::Bool && s->v.b ? 1 : 0; }
const char* lkl_tostring(lkl_State* L, int idx, size_t* len) {
  Slot* s = at(L, idx);
  if (!s || s->v.t != VT::Str) { if (len) *len = 0; return nullptr; }
  if (len) *len = s->v.str()->s.size();
  return s->v.str()->s.c_str();
}

#define NEED_ARR(var, idx) NEED_SLOT(var, idx); if (var->v.t != VT::Arr) return apiErr(L, "value at index " + std::to_string(idx) + " is not an array")
#define NEED_DICT(var, idx) NEED_SLOT(var, idx); if (var->v.t != VT::Dict) return apiErr(L, "value at index " + std::to_string(idx) + " is not a dictionary")
static int takeTop(lkl_State* L, const Type& want, Value& out, const char* what) {
  if (L->stack.size() <= L->frameBase) return apiErr(L, std::string(what) + ": the stack is empty");
  Slot& t = L->stack.back();
  if (t.t != want) return apiErr(L, std::string(what) + ": expected a value of type '" + want + "', got '" + t.t + "'");
  out = t.v; return LKL_OK;
}

long lkl_arraylen(lkl_State* L, int idx) { Slot* s = at(L, idx); return s && s->v.t == VT::Arr ? (long)s->v.arr()->v.size() : -1; }
int lkl_arrayget(lkl_State* L, int idx, size_t i) {
  NEED_ARR(s, idx);
  auto& v = s->v.arr()->v;
  if (i >= v.size()) return apiErr(L, "array index out of range: " + std::to_string(i) + " (size " + std::to_string(v.size()) + ")");
  Value e = v[i]; Type t = elemOf(s->t);
  push(L, std::move(e), std::move(t)); return LKL_OK;
}
int lkl_arrayset(lkl_State* L, int idx, size_t i) {
  NEED_ARR(s, idx);
  ArrObj* a = s->v.arr(); Value x;
  if (int rc = takeTop(L, elemOf(s->t), x, "lkl_arrayset")) return rc;
  if (i > a->v.size()) return apiErr(L, "array index out of range: " + std::to_string(i) + " (size " + std::to_string(a->v.size()) + ")");
  if (i == a->v.size()) a->v.push_back(std::move(x)); else a->v[i] = std::move(x);
  L->stack.pop_back(); return LKL_OK;
}
int lkl_arrayappend(lkl_State* L, int idx) {
  NEED_ARR(s, idx);
  ArrObj* a = s->v.arr(); Value x;
  if (int rc = takeTop(L, elemOf(s->t), x, "lkl_arrayappend")) return rc;
  a->v.push_back(std::move(x)); L->stack.pop_back(); return LKL_OK;
}
int lkl_arrayremove(lkl_State* L, int idx, size_t i) {
  NEED_ARR(s, idx);
  auto& v = s->v.arr()->v;
  if (i >= v.size()) return apiErr(L, "array index out of range: " + std::to_string(i));
  v.erase(v.begin() + (std::ptrdiff_t)i); return LKL_OK;
}
int lkl_arrayclear(lkl_State* L, int idx) { NEED_ARR(s, idx); s->v.arr()->v.clear(); return LKL_OK; }

long lkl_dictlen(lkl_State* L, int idx) { Slot* s = at(L, idx); return s && s->v.t == VT::Dict ? (long)s->v.dict()->items.size() : -1; }
int lkl_dicthas(lkl_State* L, int idx, const char* key) {
  Slot* s = at(L, idx);
  if (!s || s->v.t != VT::Dict) { apiErr(L, "lkl_dicthas: not a dictionary"); return -1; }
  return s->v.dict()->find(key ? key : "") ? 1 : 0;
}
int lkl_dictget(lkl_State* L, int idx, const char* key) {
  NEED_DICT(s, idx);
  Value* f = s->v.dict()->find(key ? key : "");
  if (!f) return apiErr(L, std::string("key not found: \"") + (key ? key : "") + "\"");
  Value x = *f; Type t = elemOf(s->t);
  push(L, std::move(x), std::move(t)); return LKL_OK;
}
int lkl_dictset(lkl_State* L, int idx, const char* key) {
  NEED_DICT(s, idx);
  DictObj* d = s->v.dict(); Value x;
  if (int rc = takeTop(L, elemOf(s->t), x, "lkl_dictset")) return rc;
  d->set(key ? key : "", x); L->stack.pop_back(); return LKL_OK;
}
int lkl_dictremove(lkl_State* L, int idx, const char* key) { NEED_DICT(s, idx); s->v.dict()->remove(key ? key : ""); return LKL_OK; }
int lkl_dictclear(lkl_State* L, int idx) { NEED_DICT(s, idx); s->v.dict()->clear(); return LKL_OK; }
int lkl_dictentry(lkl_State* L, int idx, size_t i) {
  Slot* s = at(L, idx);
  if (!s || s->v.t != VT::Dict) { apiErr(L, "lkl_dictentry: not a dictionary"); return -1; }
  auto& items = s->v.dict()->items;
  if (i >= items.size()) return 0;
  Value k = mkStr(items[i].first), v = items[i].second; Type t = elemOf(s->t);
  push(L, std::move(k), "str"); push(L, std::move(v), std::move(t)); return 1;
}

int lkl_newobject(lkl_State* L, const char* bp) {
  int bi = bp ? L->comp.blueprintIndex(bp) : -1;
  if (bi < 0) return apiErr(L, std::string("unknown blueprint '") + (bp ? bp : "") + "'");
  Value o = mkInst(bi);
  for (auto& ft : L->prog.bps[(size_t)bi].fieldTypes)
    o.inst()->f.push_back(defaultValue(ft, &L->prog));
  push(L, std::move(o), bp); return LKL_OK;
}
static int fieldOf(lkl_State* L, int idx, const char* name, Slot*& s, Compiler::FieldRef& f) {
  s = at(L, idx);
  if (!s) return apiErr(L, "invalid stack index " + std::to_string(idx));
  if (!L->comp.lookupField(s->t, name ? name : "", f)) return apiErr(L, "type '" + s->t + "' has no field '" + (name ? name : "") + "'");
  if (f.priv) return apiErr(L, "field '" + std::string(name) + "' of '" + s->t + "' is private (unaccess)");
  if (s->v.t != VT::Obj) return apiErr(L, "object is not initialised (use 'new')");
  return LKL_OK;
}
int lkl_getfield(lkl_State* L, int idx, const char* name) {
  Slot* s; Compiler::FieldRef f;
  if (int rc = fieldOf(L, idx, name, s, f)) return rc;
  Value v = s->v.inst()->f[(size_t)f.idx];
  push(L, std::move(v), f.type); return LKL_OK;
}
int lkl_setfield(lkl_State* L, int idx, const char* name) {
  Slot* s; Compiler::FieldRef f;
  if (int rc = fieldOf(L, idx, name, s, f)) return rc;
  InstObj* o = s->v.inst(); Value x;
  if (int rc = takeTop(L, f.type, x, "lkl_setfield")) return rc;
  o->f[(size_t)f.idx] = std::move(x); L->stack.pop_back(); return LKL_OK;
}

int lkl_getglobal(lkl_State* L, const char* name) {
  int gi = L->comp.globalIndex(name ? name : "");
  if (gi < 0) return apiErr(L, std::string("undefined global variable '") + (name ? name : "") + "'");
  L->vm.syncGlobals();
  push(L, L->vm.globals[(size_t)gi], L->prog.globalTypes[(size_t)gi]); return LKL_OK;
}
int lkl_setglobal(lkl_State* L, const char* name) {
  std::string nm = name ? name : "";
  if (L->stack.size() <= L->frameBase) return apiErr(L, "lkl_setglobal: the stack is empty");
  Slot& t = L->stack.back();
  int gi = L->comp.globalIndex(nm);
  if (gi < 0) {
    if (!validIdent(nm)) return apiErr(L, "invalid global name '" + nm + "'");
    try { gi = L->comp.addGlobal(nm, t.t); } catch (...) { return errFromException(L); }
  } else if (L->prog.globalTypes[(size_t)gi] != t.t)
    return apiErr(L, "global '" + nm + "' has type '" + L->prog.globalTypes[(size_t)gi] + "', got '" + t.t + "'");
  L->vm.syncGlobals();
  L->vm.globals[(size_t)gi] = std::move(t.v);
  L->stack.pop_back(); return LKL_OK;
}

static int doCall(lkl_State* L, const Compiler::Sig& sg, bool method, int nargs, const std::string& name) {
  int total = nargs + (method ? 1 : 0);
  size_t avail = L->stack.size() - L->frameBase;
  if (nargs < 0 || avail < (size_t)total) {
    L->stack.resize(L->stack.size() - std::min(avail, (size_t)(nargs < 0 ? 0 : total)));
    return apiErr(L, "lkl_call '" + name + "': not enough values on the stack");
  }
  size_t first = L->stack.size() - (size_t)total, off = method ? 1 : 0;
  auto fail = [&](const std::string& m) { L->stack.resize(first); return apiErr(L, m); };
  if (method && L->stack[first].v.t != VT::Obj) return fail("object is not initialised (use 'new')");
  if ((size_t)nargs != sg.params.size())
    return fail("'" + name + "' expects " + std::to_string(sg.params.size()) + " argument(s), got " + std::to_string(nargs));
  for (size_t i = 0; i < sg.params.size(); i++)
    if (L->stack[first + off + i].t != sg.params[i])
      return fail("argument " + std::to_string(i + 1) + " of '" + name + "': expected '" + sg.params[i] + "', got '" + L->stack[first + off + i].t + "'");
  size_t base = L->vm.hostTop;
  if (base + (size_t)total > L->vm.stack.size()) return fail("stack overflow");
  for (int i = 0; i < total; i++) L->vm.stack[base + (size_t)i] = std::move(L->stack[first + (size_t)i].v);
  L->stack.resize(first);
  Value rv;
  try {
    struct Depth { lkl_State* s; Depth(lkl_State* x) : s(x) { s->execDepth++; } ~Depth() { s->execDepth--; } } depth(L);
    L->exitPending = false;
    rv = L->vm.exec(sg.proto, base);
  } catch (...) { return errFromException(L); }
  if (sg.ret != "void") push(L, std::move(rv), sg.ret);
  return LKL_OK;
}
int lkl_call(lkl_State* L, const char* func, int nargs) {
  Compiler::Sig sg; std::string nm = func ? func : "";
  if (!L->comp.lookupFunc(nm, sg)) {
    if (nargs > 0) L->stack.resize(L->stack.size() - std::min((size_t)nargs, (size_t)lkl_gettop(L)));
    return apiErr(L, "undefined function '" + nm + "'");
  }
  return doCall(L, sg, false, nargs, nm);
}
int lkl_callmethod(lkl_State* L, const char* method, int nargs) {
  std::string nm = method ? method : "";
  auto drop = [&]() {
    size_t n = nargs < 0 ? 0 : (size_t)nargs + 1;
    L->stack.resize(L->stack.size() - std::min(n, L->stack.size() - L->frameBase));
  };
  if (nargs < 0 || lkl_gettop(L) < nargs + 1) { drop(); return apiErr(L, "lkl_callmethod: not enough values on the stack"); }
  Slot& recv = L->stack[L->stack.size() - (size_t)nargs - 1];
  Compiler::Sig sg;
  if (!L->comp.lookupMethod(recv.t, nm, sg)) { std::string t = recv.t; drop(); return apiErr(L, "type '" + t + "' has no method '" + nm + "'"); }
  if (sg.priv) { drop(); return apiErr(L, "method '" + nm + "' is private (unaccess)"); }
  return doCall(L, sg, true, nargs, nm);
}

// ============================================================================
// 10. Дизассемблер (--dis) и main()
// ============================================================================
static void disassemble(const std::deque<Proto>& protos) {
  for (const Proto& f : protos) {
    printf("== %s  (params=%d, regs=%d)\n", f.name.c_str(), f.nparams, f.maxRegs);
    for (size_t j = 0; j < f.code.size(); j++) {
      const Instr& in = f.code[j];
      printf("%4zu  line %-4d %-8s %d %d %d\n", j, f.lines[j], OP_NAMES[in.op], in.a, in.b, in.c);
    }
  }
  printf("(negative operand B/C means constant with index ~x)\n");
}

#ifndef LKL_NO_MAIN
int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  bool dis = false; const char* path = "Test_Laitkil.lkl";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dis")) dis = true; else path = argv[i];
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) { fprintf(stderr, "cannot open '%s'\n", path); return 2; }
  std::stringstream ss; ss << f.rdbuf();
  std::string src = ss.str();
  lkl_State* L = lkl_newstate();
  L->baseDir = std::filesystem::path(path).parent_path().string();
  int rc = dis ? runSource(L, src, true) : lkl_dobuffer(L, src.data(), src.size());
  fflush(stdout);
  int code = 0;
  if (rc == LKL_EXIT) code = lkl_exitcode(L);
  else if (rc != LKL_OK) { fprintf(stderr, "%s\n", lkl_geterror(L)); code = 1; }
  lkl_close(L);
  return code;
}
#endif