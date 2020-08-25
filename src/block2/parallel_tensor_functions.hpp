
/*
 * block2: Efficient MPO implementation of quantum chemistry DMRG
 * Copyright (C) 2020 Huanchen Zhai <hczhai@caltech.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "parallel_rule.hpp"
#include "tensor_functions.hpp"
#include <cassert>
#include <map>
#include <memory>
#include <set>

using namespace std;

namespace block2 {

// Operations for operator tensors (parallel case)
template <typename S> struct ParallelTensorFunctions : TensorFunctions<S> {
    using TensorFunctions<S>::opf;
    shared_ptr<ParallelRule<S>> rule;
    ParallelTensorFunctions(const shared_ptr<OperatorFunctions<S>> &opf,
                            const shared_ptr<ParallelRule<S>> &rule)
        : TensorFunctions<S>(opf), rule(rule) {}
    // c = a
    void left_assign(const shared_ptr<OperatorTensor<S>> &a,
                     shared_ptr<OperatorTensor<S>> &c) const override {
        assert(a->lmat != nullptr);
        assert(a->lmat->get_type() == SymTypes::RVec);
        assert(c->lmat != nullptr);
        assert(c->lmat->get_type() == SymTypes::RVec);
        assert(a->lmat->data.size() == c->lmat->data.size());
        for (size_t i = 0; i < a->lmat->data.size(); i++) {
            if (a->lmat->data[i]->get_type() == OpTypes::Zero)
                c->lmat->data[i] = a->lmat->data[i];
            else {
                assert(a->lmat->data[i] == c->lmat->data[i]);
                auto pa = abs_value(a->lmat->data[i]),
                     pc = abs_value(c->lmat->data[i]);
                if (rule->available(pc)) {
                    assert(rule->available(pa));
                    assert(c->ops[pc]->data == nullptr);
                    c->ops[pc]->allocate(c->ops[pc]->info);
                    if (c->ops[pc]->info->n == a->ops[pa]->info->n)
                        c->ops[pc]->copy_data_from(a->ops[pa]);
                    else
                        c->ops[pc]->selective_copy_from(a->ops[pa]);
                    c->ops[pc]->factor = a->ops[pa]->factor;
                }
            }
        }
    }
    // c = a
    void right_assign(const shared_ptr<OperatorTensor<S>> &a,
                      shared_ptr<OperatorTensor<S>> &c) const override {
        assert(a->rmat != nullptr);
        assert(a->rmat->get_type() == SymTypes::CVec);
        assert(c->rmat != nullptr);
        assert(c->rmat->get_type() == SymTypes::CVec);
        assert(a->rmat->data.size() == c->rmat->data.size());
        for (size_t i = 0; i < a->rmat->data.size(); i++) {
            if (a->rmat->data[i]->get_type() == OpTypes::Zero)
                c->rmat->data[i] = a->rmat->data[i];
            else {
                assert(a->rmat->data[i] == c->rmat->data[i]);
                auto pa = abs_value(a->rmat->data[i]),
                     pc = abs_value(c->rmat->data[i]);
                if (rule->available(pc)) {
                    assert(rule->available(pa));
                    assert(c->ops[pc]->data == nullptr);
                    c->ops[pc]->allocate(c->ops[pc]->info);
                    if (c->ops[pc]->info->n == a->ops[pa]->info->n)
                        c->ops[pc]->copy_data_from(a->ops[pa]);
                    else
                        c->ops[pc]->selective_copy_from(a->ops[pa]);
                    c->ops[pc]->factor = a->ops[pa]->factor;
                }
            }
        }
    }
    // vmat = expr[L part | R part] x cmat (for perturbative noise)
    void tensor_product_partial_multiply(
        const shared_ptr<OpExpr<S>> &expr,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &lop,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &rop,
        bool trace_right, const shared_ptr<SparseMatrix<S>> &cmat,
        const vector<pair<uint8_t, S>> &psubsl,
        const vector<
            vector<shared_ptr<typename SparseMatrixInfo<S>::ConnectionInfo>>>
            &cinfos,
        const vector<S> &vdqs,
        const shared_ptr<SparseMatrixGroup<S>> &vmats) const override {
        const shared_ptr<OpElement<S>> i_op =
            make_shared<OpElement<S>>(OpNames::I, SiteIndex(), S());
        switch (expr->get_type()) {
        case OpTypes::Prod: {
            shared_ptr<OpString<S>> op =
                dynamic_pointer_cast<OpString<S>>(expr);
            assert(op->b != nullptr);
            shared_ptr<typename SparseMatrixInfo<S>::ConnectionInfo> old_cinfo =
                cmat->info->cinfo;
            if (trace_right) {
                assert(lop.count(op->a) != 0 && rop.count(i_op) != 0);
                shared_ptr<SparseMatrix<S>> lmat = lop.at(op->a);
                shared_ptr<SparseMatrix<S>> rmat = rop.at(i_op);
                S opdq = (op->conj & 1) ? -op->a->q_label : op->a->q_label;
                S pks = cmat->info->delta_quantum + opdq;
                int ij = lower_bound(psubsl.begin(), psubsl.end(),
                                     make_pair((uint8_t)(op->conj & 1), opdq)) -
                         psubsl.begin();
                for (int k = 0; k < pks.count(); k++) {
                    S vdq = pks[k];
                    int iv = lower_bound(vdqs.begin(), vdqs.end(), vdq) -
                             vdqs.begin();
                    shared_ptr<SparseMatrix<S>> vmat = (*vmats)[iv];
                    cmat->info->cinfo = cinfos[ij][k];
                    opf->tensor_product_multiply(op->conj & 1, lmat, rmat, cmat,
                                                 vmat, opdq, op->factor);
                }
            } else {
                assert(lop.count(i_op) != 0 && rop.count(op->b) != 0);
                shared_ptr<SparseMatrix<S>> lmat = lop.at(i_op);
                shared_ptr<SparseMatrix<S>> rmat = rop.at(op->b);
                S opdq = (op->conj & 2) ? -op->b->q_label : op->b->q_label;
                S pks = cmat->info->delta_quantum + opdq;
                int ij =
                    lower_bound(psubsl.begin(), psubsl.end(),
                                make_pair((uint8_t)(!!(op->conj & 2)), opdq)) -
                    psubsl.begin();
                for (int k = 0; k < pks.count(); k++) {
                    S vdq = pks[k];
                    int iv = lower_bound(vdqs.begin(), vdqs.end(), vdq) -
                             vdqs.begin();
                    shared_ptr<SparseMatrix<S>> vmat = (*vmats)[iv];
                    cmat->info->cinfo = cinfos[ij][k];
                    opf->tensor_product_multiply(op->conj & 2, lmat, rmat, cmat,
                                                 vmat, opdq, op->factor);
                }
            }
            cmat->info->cinfo = old_cinfo;
        } break;
        case OpTypes::Sum: {
            shared_ptr<OpSum<S>> op = dynamic_pointer_cast<OpSum<S>>(expr);
            for (auto &x : op->strings)
                tensor_product_partial_multiply(x, lop, rop, trace_right, cmat,
                                                psubsl, cinfos, vdqs, vmats);
        } break;
        case OpTypes::ExprRef: {
            shared_ptr<OpExprRef<S>> op =
                dynamic_pointer_cast<OpExprRef<S>>(expr);
            tensor_product_partial_multiply(op->op, lop, rop, trace_right, cmat,
                                            psubsl, cinfos, vdqs, vmats);
            if (opf->seq->mode != SeqTypes::Auto)
                rule->comm->reduce_sum(vmats, rule->comm->root);
        } break;
        case OpTypes::Zero:
            break;
        default:
            assert(false);
            break;
        }
    }
    // vmats = expr x cmats
    void tensor_product_multi_multiply(
        const shared_ptr<OpExpr<S>> &expr,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &lop,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &rop,
        const shared_ptr<SparseMatrixGroup<S>> &cmats,
        const shared_ptr<SparseMatrixGroup<S>> &vmats, S opdq,
        bool all_reduce) const override {
        switch (expr->get_type()) {
        case OpTypes::ExprRef: {
            shared_ptr<OpExprRef<S>> op =
                dynamic_pointer_cast<OpExprRef<S>>(expr);
            tensor_product_multi_multiply(op->op, lop, rop, cmats, vmats, opdq,
                                          false);
            if (all_reduce)
                rule->comm->allreduce_sum(vmats);
        } break;
        case OpTypes::Zero:
            break;
        default:
            for (int i = 0; i < cmats->n; i++)
                tensor_product_multiply(expr, lop, rop, (*cmats)[i],
                                        (*vmats)[i], opdq, false);
            break;
        }
    }
    // vmat = expr x cmat
    void tensor_product_multiply(
        const shared_ptr<OpExpr<S>> &expr,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &lop,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &rop,
        const shared_ptr<SparseMatrix<S>> &cmat,
        const shared_ptr<SparseMatrix<S>> &vmat, S opdq,
        bool all_reduce) const override {
        switch (expr->get_type()) {
        case OpTypes::Prod: {
            shared_ptr<OpString<S>> op =
                dynamic_pointer_cast<OpString<S>>(expr);
            assert(op->b != nullptr);
            assert(!(lop.count(op->a) == 0 || rop.count(op->b) == 0));
            shared_ptr<SparseMatrix<S>> lmat = lop.at(op->a);
            shared_ptr<SparseMatrix<S>> rmat = rop.at(op->b);
            opf->tensor_product_multiply(op->conj, lmat, rmat, cmat, vmat, opdq,
                                         op->factor);
        } break;
        case OpTypes::Sum: {
            shared_ptr<OpSum<S>> op = dynamic_pointer_cast<OpSum<S>>(expr);
            for (auto &x : op->strings)
                tensor_product_multiply(x, lop, rop, cmat, vmat, opdq, false);
        } break;
        case OpTypes::ExprRef: {
            shared_ptr<OpExprRef<S>> op =
                dynamic_pointer_cast<OpExprRef<S>>(expr);
            tensor_product_multiply(op->op, lop, rop, cmat, vmat, opdq, false);
            if (all_reduce)
                rule->comm->allreduce_sum(vmat);
        } break;
        case OpTypes::Zero:
            break;
        default:
            assert(false);
            break;
        }
    }
    // mat = diag(expr)
    void tensor_product_diagonal(
        const shared_ptr<OpExpr<S>> &expr,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &lop,
        const map<shared_ptr<OpExpr<S>>, shared_ptr<SparseMatrix<S>>,
                  op_expr_less<S>> &rop,
        shared_ptr<SparseMatrix<S>> &mat, S opdq) const override {
        switch (expr->get_type()) {
        case OpTypes::Prod: {
            shared_ptr<OpString<S>> op =
                dynamic_pointer_cast<OpString<S>>(expr);
            assert(op->b != nullptr);
            assert(!(lop.count(op->a) == 0 || rop.count(op->b) == 0));
            shared_ptr<SparseMatrix<S>> lmat = lop.at(op->a);
            shared_ptr<SparseMatrix<S>> rmat = rop.at(op->b);
            opf->tensor_product_diagonal(op->conj, lmat, rmat, mat, opdq,
                                         op->factor);
        } break;
        case OpTypes::Sum: {
            shared_ptr<OpSum<S>> op = dynamic_pointer_cast<OpSum<S>>(expr);
            for (auto &x : op->strings)
                tensor_product_diagonal(x, lop, rop, mat, opdq);
        } break;
        case OpTypes::ExprRef: {
            shared_ptr<OpExprRef<S>> op =
                dynamic_pointer_cast<OpExprRef<S>>(expr);
            tensor_product_diagonal(op->op, lop, rop, mat, opdq);
            if (opf->seq->mode != SeqTypes::Auto)
                rule->comm->allreduce_sum(mat);
        } break;
        case OpTypes::Zero:
            break;
        default:
            assert(false);
            break;
        }
    }
    // c = mpst_bra x a x mpst_ket
    void left_rotate(const shared_ptr<OperatorTensor<S>> &a,
                     const shared_ptr<SparseMatrix<S>> &mpst_bra,
                     const shared_ptr<SparseMatrix<S>> &mpst_ket,
                     shared_ptr<OperatorTensor<S>> &c) const override {
        for (size_t i = 0; i < a->lmat->data.size(); i++)
            if (a->lmat->data[i]->get_type() != OpTypes::Zero) {
                auto pa = abs_value(a->lmat->data[i]);
                if (rule->available(pa)) {
                    assert(c->ops.at(pa)->data == nullptr);
                    c->ops.at(pa)->allocate(c->ops.at(pa)->info);
                }
                if (rule->own(pa))
                    opf->tensor_rotate(a->ops.at(pa), c->ops.at(pa), mpst_bra,
                                       mpst_ket, false);
            }
        if (opf->seq->mode == SeqTypes::Auto)
            opf->seq->auto_perform();
        for (size_t i = 0; i < a->lmat->data.size(); i++)
            if (a->lmat->data[i]->get_type() != OpTypes::Zero) {
                auto pa = abs_value(a->lmat->data[i]);
                if (rule->repeat(pa))
                    rule->comm->broadcast(c->ops.at(pa), rule->owner(pa));
            }
    }
    // c = mpst_bra x a x mpst_ket
    void right_rotate(const shared_ptr<OperatorTensor<S>> &a,
                      const shared_ptr<SparseMatrix<S>> &mpst_bra,
                      const shared_ptr<SparseMatrix<S>> &mpst_ket,
                      shared_ptr<OperatorTensor<S>> &c) const override {
        for (size_t i = 0; i < a->rmat->data.size(); i++)
            if (a->rmat->data[i]->get_type() != OpTypes::Zero) {
                auto pa = abs_value(a->rmat->data[i]);
                if (rule->available(pa)) {
                    assert(c->ops.at(pa)->data == nullptr);
                    c->ops.at(pa)->allocate(c->ops.at(pa)->info);
                }
                if (rule->own(pa))
                    opf->tensor_rotate(a->ops.at(pa), c->ops.at(pa), mpst_bra,
                                       mpst_ket, true);
            }
        if (opf->seq->mode == SeqTypes::Auto)
            opf->seq->auto_perform();
        for (size_t i = 0; i < a->rmat->data.size(); i++)
            if (a->rmat->data[i]->get_type() != OpTypes::Zero) {
                auto pa = abs_value(a->rmat->data[i]);
                if (rule->repeat(pa))
                    rule->comm->broadcast(c->ops.at(pa), rule->owner(pa));
            }
    }
    // Numerical transform from normal operators
    // to complementary operators near the middle site
    void
    numerical_transform(const shared_ptr<OperatorTensor<S>> &a,
                        const shared_ptr<Symbolic<S>> &names,
                        const shared_ptr<Symbolic<S>> &exprs) const override {
        for (auto &op : a->ops)
            if (op.second->data == nullptr)
                op.second->allocate(op.second->info);
        assert(names->data.size() == exprs->data.size());
        assert((a->lmat == nullptr) ^ (a->rmat == nullptr));
        if (a->lmat == nullptr)
            a->rmat = names;
        else
            a->lmat = names;
        for (size_t i = 0; i < a->ops.size(); i++) {
            bool found = false;
            for (size_t k = 0; k < names->data.size(); k++) {
                if (exprs->data[k]->get_type() == OpTypes::Zero)
                    continue;
                shared_ptr<OpExpr<S>> nop = abs_value(names->data[k]);
                shared_ptr<OpExpr<S>> expr =
                    exprs->data[k] *
                    (1 / dynamic_pointer_cast<OpElement<S>>(names->data[k])
                             ->factor);
                if (expr->get_type() != OpTypes::ExprRef)
                    expr = rule->localize_expr(expr, rule->owner(nop))->op;
                else
                    expr = dynamic_pointer_cast<OpExprRef<S>>(expr)->op;
                switch (expr->get_type()) {
                case OpTypes::Sum: {
                    shared_ptr<OpSum<S>> op =
                        dynamic_pointer_cast<OpSum<S>>(expr);
                    found |= i < op->strings.size();
                    if (i < op->strings.size()) {
                        shared_ptr<OpElement<S>> nexpr =
                            op->strings[i]->get_op();
                        assert(a->ops.count(nexpr) != 0);
                        opf->iadd(a->ops.at(nop), a->ops.at(nexpr),
                                  op->strings[i]->factor,
                                  op->strings[i]->conj != 0);
                    }
                } break;
                case OpTypes::Zero:
                    break;
                default:
                    assert(false);
                    break;
                }
            }
            if (!found)
                break;
            else if (opf->seq->mode == SeqTypes::Simple)
                opf->seq->simple_perform();
        }
        if (opf->seq->mode == SeqTypes::Auto)
            opf->seq->auto_perform();
        for (size_t k = 0; k < names->data.size(); k++) {
            shared_ptr<OpExpr<S>> nop = abs_value(names->data[k]);
            if (exprs->data[k]->get_type() == OpTypes::Zero)
                continue;
            shared_ptr<OpExpr<S>> expr = exprs->data[k];
            bool is_local = false;
            if (expr->get_type() != OpTypes::ExprRef)
                is_local =
                    rule->localize_expr(expr, rule->owner(nop))->is_local;
            else
                is_local = dynamic_pointer_cast<OpExprRef<S>>(expr)->is_local;
            if (!is_local)
                rule->comm->reduce_sum(a->ops.at(nop), rule->owner(nop));
        }
    }
    // delayed left and right block contraction
    shared_ptr<DelayedOperatorTensor<S>>
    delayed_contract(const shared_ptr<OperatorTensor<S>> &a,
                     const shared_ptr<OperatorTensor<S>> &b,
                     const shared_ptr<OpExpr<S>> &op) const override {
        shared_ptr<DelayedOperatorTensor<S>> dopt =
            TensorFunctions<S>::delayed_contract(a, b, op);
        dopt->mat->data[0] =
            rule->localize_expr(dopt->mat->data[0], rule->owner(dopt->ops[0]));
        return dopt;
    }
    // delayed left and right block contraction
    // using the pre-computed exprs
    shared_ptr<DelayedOperatorTensor<S>>
    delayed_contract(const shared_ptr<OperatorTensor<S>> &a,
                     const shared_ptr<OperatorTensor<S>> &b,
                     const shared_ptr<Symbolic<S>> &ops,
                     const shared_ptr<Symbolic<S>> &exprs) const override {
        shared_ptr<DelayedOperatorTensor<S>> dopt =
            TensorFunctions<S>::delayed_contract(a, b, ops, exprs);
        for (size_t i = 0; i < dopt->mat->data.size(); i++)
            if (dopt->mat->data[i]->get_type() != OpTypes::ExprRef)
                dopt->mat->data[i] = rule->localize_expr(
                    dopt->mat->data[i], rule->owner(dopt->ops[i]));
        return dopt;
    }
    // c = a x b (dot)
    void left_contract(
        const shared_ptr<OperatorTensor<S>> &a,
        const shared_ptr<OperatorTensor<S>> &b,
        shared_ptr<OperatorTensor<S>> &c,
        const shared_ptr<Symbolic<S>> &cexprs = nullptr) const override {
        if (a == nullptr)
            left_assign(b, c);
        else {
            shared_ptr<Symbolic<S>> exprs =
                cexprs == nullptr ? a->lmat * b->lmat : cexprs;
            assert(exprs->data.size() == c->lmat->data.size());
            vector<shared_ptr<SparseMatrix<S>>> mats(exprs->data.size());
            for (size_t i = 0; i < exprs->data.size(); i++)
                mats[i] = c->ops.at(abs_value(c->lmat->data[i]));
            auto f = [&a, &b, this](const shared_ptr<OpExpr<S>> &expr,
                                    shared_ptr<SparseMatrix<S>> &mat) {
                assert(mat->data == nullptr);
                mat->allocate(mat->info);
                this->tensor_product(expr, a->ops, b->ops, mat);
            };
            auto g = [this]() {
                if (this->opf->seq->mode == SeqTypes::Auto)
                    this->opf->seq->auto_perform();
            };
            rule->parallel_apply(f, g, c->lmat->data, exprs->data, mats);
        }
    }
    // c = b (dot) x a
    void right_contract(
        const shared_ptr<OperatorTensor<S>> &a,
        const shared_ptr<OperatorTensor<S>> &b,
        shared_ptr<OperatorTensor<S>> &c,
        const shared_ptr<Symbolic<S>> &cexprs = nullptr) const override {
        if (a == nullptr)
            right_assign(b, c);
        else {
            shared_ptr<Symbolic<S>> exprs =
                cexprs == nullptr ? b->rmat * a->rmat : cexprs;
            assert(exprs->data.size() == c->rmat->data.size());
            vector<shared_ptr<SparseMatrix<S>>> mats(exprs->data.size());
            for (size_t i = 0; i < exprs->data.size(); i++)
                mats[i] = c->ops.at(abs_value(c->rmat->data[i]));
            auto f = [&a, &b, this](const shared_ptr<OpExpr<S>> &expr,
                                    shared_ptr<SparseMatrix<S>> &mat) {
                assert(mat->data == nullptr);
                mat->allocate(mat->info);
                this->tensor_product(expr, b->ops, a->ops, mat);
            };
            auto g = [this]() {
                if (this->opf->seq->mode == SeqTypes::Auto)
                    this->opf->seq->auto_perform();
            };
            rule->parallel_apply(f, g, c->rmat->data, exprs->data, mats);
        }
    }
};

} // namespace block2