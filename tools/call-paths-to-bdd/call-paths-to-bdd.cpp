#include "call-paths-to-bdd.h"

std::string expr_to_string(klee::ref<klee::Expr> expr, bool one_liner) {
  std::string expr_str;
  if (expr.isNull())
    return expr_str;
  llvm::raw_string_ostream os(expr_str);
  expr->print(os);
  os.str();

  if (one_liner) {
    // remove new lines
    expr_str.erase(std::remove(expr_str.begin(), expr_str.end(), '\n'), expr_str.end());

    // remove duplicated whitespaces
    auto bothAreSpaces = [](char lhs, char rhs) -> bool { return (lhs == rhs) && (lhs == ' '); };
    std::string::iterator new_end = std::unique(expr_str.begin(), expr_str.end(), bothAreSpaces);
    expr_str.erase(new_end, expr_str.end());
  }

  return expr_str;
}

namespace BDD {
bool solver_toolbox_t::is_expr_always_true(klee::ref<klee::Expr> expr) const {
  klee::ConstraintManager no_constraints;
  return is_expr_always_true(no_constraints, expr);
}

bool solver_toolbox_t::is_expr_always_true(klee::ConstraintManager constraints, klee::ref<klee::Expr> expr) const {
  klee::Query sat_query(constraints, expr);

  bool result;
  bool success = solver->mustBeTrue(sat_query, result);
  assert(success);

  return result;
}

bool solver_toolbox_t::is_expr_always_true(klee::ConstraintManager constraints,
                                           klee::ref<klee::Expr> expr,
                                           ReplaceSymbols& symbol_replacer) const {
    klee::ConstraintManager replaced_constraints;

    for (auto constr : constraints) {
      replaced_constraints.addConstraint(symbol_replacer.visit(constr));
    }

    return is_expr_always_true(replaced_constraints, expr);
  }

bool solver_toolbox_t::is_expr_always_false(klee::ref<klee::Expr> expr) const {
  klee::ConstraintManager no_constraints;
  return is_expr_always_false(no_constraints, expr);
}

bool solver_toolbox_t::is_expr_always_false(klee::ConstraintManager constraints,
                                            klee::ref<klee::Expr> expr) const {
  klee::Query sat_query(constraints, expr);

  bool result;
  bool success = solver->mustBeFalse(sat_query, result);
  assert(success);

  return result;
}

bool solver_toolbox_t::is_expr_always_false(klee::ConstraintManager constraints,
                                            klee::ref<klee::Expr> expr,
                                            ReplaceSymbols& symbol_replacer) const {
    klee::ConstraintManager replaced_constraints;

    for (auto constr : constraints) {
      replaced_constraints.addConstraint(symbol_replacer.visit(constr));
    }

    return is_expr_always_false(replaced_constraints, expr);
  }

bool solver_toolbox_t::are_exprs_always_equal(klee::ref<klee::Expr> expr1, klee::ref<klee::Expr> expr2) const {
  if (expr1.isNull() != expr2.isNull()) {
    return false;
  }

  if (expr1.isNull()) {
    return true;
  }

  RetrieveSymbols symbol_retriever;
  symbol_retriever.visit(expr1);
  std::vector<klee::ref<klee::ReadExpr>> symbols = symbol_retriever.get_retrieved();

  ReplaceSymbols symbol_replacer(symbols);
  klee::ref<klee::Expr> replaced = symbol_replacer.visit(expr2);

  return is_expr_always_true(exprBuilder->Eq(expr1, replaced));
}

uint64_t solver_toolbox_t::value_from_expr(klee::ref<klee::Expr> expr) const {
  klee::ConstraintManager no_constraints;
  klee::Query sat_query(no_constraints, expr);

  klee::ref<klee::ConstantExpr> value_expr;
  bool success = solver->getValue(sat_query, value_expr);

  assert(success);
  return value_expr->getZExtValue();
}

void CallPathsGroup::group_call_paths() {
  assert(call_paths.size());

  for (const auto& cp : call_paths) {
    on_true.clear();
    on_false.clear();

    if (cp->calls.size() == 0) {
      continue;
    }

    call_t call = cp->calls[0];

    for (auto _cp : call_paths) {
      if (_cp->calls.size() && are_calls_equal(_cp->calls[0], call)) {
        on_true.push_back(_cp);
        continue;
      }

      on_false.push_back(_cp);
    }

    // all calls are equal, no need do discriminate
    if (on_false.size() == 0) {
      return;
    }

    discriminating_constraint = find_discriminating_constraint();

    if (!discriminating_constraint.isNull()) {
      return;
    }
  }

  // no more calls
  if (on_true.size() == 0 && on_false.size() == 0) {
    on_true = call_paths;
    return;
  }

  assert(!discriminating_constraint.isNull());
}

bool CallPathsGroup::are_calls_equal(call_t c1, call_t c2) {
  if (c1.function_name != c2.function_name) {
    return false;
  }

  if (is_skip_function(c1.function_name)) {
    return true;
  }

  for (auto arg_name_value_pair : c1.args) {
    auto arg_name = arg_name_value_pair.first;

    // exception: we don't care about 'p' differences
    if (arg_name == "p") {
      continue;
    }

    auto c1_arg = c1.args[arg_name];
    auto c2_arg = c2.args[arg_name];

    if (!c1_arg.out.isNull()) {
      continue;
    }

    // comparison between modifications to the received packet
    if (c1.function_name == "packet_return_chunk" && arg_name == "the_chunk" &&
        !solver_toolbox.are_exprs_always_equal(c1_arg.in, c2_arg.in))
        return false;

    if (!solver_toolbox.are_exprs_always_equal(c1_arg.expr, c2_arg.expr)) {
      if (c1.function_name == "packet_receive") {
        std::cerr << arg_name << " diff" << "\n";
        char c; std::cin >> c;
      }
      return false;
    }
  }

  return true;
}

klee::ref<klee::Expr> CallPathsGroup::find_discriminating_constraint() {
  assert(on_true.size());

  auto possible_discriminating_constraints = get_possible_discriminating_constraints();

  for (auto constraint : possible_discriminating_constraints) {
    if (check_discriminating_constraint(constraint)) {
      return constraint;
    }
  }

  return klee::ref<klee::Expr>();
}

std::vector<klee::ref<klee::Expr>> CallPathsGroup::get_possible_discriminating_constraints() const {
  std::vector<klee::ref<klee::Expr>> possible_discriminating_constraints;
  assert(on_true.size());

  for (auto constraint : on_true[0]->constraints) {
    if (satisfies_constraint(on_true, constraint)) {
      possible_discriminating_constraints.push_back(constraint);
    }
  }

  return possible_discriminating_constraints;
}

bool CallPathsGroup::satisfies_constraint(std::vector<call_path_t*> call_paths,
                                          klee::ref<klee::Expr> constraint) const {
  for (const auto& call_path : call_paths) {
    if (!satisfies_constraint(call_path, constraint)) {
      return false;
    }
  }
  return true;
}

bool CallPathsGroup::satisfies_constraint(call_path_t* call_path, klee::ref<klee::Expr> constraint) const {
  RetrieveSymbols symbol_retriever;
  symbol_retriever.visit(constraint);
  std::vector<klee::ref<klee::ReadExpr>> symbols = symbol_retriever.get_retrieved();

  ReplaceSymbols symbol_replacer(symbols);
  auto not_constraint = solver_toolbox.exprBuilder->Not(constraint);

  return solver_toolbox.is_expr_always_false(call_path->constraints, not_constraint, symbol_replacer);
}

bool CallPathsGroup::satisfies_not_constraint(std::vector<call_path_t*> call_paths,
                                              klee::ref<klee::Expr> constraint) const {
  for (const auto& call_path : call_paths) {
    if (!satisfies_not_constraint(call_path, constraint)) {
      return false;
    }
  }
  return true;
}

bool CallPathsGroup::satisfies_not_constraint(call_path_t* call_path, klee::ref<klee::Expr> constraint) const {
  RetrieveSymbols symbol_retriever;
  symbol_retriever.visit(constraint);
  std::vector<klee::ref<klee::ReadExpr>> symbols = symbol_retriever.get_retrieved();

  ReplaceSymbols symbol_replacer(symbols);
  auto not_constraint = solver_toolbox.exprBuilder->Not(constraint);

  return solver_toolbox.is_expr_always_true(call_path->constraints, not_constraint, symbol_replacer);
}

bool CallPathsGroup::check_discriminating_constraint(klee::ref<klee::Expr> constraint) {
  assert(on_true.size());
  assert(on_false.size());

  std::vector<call_path*> _on_true = on_true;
  std::vector<call_path*> _on_false;

  for (auto call_path : on_false) {
    if (satisfies_constraint(call_path, constraint)) {
      _on_true.push_back(call_path);
    } else {
      _on_false.push_back(call_path);
    }
  }

  if (_on_false.size() && satisfies_not_constraint(_on_false, constraint)) {
    on_true  = _on_true;
    on_false = _on_false;
    return true;
  }

  return false;
}

call_t BDD::get_successful_call(std::vector<call_path_t*> call_paths) const {
  assert(call_paths.size());

  for (const auto& cp : call_paths) {
    assert(cp->calls.size());
    call_t call = cp->calls[0];

    if (call.ret.isNull()) {
      return call;
    }

    auto zero = solver_toolbox.exprBuilder->Constant(0, call.ret->getWidth());
    auto eq_zero = solver_toolbox.exprBuilder->Eq(call.ret, zero);
    auto is_ret_success = solver_toolbox.is_expr_always_false(eq_zero);

    if (is_ret_success) {
      return call;
    }
  }

  // no function with successful return
  return call_paths[0]->calls[0];
}

Node* BDD::populate(std::vector<call_path_t*> call_paths) {
  Node* local_root = nullptr;
  Node* local_leaf = nullptr;

  while (call_paths.size()) {
    CallPathsGroup group(call_paths, solver_toolbox);

    auto on_true  = group.get_on_true();
    auto on_false = group.get_on_false();

    if (on_true.size() == call_paths.size()) {
      assert(on_false.size() == 0);

      if (on_true[0]->calls.size() == 0) {
        return local_root;
      }

      Call* node = new Call(get_and_inc_id(), get_successful_call(on_true), on_true);

      // root node
      if (local_root == nullptr) {
        local_root = node;
        local_leaf = node;
      } else {
        local_leaf->add_next(node);
        node->add_prev(local_leaf);
        local_leaf = node;
      }

      for (auto& cp : call_paths) {
        assert(cp->calls.size());
        cp->calls.erase(cp->calls.begin());
      }
    } else {
      auto discriminating_constraint = group.get_discriminating_constraint();

      Branch* node = new Branch(get_and_inc_id(), discriminating_constraint, call_paths);

      Node* on_true_root  = populate(on_true);
      Node* on_false_root = populate(on_false);

      node->add_on_true(on_true_root);
      node->add_on_false(on_false_root);

      if (local_root == nullptr) {
        return node;
      }

      local_leaf->add_next(node);
      node->add_prev(local_leaf);

      return local_root;
    }
  }

  return local_root;
}

void BDD::dump() const {
  Node* node = root.get();
  dump(0, node);
}

void BDD::dump(int lvl, const Node* node) const {
  std::string sep = std::string(lvl*2, ' ');

  if (node) {
    std::cerr << "\n";
    for (auto filename : node->get_call_paths_filenames()) {
      std::cerr << sep << "[" << filename << "]" << "\n";
    }
  }

  while (node) {
    node->dump_compact(lvl);

    if (node->get_type() == Node::NodeType::BRANCH) {
      const Branch* branch_node = static_cast<const Branch*>(node);
      dump(lvl+1, branch_node->get_on_true());
      dump(lvl+1, branch_node->get_on_false());
      return;
    }

    node = node->get_next();
  }
}
}