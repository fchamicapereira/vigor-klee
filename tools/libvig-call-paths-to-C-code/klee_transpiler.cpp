#include "ast.h"

Type_ptr klee_width_to_type(klee::Expr::Width width) {
  Type_ptr type;

  switch (width) {
  case klee::Expr::InvalidWidth:
    assert(false);
  case klee::Expr::Bool:
    type = PrimitiveType::build(PrimitiveType::Kind::BOOL);
    break;
  case klee::Expr::Int8:
    type = PrimitiveType::build(PrimitiveType::Kind::UINT8_T);
    break;
  case klee::Expr::Int16:
    type = PrimitiveType::build(PrimitiveType::Kind::UINT16_T);
    break;
  case klee::Expr::Int32:
    type = PrimitiveType::build(PrimitiveType::Kind::UINT32_T);
    break;
  case klee::Expr::Int64:
    type = PrimitiveType::build(PrimitiveType::Kind::UINT64_T);
    break;
  case klee::Expr::Fl80:
  default:
    if (width % 8 != 0) {
      assert(false && "Width not a byte multiple");
    }

    Type_ptr byte = PrimitiveType::build(PrimitiveType::Kind::UINT8_T);
    type = Array::build(byte, width / 8);
  }

  return type;
}

Expr_ptr const_to_ast_expr(const klee::ref<klee::Expr> &e) {
  if (e->getKind() != klee::Expr::Kind::Constant) {
    return nullptr;
  }

  klee::ConstantExpr* constant = static_cast<klee::ConstantExpr *>(e.get());

  uint64_t value = constant->getZExtValue();
  Type_ptr type = klee_width_to_type(constant->getWidth());

  assert(type->get_kind() == Node::Kind::PRIMITIVE);
  PrimitiveType* p = static_cast<PrimitiveType*>(type.get());

  return Constant::build(p->get_primitive_kind(), value);
}

uint64_t const_to_value(const klee::ref<klee::Expr> &e) {
  assert(e->getKind() == klee::Expr::Kind::Constant);

  klee::ConstantExpr* constant = static_cast<klee::ConstantExpr *>(e.get());
  uint64_t value = constant->getZExtValue();

  return value;
}

Expr_ptr transpile(AST* ast, const klee::ref<klee::Expr> &e) {
  Expr_ptr result = const_to_ast_expr(e);

  if (result != nullptr) {
    return result;
  }

  KleeExprToASTNodeConverter converter(ast);
  converter.visit(e);
  assert(converter.get_result());

  return converter.get_result();
}

uint64_t get_first_concat_idx(const klee::ref<klee::Expr> &e) {
  assert(e->getKind() == klee::Expr::Kind::Concat);

  klee::ref<klee::Expr> curr_node = e;
  while (curr_node->getKind() == klee::Expr::Kind::Concat) {
    curr_node = curr_node->getKid(1);
  }

  assert(curr_node->getKind() == klee::Expr::Kind::Read);
  klee::ReadExpr* read = static_cast<klee::ReadExpr*>(curr_node.get());

  return const_to_value(read->index);
}

uint64_t get_last_concat_idx(const klee::ref<klee::Expr> &e) {
  assert(e->getKind() == klee::Expr::Kind::Concat);

  klee::ref<klee::Expr> left = e->getKid(0);

  assert(left->getKind() == klee::Expr::Kind::Read);
  klee::ReadExpr* read = static_cast<klee::ReadExpr*>(left.get());

  return const_to_value(read->index);
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitRead(const klee::ReadExpr &e) {
  klee::ref<klee::Expr> eref = const_cast<klee::ReadExpr *>(&e);

  Expr_ptr local = ast->get_from_local(eref);
  if (local != nullptr) {
    save_result(local);
    return klee::ExprVisitor::Action::skipChildren();
  }

  Type_ptr type = klee_width_to_type(e.getWidth());
  Expr_ptr idx = transpile(ast, e.index);

  klee::UpdateList ul = e.updates;
  const klee::Array *root = ul.root;
  std::string symbol = root->name;

  if (symbol == "VIGOR_DEVICE") {
    symbol = "src_devices";
  }

  else if (symbol == "next_time") {
    symbol = "now";
  }

  else if (symbol == "data_len") {
    symbol = "pkt_len";
  }

  else if (symbol == "packet_chunks") {
    assert(idx->get_kind() == Node::Kind::CONSTANT);
    Constant* idx_const = static_cast<Constant*>(idx.get());

    AST::chunk_t chunk_info = ast->get_chunk_from_local(idx_const->get_value());
    Variable_ptr var = chunk_info.var;
    assert(var != nullptr);

    unsigned new_idx_value = idx_const->get_value() - chunk_info.start_index;

    PrimitiveType* p = static_cast<PrimitiveType*>(idx_const->get_type().get());
    Constant_ptr new_idx = Constant::build(p->get_primitive_kind(), new_idx_value);

    Read_ptr read = Read::build(var, type, new_idx);
    assert(read);

    save_result(read);

    return klee::ExprVisitor::Action::skipChildren();
  }

  symbol_width = std::make_pair(true, root->getSize() * 8);

  Variable_ptr var = ast->get_from_local(symbol);
  assert(var != nullptr);

  Read_ptr read = Read::build(var, type, idx);
  assert(read);

  save_result(read);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSelect(const klee::SelectExpr& e) {
  assert(e.getNumKids() == 3);

  Expr_ptr cond = transpile(ast, e.getKid(0));
  assert(cond);

  Expr_ptr first = transpile(ast, e.getKid(1));
  assert(first);

  Expr_ptr second = transpile(ast, e.getKid(2));
  assert(second);

  Select_ptr select = Select::build(cond, first, second);

  save_result(select);

  return klee::ExprVisitor::Action::skipChildren();
}

Expr_ptr simplify_concat(Expr_ptr var, Concat_ptr concat) {
  auto var_size = var->get_type()->get_size();
  auto concat_size = concat->get_type()->get_size();

  bool sequential_of_reads = concat->is_concat_of_reads_and_concats() &&
                             concat->is_sequential();

  if (!sequential_of_reads) {
    return concat;
  }

  if (var->get_kind() == Node::Kind::STRUCT) {
    std::cerr << "\n======================================================================\n";
    std::cerr << "concat:" << "\n";
    concat->debug(std::cerr);
    std::cerr << "\n";
    std::cerr << "concat of reads and concats: " << concat->is_concat_of_reads_and_concats() << "\n";
    std::cerr << "sequential: " << concat->is_sequential() << "\n";
    std::cerr << "\n======================================================================\n";

    assert(false && "Not implemented");
  }

  if (var_size == concat_size) {
    return var;
  }

  return concat;
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitConcat(const klee::ConcatExpr& e) {
  Expr_ptr left = transpile(ast, e.getKid(0));
  Expr_ptr right = transpile(ast, e.getKid(1));
  Type_ptr type = klee_width_to_type(e.getWidth());

  Concat_ptr concat = Concat::build(left, right, type);

  RetrieveSymbols retriever;
  retriever.visit(klee::ref<klee::Expr>(const_cast<klee::ConcatExpr *>(&e)));
  auto symbols = retriever.get_retrieved_strings();

  if (symbols.size() != 1) {
    save_result(concat);
    return klee::ExprVisitor::Action::skipChildren();
  }

  std::string symbol = symbols[0];
  Expr_ptr var;

  var = ast->get_from_state(symbol);

  if (var == nullptr) {
    var = ast->get_from_local(symbol);
  }

  if (var == nullptr) {
    save_result(concat);
    return klee::ExprVisitor::Action::skipChildren();
  }

  save_result(simplify_concat(var, concat));

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitExtract(const klee::ExtractExpr& e) {
  auto expr = e.expr;
  auto offset_value = e.offset;
  auto size = e.width;

  Type_ptr type = klee_width_to_type(e.getWidth());

  Expr_ptr ast_expr = transpile(ast, expr);
  assert(ast_expr);

  Expr_ptr mask = Constant::build(PrimitiveType::Kind::UINT64_T, (1 << size) - 1, true);
  Expr_ptr extract;

  if (offset_value > 0) {
    Expr_ptr offset = Constant::build(PrimitiveType::Kind::UINT64_T, offset_value);
    ShiftRight_ptr shift = ShiftRight::build(ast_expr, offset);
    extract = And::build(shift, mask);
  }

  else {
    extract = ast_expr;
  }

  Cast_ptr cast = Cast::build(extract, type);

  save_result(cast);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitZExt(const klee::ZExtExpr& e) {
  assert(e.getNumKids() == 1);

  Type_ptr type = klee_width_to_type(e.getWidth());
  auto expr = e.getKid(0);

  Expr_ptr ast_expr = transpile(ast, expr);
  assert(ast_expr);

  Cast_ptr cast = Cast::build(ast_expr, type);

  save_result(cast);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSExt(const klee::SExtExpr& e) {
  assert(e.getNumKids() == 1);

  auto size = e.getWidth();
  auto expr_size = e.getKid(0)->getWidth();

  Type_ptr type = klee_width_to_type(e.getWidth());
  Expr_ptr expr = transpile(ast, e.getKid(0));
  assert(expr);

  unsigned int mask = 0;
  for (unsigned int i = 0; i < size; i++) {
    if (i < (size - expr_size)) {
      mask = (mask << 1) | 1;
    } else {
      mask = (mask << 1);
    }
  }

  Expr_ptr result;

  if (size > expr_size) {
    assert(type->get_kind() == Node::Kind::PRIMITIVE);

    PrimitiveType* primitive = static_cast<PrimitiveType*>(type.get());
    Expr_ptr mask_expr = Constant::build(primitive->get_primitive_kind(), mask, true);

    Expr_ptr shift_value = Constant::build(primitive->get_primitive_kind(), size - 1);
    ShiftRight_ptr msb = ShiftRight::build(expr, shift_value);

    Expr_ptr if_msb_one = Or::build(mask_expr, expr);
    Expr_ptr if_msb_not_one = Cast::build(expr, type);

    result = Select::build(msb, if_msb_one, if_msb_not_one);
  } else if (size == expr_size) {
    result = expr;
  } else {
    result = Cast::build(expr, type);

  }

  save_result(result);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitAdd(const klee::AddExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Add_ptr a = Add::build(left, right);
  save_result(a);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSub(const klee::SubExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Sub_ptr s = Sub::build(left, right);
  save_result(s);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitMul(const klee::MulExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Mul_ptr m = Mul::build(left, right);
  save_result(m);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitUDiv(const klee::UDivExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Div_ptr d = Div::build(left, right);
  save_result(d);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSDiv(const klee::SDivExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr cast = Cast::build(left, true);
  Div_ptr d = Div::build(cast, right);
  save_result(d);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitURem(const klee::URemExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Mod_ptr m = Mod::build(left, right);
  save_result(m);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSRem(const klee::SRemExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr cast = Cast::build(left, true);
  Mod_ptr m = Mod::build(cast, right);

  save_result(m);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitNot(const klee::NotExpr& e) {
  assert(e.getNumKids() == 1);

  Expr_ptr arg = transpile(ast, e.getKid(0));

  save_result(Not::build(arg));

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitAnd(const klee::AndExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  And_ptr a = And::build(left, right);
  save_result(a);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitOr(const klee::OrExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Or_ptr o = Or::build(left, right);
  save_result(o);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitXor(const klee::XorExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Xor_ptr x = Xor::build(left, right);
  save_result(x);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitShl(const klee::ShlExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  ShiftLeft_ptr sl = ShiftLeft::build(left, right);
  save_result(sl);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitLShr(const klee::LShrExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  ShiftRight_ptr sr = ShiftRight::build(left, right);
  save_result(sr);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitAShr(const klee::AShrExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr cast = Cast::build(left, true);
  ShiftRight_ptr sr = ShiftRight::build(cast, right);

  save_result(sr);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitEq(const klee::EqExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  if (right->get_kind() == Node::Kind::EQUALS &&
      left->get_kind() == Node::Kind::CONSTANT) {

    Constant* left_const = static_cast<Constant*>(left.get());
    Equals* right_eq = static_cast<Equals*>(right.get());
    Expr_ptr right_eq_left = right_eq->get_lhs();

    if (right_eq_left->get_kind() == Node::Kind::CONSTANT) {
      Constant* right_eq_left_const = static_cast<Constant*>(right_eq_left.get());

      if (right_eq_left_const->get_value() == 0 && left_const->get_value() == 0) {
        save_result(right_eq->get_rhs());
        return klee::ExprVisitor::Action::skipChildren();
      }
    }
  }

  Equals_ptr equals = Equals::build(left, right);
  save_result(equals);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitNe(const klee::NeExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  NotEquals_ptr ne = NotEquals::build(left, right);

  save_result(ne);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitUlt(const klee::UltExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Less_ptr lt = Less::build(left, right);

  save_result(lt);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitUle(const klee::UleExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  LessEq_ptr le = LessEq::build(left, right);

  save_result(le);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitUgt(const klee::UgtExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Greater_ptr gt = Greater::build(left, right);

  save_result(gt);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitUge(const klee::UgeExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  GreaterEq_ptr ge = GreaterEq::build(left, right);

  save_result(ge);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSlt(const klee::SltExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr lc = Cast::build(left, true);
  Cast_ptr rc = Cast::build(right, true);

  Less_ptr lt = Less::build(lc, rc);

  save_result(lt);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSle(const klee::SleExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr lc = Cast::build(left, true);
  Cast_ptr rc = Cast::build(right, true);

  LessEq_ptr le = LessEq::build(lc, rc);

  save_result(le);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSgt(const klee::SgtExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr lc = Cast::build(left, true);
  Cast_ptr rc = Cast::build(right, true);

  Greater_ptr gt = Greater::build(lc, rc);

  save_result(gt);

  return klee::ExprVisitor::Action::skipChildren();
}

klee::ExprVisitor::Action KleeExprToASTNodeConverter::visitSge(const klee::SgeExpr& e) {
  assert(e.getNumKids() == 2);

  Expr_ptr left = transpile(ast, e.getKid(0));
  assert(left);

  Expr_ptr right = transpile(ast, e.getKid(1));
  assert(right);

  Cast_ptr lc = Cast::build(left, true);
  Cast_ptr rc = Cast::build(right, true);

  GreaterEq_ptr ge = GreaterEq::build(lc, rc);

  save_result(ge);

  return klee::ExprVisitor::Action::skipChildren();
}
