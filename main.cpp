#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Token Definitions (Lexer)                                               ===//
//===----------------------------------------------------------------------===//

enum Token {
  tok_eof = -1,

  // commands
  tok_fun = -2,
  tok_imp = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5
};

static std::string IdentifierStr; // Filled in if tok_identifier
static double NumVal;             // Filled in if tok_number

/// getTok - Reads the next token from standard input.
static int getTok() {
  static int LastChar = ' ';

  // Skip any whitespace
  while (isspace(LastChar))
    LastChar = getchar();

  // Handle identifiers and keywords
  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;
    while (isalnum((LastChar = getchar())))
      IdentifierStr += LastChar;

    if (IdentifierStr == "fn")
      return tok_fun;
    if (IdentifierStr == "import")
      return tok_imp;

    return tok_identifier;
  }

  // Handle numbers
  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');
    NumVal = strtod(NumStr.c_str(), 0);
    return tok_number;
  }

  // Handle comments
  if (LastChar == '/') {
    LastChar = getchar();
    if (LastChar == '/') {
      // Single-line comment
      do
        LastChar = getchar();
      while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

      if (LastChar != EOF)
        return getTok();
    } else if (LastChar == '*') {
      // Multi-line comment
      while (true) {
        LastChar = getchar();
        if (LastChar == EOF) {
          fprintf(stderr, "Error: Unterminated multi-line comment\n");
          return tok_eof;
        }
        if (LastChar == '*') {
          LastChar = getchar();
          if (LastChar == '/')
            break;
        }
      }
      LastChar = getchar();
      return getTok();
    } else {
      return '/';
    }
  }

  // Check for end of file
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, return the character as its ASCII value
  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (AST)
//===----------------------------------------------------------------------===//

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  Value *codegen() override;
};

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value *> NamedValues;

Value *Logerror

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
};

/// PrototypeAST - Represents the "prototype" for a function.
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}
  const std::string &getName() const { return Name; }
};

/// FunctionAST - Represents a function definition.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

static int CurTok;
static int getNextToken() { return CurTok = getTok(); }

/// LogError - Helper function for error handling.
std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// ParseNumberExpr - Parse a numeric literal.
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // Consume the number
  return std::move(result);
}

/// ParseParenExpr - Parse expressions surrounded by parentheses.
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // Eat '('
  auto V = ParseExpression();
  if (!V)
    return nullptr;
  if (CurTok != ')')
    return LogError("Expected ')'");
  getNextToken(); // Eat ')'
  return V;
}

/// ParseIdentifierExpr - Parse identifiers and function calls.
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;
  getNextToken(); // Consume the identifier

  if (CurTok != '(') // Simple variable reference
    return std::make_unique<VariableExprAST>(IdName);

  // Function call
  getNextToken(); // Eat '('
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;
      if (CurTok != ',')
        return LogError("Expected ',' or ')' in argument list");
      getNextToken();
    }
  }
  getNextToken(); // Eat ')'

  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

/// ParsePrimary - Parse primary expressions.
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    return LogError("Unknown token when expecting an expression");
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  }
}

//===----------------------------------------------------------------------===//
// Driver (Main Loop)
//===----------------------------------------------------------------------===//

/// MainLoop - The main REPL loop.
static void MainLoop() {
  while (true) {
    fprintf(stderr, "hg -> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // Ignore top-level semicolons
      getNextToken();
      break;
    case tok_fun:
      HandleFunction();
      break;
    case tok_imp:
      HandleImport();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

int main() {
  // Install standard binary operators.
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40; // Highest precedence

  // Prime the first token.
  fprintf(stderr, "hg -> ");
  getNextToken();

  // Run the main REPL loop.
  MainLoop();

  return 0;
}
