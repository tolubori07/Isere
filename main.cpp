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
#include "llvm/Support/raw_ostream.h"
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

namespace {

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

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
  Value *codegen() override;
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  Value *codegen() override;
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
  Value *codegen() override;
};

/// PrototypeAST - Represents the "prototype" for a function.
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}
  Function *codegen();
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
  Function *codegen();
};
} // end of anonymous namespace

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//
/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.

/// BinopPrecedence - This holds the precedence for each binary operator that is
/// defined.
static std::map<char, int> BinopPrecedence;
static std::unique_ptr<ExprAST> ParseExpression();

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

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;

  // Make sure it's a declared binop.
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
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

/// ParseNumberExpr - Parse a numeric literal.
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // Consume the number
  return std::move(result);
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

/// binoprhs
///   ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  // If this is a binop, find its precedence.
  while (true) {
    int TokPrec = GetTokPrecedence();

    // If this is a binop that binds at least as tightly as the current binop,
    // consume it, otherwise we are done.
    if (TokPrec < ExprPrec)
      return LHS;

    // Okay, we know this is a binop.
    int BinOp = CurTok;
    getNextToken(); // eat binop

    // Parse the primary expression after the binary operator.
    auto RHS = ParsePrimary();
    if (!RHS)
      return nullptr;

    // If BinOp binds less tightly with RHS than the operator after RHS, let
    // the pending operator take RHS as its LHS.
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // Merge LHS/RHS.
    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

/// expression
///   ::= primary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS)
    return nullptr;

  return ParseBinOpRHS(0, std::move(LHS));
}

// prototype
///   ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier)
    return LogErrorP("Expected function name in prototype");

  std::string FnName = IdentifierStr;
  getNextToken();

  if (CurTok != '(')
    return LogErrorP("Expected '(' in prototype");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(IdentifierStr);
  if (CurTok != ')')
    return LogErrorP("Expected ')' in prototype");

  // success.
  getNextToken(); // eat ')'.

  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

/// function definition ::= 'fn'
static std::unique_ptr<FunctionAST> ParseFunDef() {
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

//===-----------------------------------------------------------------------==//
// CODE GEN
//===-----------------------------------------------------------------------==//
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value *> NamedValues;

//---- EXPRESSION CODE GEN ----//
Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  // find this variable in the function
  Value *V = NamedValues[Name];
  if (!V)
    LogErrorV("Unknown Variable name");
  return V;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;
  switch (Op) {
  case '+':
    return Builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder->CreateFSub(L, R, "subtmp");
  case '*':
    return Builder->CreateFMul(L, R, "multmp");
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
  default:
    return LogErrorV("invalid binary operator");
  }
}

Value *CallExprAST::codegen() {
  // Look up the name in the global module table
  Function *CalleeF = TheModule->getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unkown Function referenced");

  // argument mismatch error -> refers to if there are more/less arguments than
  // expected
  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("Incorrect number of arguments passed into function call");
  std::vector<Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen());
    if (!ArgsV.back())
      return nullptr;
  }
  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

//----------> Code gen for function prototypes <----------//
Function *PrototypeAST::codegen() {
  // Make the function type:  double(double,double) etc.
  std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
  FunctionType *FT =
      FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);
  Function *F =
      Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);
  return F;
}

Function *FunctionAST::codegen() {
  // Check for an existing function from a previous 'import' declaration
  Function *TheFunction = TheModule->getFunction(Proto->getName());

  if (!TheFunction)
    TheFunction = Proto->codegen();
  if (!TheFunction)
    return nullptr;
  if (!TheFunction->empty())
    return (Function *)LogErrorV("Function cannot be redefined");

  // create a new basic block to start insertion into
  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  // Record the function arguments in the NamedValues map
  NamedValues.clear();
  for (auto &Arg : TheFunction->args())
    NamedValues[std::string(Arg.getName())] = &Arg;
  if (Value *RetVal = Body->codegen()) {
    // Finish da function
    Builder->CreateRet(RetVal);

    // validate the generated code
    verifyFunction(*TheFunction);
    return TheFunction;
  }
  // Error reading body, remove function.
  TheFunction->eraseFromParent();
  return nullptr;
}
//= == -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
//-- -- -- -- -- -- -- -- -- -- --==
//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

static void InitializeModule() {
  // Open a new context and module.
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("my cool jit", *TheContext);

  // Create a new builder for the module.
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
  if (auto FnAST = ParseFunDef()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition:");
      FnIR->print(errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseImport()) {
    if (auto *FnIR = ProtoAST->codegen()) {
      fprintf(stderr, "Read extern: ");
      FnIR->print(errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read top-level expression:");
      FnIR->print(errs());
      fprintf(stderr, "\n");

      // Remove the anonymous expression.
      FnIR->eraseFromParent();
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
  while (true) {
    fprintf(stderr, "is-> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // ignore top-level semicolons.
      getNextToken();
      break;
    case tok_fun:
      HandleDefinition();
      break;
    case tok_imp:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {
  // Install standard binary operators.
  // 1 is lowest precedence.
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40; // highest.

  // Prime the first token.
  fprintf(stderr, "Isere Version alpha 0.1");
  fprintf(stderr, "is->");
  getNextToken();

  // Make the module, which holds all the code.
  InitializeModule();

  // Run the main "interpreter loop" now.
  MainLoop();

  // Print out all of the generated code.
  TheModule->print(errs(), nullptr);

  return 0;
}
