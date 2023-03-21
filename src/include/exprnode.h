#pragma once

#include <memory>
#include <variant>

#include "toplevelnode.h"

namespace nir {

class Identifier;
class Integer;
class Values;
class ArithPlus;
class Void;
class Lambda;
class Begin;

using ExprNode =
    std::variant<Integer, Identifier, Values, ArithPlus, Void, Lambda, Begin>;

struct ToTopLevelNode {
  std::unique_ptr<TLNode> operator()(nir::Identifier &&Id);
  std::unique_ptr<TLNode> operator()(nir::Integer &&Int);
  std::unique_ptr<TLNode> operator()(nir::Values &&V);
  std::unique_ptr<TLNode> operator()(nir::ArithPlus &&AP);
  std::unique_ptr<TLNode> operator()(nir::Void &&Vd);
  std::unique_ptr<TLNode> operator()(nir::Lambda &&Vd);
  std::unique_ptr<TLNode> operator()(nir::Begin &&Vd);
};

}; // namespace nir