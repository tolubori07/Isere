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

/*using namespace llvm;*/
// The lexer returns tokens [0-255] if it is an unknown
// character, otherwise one
// of these for known things.
enum Token {
  tok_eof = -1,

  // commands
  tok_fun = -2,
  tok_imp = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5
};

static std::string IdentifierStr; // filled in if tok_identifier
static double NumVal;             // filled in if tok_Num

static int getTok() {
  static int LastChar = ' ';

  // Skip any whitespace
  while (isspace(LastChar)) {
    LastChar = getchar();
  }

  // Handle alphabets
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

  // Handle single-line comments with //
  if (LastChar == '/') {
    LastChar = getchar();
    if (LastChar == '/') {
      // Consume characters until the end of the line or EOF
      do
        LastChar = getchar();
      while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

      if (LastChar != EOF)
        return getTok(); // Continue lexing after the comment
    } else {
      // Not a comment, return '/' as a token
      return '/';
    }
  }

  // Handle multi-line comments
  if (LastChar == '/') {
    LastChar = getchar();
    if (LastChar == '*') {
      // Consume characters until the end of the comment
      while (true) {
        LastChar = getchar();
        if (LastChar == EOF) {
          fprintf(stderr, "Error: Unterminated multi-line comment\n");
          return tok_eof;
        }
        if (LastChar == '*') {
          LastChar = getchar();
          if (LastChar == '/')
            break; // End of multi-line comment
        }
      }
      LastChar = getchar(); // Move to the next character after the comment
      return getTok();
    } else {
      // Not a comment; return '/' as a token
      return '/';
    }
  }

  // Check for end of file
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, just return the character as its ASCII value
  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}

/// ExprAST: Base Clss for all expression nodes
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

/// NumberExprAST: Expression class for numeric literals
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  Value *codegen() override;
};
/// VariableExprAST: Expression class referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
};

/// BinaryExpreAST: Expresson class for binary operators
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

/// callExprAST: Expression class for function calls
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
};

/// prototypeAST: represents the "prototype" for a function which captures its
/// name and its arguments names => implicitly captures the number of
/// arguments the function takes
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}
  const std::string &getName() const { return Name; }
};

/// FunctionAST: Represents a function definition itself i.e the functions
/// main body
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the
/// current token the parser is looking at.  getNextToken reads another token
/// from the lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken() { return CurTok = getTok(); }

/// helper functions for error handling
std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// Numeric literals => numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // consume that number!!!
  return std::move(result);
}

/// parenthesis expressions => parenexpr ::='('expression')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat )
  auto V = ParseExpression();
  if (!V)
    return nullptr;
  if (CurTok != ')')
    return LogError(
        "Expected parenthesis => ')', Fix the error and get back here");
  getNextToken(); // eat another )
  return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  getNextToken();
  if (CurTok != '(')
    return std::make_unique<VariableExprAST>(IdName);

  // call
  getNextToken(); // eat '('
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
        return LogError("Expected ')' or ',' in argument list");
      getNextToken();
    }
  }
  // eat the ')'
  getNextToken();

  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

/// primary
///  ::identifierexper
///  :: numberexpr
///  ::parenexpr
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

/// BinopPrecedence: This holds the precedence value for each binary operator
/// defined(BIDMAS)
static std::map<char, int> BinopPrecedence;

/// GetTokPrecedence: Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;
  // Make syre it's a declared binary operator
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}
/// binary Op RHS
///::=('+'primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();
    // If this is a binop that binds at least as tightly as the current binop,
    // consume it, otherwise we are done.
    if (TokPrec < ExprPrec)
      return LHS;
    // We are now sure this is a binary operator
    int BinOp = CurTok;
    getNextToken(); // eat binary operator

    // parse the primary expression after the binary operator
    auto RHS = ParsePrimary();
    if (!RHS)
      return nullptr;
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      // will return back here
    }
    // Merge LHS and RHS of expression
    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

/// expression
///::primary binorphs
static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS)
    return nullptr;

  return ParseBinOpRHS(0, std::move(LHS));
}

/// prototype and shi
///  ::= id'('id*')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier)
    return LogErrorP("Expected function name in prototype. C'mon mane");

  std::string FnName = IdentifierStr;
  getNextToken();
  if (CurTok != '(')
    return LogErrorP("C'mon mane; expected a '(' in prototype definition");
  // Read the list of params
  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(IdentifierStr);
  if (CurTok != ')')
    return LogErrorP("Expected ')' in prototype; stop messing around");

  // successfully parsed
  getNextToken(); // eat the ')'
  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

/// function definition ::= 'fn'
static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken(); // eat fn
  auto Proto = ParsePrototype();
  if (!Proto)
    return nullptr;
  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

/// import ::= 'import' prototype
static std::unique_ptr<PrototypeAST> ParseImport() {
  getNextToken(); // eat Import
  return ParsePrototype();
}

/// topLevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    // make an anonymous proto
    auto Proto = std::make_unique<PrototypeAST>("", std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}
static void HandleFunction() {
  if (ParseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleImport() {
  if (ParseImport()) {
    fprintf(stderr, "Parsed an extern\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (ParseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// DRIVERRRRR!!! or REPL if you want to be fancy
/// top ::= function|external|expression ';'
static void MainLoop() {
  while (true) {
    fprintf(stderr, "hg ->");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // ignore top-level semicolons.
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
  // 1 is lowest precedence.
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40; // highest.
  // Prime the first token.
  fprintf(stderr, "hg -> ");
  getNextToken();

  // Run the main "interpreter loop" now.
  MainLoop();

  return 0;
}
