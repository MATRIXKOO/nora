#include "Parse.h"

#include "Casting.h"
#include "Lex.h"
#include "idpool.h"

#include "mlir/IR/Diagnostics.h"

#include <array>
#include <cassert>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unicode/unistr.h>
#include <utility>
#include <vector>

using namespace Lex;

// This is the main parse file that parses linklets generated by Racket
// Racket docs do not have a formal grammar in one place and it's indeed split
// into different pages. I have tried to collect all the relevant information
// here.
//
// Linklet grammar starts in
// https://docs.racket-lang.org/reference/linklets.html#%28tech._linklet%29
// but then you're forward to
// https://docs.racket-lang.org/reference/syntax-model.html#%28part._fully-expanded%29
// and this one still misses some important information, like the definition of
// datum for example.
//
// The following text will try to summarize what I understand from the docs, and
// as a reference for what I have implemented.
//
// Linklet grammar:
//
// linklet := (linklet [[<imported-id/renamed-id> ...] ...]
//                     [<exported-id/renamed> ...]
//               <defn-or-expr> ...)
//
// imported-id/renamed := <imported-id>
//                      | (<external-imported-id> <internal-imported-id>)
//
// exported-id/renamed := <exported-id>
//                      | (<internal-exported-id> <external-exported-id>)
//
// defn-or-expr := <defn> | <expr>                          - parseDefnOrExpr
//
// defn := (define-values (<id> ...) <expr>)                - parseDefineValues
//       | (define-syntaxes (<id> ...) <expr>)
//
// expr := <id>                                             - parseIdentifier
//       | (lambda <formals> <expr>)                        - parseLambda
//       | (case-lambda (<formals> <expr>) ...)
//       | (if <expr> <expr> <expr>)                        - parseIfCond
//       | (begin <expr> ...+)                              - parseBegin
//       | (begin0 <expr> ...+)                             - parseBegin
//       | (let-values ([<id> ...) <expr>] ...) <expr>)     - parseLetValues
//       | (letrec-values ([(<id> ...) <expr>] ...) <expr>)
//       | (set! <id> <expr>)                               - parseSetBang
//       | (quote <datum>)
//       | (with-continuation-mark <expr> <expr> <expr>)
//       | (<expr> ...+)                                    - parseApplication
//       | (%variable-reference <id>)
//       | (%variable-reference (%top . id))  <- Allowed?
//       | (%variable-reference)              <- Allowed?
//
// formals := <id>                                          - parseFormals
//          | (<id> ...+ . id)
//          | (id ...)
//
// <id> := TODO
//
// <datum> := <self-quoting-datum> | <character> | TODO
//
// <character> := TODO
//
// <self-quoting-datum> := <boolean> | <number> | <string> | <byte-string>
//
// <boolean> := #t | #f
//
// <number> := <integer> | TODO
//
// <string> := TODO
//
// <byte-string> := TODO
//
// The following is a list of parsed runtime library functions that are not part
// of the core grammar:
// - values

std::vector<ast::Linklet::idpair_t> parseLinkletExports(SourceStream &S) {
  // parse a sequence of
  // (internal-exported-id external-exported-id)
  std::vector<ast::Linklet::idpair_t> Vec;

  while (true) {
    Tok T = gettok(S);
    if (!T.is(Tok::TokType::LPAREN)) {
      S.rewind(T.size());
      return Vec;
    }

    // parse internal-exported-id
    std::optional<Tok> T1 = maybeLexIdOrNumber(S);
    if (!T1) {
      return {};
    }

    // parse external-exported-id
    std::optional<Tok> T2 = maybeLexIdOrNumber(S);
    if (!T2) {
      return {};
    }

    T = gettok(S);
    if (!T.is(Tok::TokType::RPAREN)) {
      return {};
    }

    IdPool &IP = IdPool::instance();

    Vec.push_back(std::make_pair(IP.create(T1.value().Value),
                                 IP.create(T2.value().Value)));
  }

  return Vec;
}

std::unique_ptr<ast::TLNode> Parse::parseDefn(SourceStream &S) {
  auto DefnValues = parseDefineValues(S);
  if (!DefnValues) {
    return nullptr;
  }

  return DefnValues;
}

std::unique_ptr<ast::TLNode> Parse::parseDefnOrExpr(SourceStream &S) {
  std::unique_ptr<ast::TLNode> D = parseDefn(S);
  if (D) {
    return D;
  }
  std::unique_ptr<ast::ExprNode> E = parseExpr(S);
  return dyn_castU<ast::TLNode>(E);
}

std::unique_ptr<ast::Integer> Parse::parseInteger(SourceStream &S) {
  std::optional<Tok> Num = gettok(S);
  if (!Num->is(Tok::TokType::NUM)) {
    S.rewind(Num->size());
    return nullptr;
  }

  // We know the wstring is mostly just digits with possibly a prefix minus
  // so convert to string.
  std::string_view V = Num.value().Value;
  std::string NumStr(V.cbegin(), V.cend());

  return std::make_unique<ast::Integer>(NumStr);
}

// An expression is either:
// - an integer
// - an identifier
// - a values
// - an arithmetic plus
// - a lambda
// - a begin
std::unique_ptr<ast::ExprNode> Parse::parseExpr(SourceStream &S) {
  std::unique_ptr<ast::Integer> I = parseInteger(S);
  if (I) {
    return I;
  }

  std::unique_ptr<ast::BooleanLiteral> Bool = parseBooleanLiteral(S);
  if (Bool) {
    return Bool;
  }

  std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
  if (Id) {
    return Id;
  }

  std::unique_ptr<ast::Values> V = parseValues(S);
  if (V) {
    return V;
  }

  std::unique_ptr<ast::Lambda> L = parseLambda(S);
  if (L) {
    return L;
  }

  std::unique_ptr<ast::Begin> B = parseBegin(S);
  if (B) {
    return B;
  }

  std::unique_ptr<ast::SetBang> SB = parseSetBang(S);
  if (SB) {
    return SB;
  }

  std::unique_ptr<ast::IfCond> IC = parseIfCond(S);
  if (IC) {
    return IC;
  }

  std::unique_ptr<ast::LetValues> LV = parseLetValues(S);
  if (LV) {
    return LV;
  }

  std::unique_ptr<ast::Application> A = parseApplication(S);
  if (A) {
    return A;
  }

  return nullptr;
}

std::unique_ptr<ast::Identifier> Parse::parseIdentifier(SourceStream &S) {
  Tok IDTok = gettok(S);
  if (!IDTok.is(Tok::TokType::ID)) {
    S.rewind(IDTok.size());
    return nullptr;
  }
  return std::make_unique<ast::Identifier>(
      IdPool::instance().create(IDTok.Value));
}

// Parses an expression of the form:
// (define-values (id ...) expr)
std::unique_ptr<ast::DefineValues> Parse::parseDefineValues(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  Tok DVTok = gettok(S);
  if (!DVTok.is(Tok::TokType::DEFINE_VALUES)) {
    S.rewindTo(Start);
    return nullptr;
  }

  // parse the list of ids
  std::vector<ast::Identifier> Ids;
  T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    return nullptr;
  }

  while (true) {
    std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
    if (!Id) {
      break;
    }
    Ids.emplace_back(*Id);
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    return nullptr;
  }

  // parse the expression
  std::unique_ptr<ast::ExprNode> Expr = parseExpr(S);
  if (!Expr) {
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    return nullptr;
  }

  return std::make_unique<ast::DefineValues>(Ids, Expr);
}

// Parses an expression of the form:
// (values expr ...)
std::unique_ptr<ast::Values> Parse::parseValues(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  std::optional<Tok> IDTok = maybeLexIdOrNumber(S);
  if (!IDTok || !IDTok->is(Tok::TokType::VALUES)) {
    S.rewindTo(Start);
    return nullptr;
  }

  std::vector<std::unique_ptr<ast::ExprNode>> Exprs;
  while (true) {
    std::unique_ptr<ast::ExprNode> Expr = parseExpr(S);
    if (!Expr) {
      break;
    }
    Exprs.push_back(std::move(Expr));
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    return nullptr;
  }

  return std::make_unique<ast::Values>(std::move(Exprs));
}

std::unique_ptr<ast::Linklet> Parse::parseLinklet(SourceStream &S) {
  //   (linklet [[imported-id/renamed ...] ...]
  //          [exported-id/renamed ...]
  //   defn-or-expr ...)
  //
  // imported-id/renamed	 	=	 	imported-id
  //  	 	|	 	(external-imported-id
  //  internal-imported-id)
  //
  // exported-id/renamed	 	=	 	exported-id
  //  	 	|	 	(internal-exported-id
  //  external-exported-id)

  size_t Start = S.getPosition();
  auto Linklet = std::make_unique<ast::Linklet>();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::LINKLET)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  /// FIXME: Add imports support
  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  // Parsing linklet exports
  const std::vector<ast::Linklet::idpair_t> Exports = parseLinkletExports(S);
  for (const auto &E : Exports) {
    Linklet->appendExport(E.first, E.second);
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  while (true) {
    std::unique_ptr<ast::TLNode> Exp = parseDefnOrExpr(S);
    if (!Exp) {
      break;
    }
    Linklet->appendBodyForm(std::move(Exp));
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  if (Linklet->bodyFormsCount() == 0) {
    S.rewindTo(Start);
    return nullptr;
  }

  return Linklet;
}

// Parse lambda expression of the form:
// (lambda <formals> body)
// where formals are parsed by parseFormals
std::unique_ptr<ast::Lambda> Parse::parseLambda(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::LAMBDA)) {
    S.rewindTo(Start);
    return nullptr;
  }

  // Lets create the lambda node
  std::unique_ptr<ast::Lambda> Lambda = std::make_unique<ast::Lambda>();

  std::unique_ptr<ast::Formal> Formals = parseFormals(S);
  if (!Formals) {
    S.rewindTo(Start);
    return nullptr;
  }
  Lambda->setFormals(std::move(Formals));

  std::unique_ptr<ast::ExprNode> Body = parseExpr(S);
  if (!Body) {
    S.rewindTo(Start);
    return nullptr;
  }
  Lambda->setBody(std::move(Body));

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  return Lambda;
}

// Parse formals of the form:
// (id ...)
// (id ... . id)
// id
std::unique_ptr<ast::Formal> Parse::parseFormals(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);

    // If it's not a list, it's a single id
    std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
    if (Id) {
      return std::make_unique<ast::IdentifierFormal>(*Id);
    }

    S.rewindTo(Start);
    return nullptr;
  }

  std::vector<ast::Identifier> Ids;
  while (true) {
    T = gettok(S);
    if (T.is(Tok::TokType::RPAREN) || T.is(Tok::TokType::DOT)) {
      break;
    }
    S.rewind(T.size()); // put the token back

    std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
    if (!Id) {
      S.rewindTo(Start);
      return nullptr;
    }

    Ids.push_back(*Id);
  }

  if (T.is(Tok::TokType::DOT)) {

    std::unique_ptr<ast::Identifier> RestId = parseIdentifier(S);
    if (!RestId) {
      S.rewindTo(Start);
      return nullptr;
    }

    T = gettok(S);
    if (!T.is(Tok::TokType::RPAREN)) {
      S.rewindTo(Start);
      return nullptr;
    }

    return std::make_unique<ast::ListRestFormal>(Ids, *RestId);
  }

  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  return std::make_unique<ast::ListFormal>(Ids);
}

// Parse lambda expression of the form:
// (begin <expr>+) | (begin0 <expr>+)
std::unique_ptr<ast::Begin> Parse::parseBegin(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::BEGIN) && !T.is(Tok::TokType::BEGIN0)) {
    S.rewindTo(Start);
    return nullptr;
  }

  // Lets create the begin node
  std::unique_ptr<ast::Begin> Begin = std::make_unique<ast::Begin>();
  if (T.is(Tok::TokType::BEGIN0)) {
    Begin->markAsBegin0();
  }

  while (true) {
    std::unique_ptr<ast::ExprNode> Exp = parseExpr(S);
    if (!Exp) {
      break;
    }
    Begin->appendExpr(std::move(Exp));
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  if (Begin->bodyCount() == 0) {
    S.rewindTo(Start);
    return nullptr;
  }

  return Begin;
}

// Parse application of the form:
// (<expr> <expr>*)
std::unique_ptr<ast::Application> Parse::parseApplication(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  auto App = std::make_unique<ast::Application>();

  while (true) {
    std::unique_ptr<ast::ExprNode> Exp = parseExpr(S);
    if (!Exp) {
      break;
    }
    App->appendExpr(std::move(Exp));
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  if (App->length() == 0) {
    S.rewindTo(Start);
    return nullptr;
  }

  return App;
}

// Parse set!:
// (set! <id> <expr>)
std::unique_ptr<ast::SetBang> Parse::parseSetBang(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::SETBANG)) {
    S.rewindTo(Start);
    return nullptr;
  }

  auto Set = std::make_unique<ast::SetBang>();

  std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
  if (!Id) {
    S.rewindTo(Start);
    return nullptr;
  }
  Set->setIdentifier(std::move(Id));

  std::unique_ptr<ast::ExprNode> Exp = parseExpr(S);
  if (!Exp) {
    S.rewindTo(Start);
    return nullptr;
  }
  Set->setExpr(std::move(Exp));

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  return Set;
}

std::unique_ptr<ast::IfCond> Parse::parseIfCond(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::IF)) {
    S.rewindTo(Start);
    return nullptr;
  }

  auto If = std::make_unique<ast::IfCond>();

  std::unique_ptr<ast::ExprNode> Cond = parseExpr(S);
  if (!Cond) {
    S.rewindTo(Start);
    return nullptr;
  }
  If->setCond(std::move(Cond));

  std::unique_ptr<ast::ExprNode> Then = parseExpr(S);
  if (!Then) {
    S.rewindTo(Start);
    return nullptr;
  }
  If->setThen(std::move(Then));

  std::unique_ptr<ast::ExprNode> Else = parseExpr(S);
  if (!Else) {
    S.rewindTo(Start);
    return nullptr;
  }
  If->setElse(std::move(Else));

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  return If;
}

// Parse boolean literal
std::unique_ptr<ast::BooleanLiteral>
Parse::parseBooleanLiteral(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (T.is(Tok::TokType::BOOL_TRUE)) {
    return std::make_unique<ast::BooleanLiteral>(true);
  }
  if (T.is(Tok::TokType::BOOL_FALSE)) {
    return std::make_unique<ast::BooleanLiteral>(false);
  }
  S.rewindTo(Start);

  return nullptr;
}

// Parse a let-values form.
// (let-values ([(id ...) val-expr] ...) body ...+)
std::unique_ptr<ast::LetValues> Parse::parseLetValues(SourceStream &S) {
  size_t Start = S.getPosition();

  Tok T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::LET_VALUES)) {
    S.rewindTo(Start);
    return nullptr;
  }

  auto Let = std::make_unique<ast::LetValues>();

  T = gettok(S);
  if (!T.is(Tok::TokType::LPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  while (true) {
    T = gettok(S);
    if (T.is(Tok::TokType::RPAREN)) {
      break;
    }

    // Parse the binding.
    if (!T.is(Tok::TokType::LPAREN)) {
      S.rewindTo(Start);
      return nullptr;
    }

    // Parse list of identifiers.
    T = gettok(S);
    if (!T.is(Tok::TokType::LPAREN)) {
      S.rewindTo(Start);
      return nullptr;
    }

    std::vector<ast::Identifier> Ids;
    while (true) {
      T = gettok(S);
      if (T.is(Tok::TokType::RPAREN)) {
        break;
      }

      S.rewind(T.size());
      std::unique_ptr<ast::Identifier> Id = parseIdentifier(S);
      if (!Id) {
        S.rewindTo(Start);
        return nullptr;
      }
      Ids.push_back(*Id);
    }

    // Parse the value expression.
    std::unique_ptr<ast::ExprNode> Val = parseExpr(S);
    if (!Val) {
      S.rewindTo(Start);
      return nullptr;
    }

    T = gettok(S);
    if (!T.is(Tok::TokType::RPAREN)) {
      S.rewindTo(Start);
      return nullptr;
    }

    Let->appendBinding(std::move(Ids), std::move(Val));
  }

  while (true) {
    std::unique_ptr<ast::ExprNode> Exp = parseExpr(S);
    if (!Exp) {
      break;
    }
    Let->appendBody(std::move(Exp));
  }

  T = gettok(S);
  if (!T.is(Tok::TokType::RPAREN)) {
    S.rewindTo(Start);
    return nullptr;
  }

  if (Let->bodyCount() == 0) {
    S.rewindTo(Start);
    return nullptr;
  }

  return Let;
}