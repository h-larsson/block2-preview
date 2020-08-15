
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

#include "expr.hpp"
#include "matrix.hpp"
#include "moving_environment.hpp"
#include "sparse_matrix.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace std;

namespace block2 {

enum struct DecompositionTypes : uint8_t { SVD = 0, DensityMatrix = 1 };

// Density Matrix Renormalization Group
template <typename S> struct DMRG {
    shared_ptr<MovingEnvironment<S>> me;
    vector<uint16_t> bond_dims;
    vector<double> noises;
    vector<vector<double>> energies;
    vector<vector<vector<pair<S, double>>>> mps_quanta;
    vector<double> davidson_conv_thrds;
    int davidson_max_iter = 5000;
    bool forward;
    uint8_t iprint = 2;
    NoiseTypes noise_type = NoiseTypes::DensityMatrix;
    TruncationTypes trunc_type = TruncationTypes::Physical;
    DecompositionTypes decomp_type = DecompositionTypes::DensityMatrix;
    double cutoff = 1E-14;
    double quanta_cutoff = 1E-3;
    DMRG(const shared_ptr<MovingEnvironment<S>> &me,
         const vector<uint16_t> &bond_dims, const vector<double> &noises)
        : me(me), bond_dims(bond_dims), noises(noises), forward(false) {}
    struct Iteration {
        vector<double> energies;
        vector<vector<pair<S, double>>> quanta;
        double error;
        int ndav;
        double tdav;
        size_t nflop;
        Iteration(const vector<double> &energies, double error, int ndav,
                  size_t nflop = 0, double tdav = 1.0,
                  const vector<vector<pair<S, double>>> &quanta =
                      vector<vector<pair<S, double>>>())
            : energies(energies), error(error), ndav(ndav), nflop(nflop),
              tdav(tdav) {}
        friend ostream &operator<<(ostream &os, const Iteration &r) {
            os << fixed << setprecision(8);
            os << "Ndav = " << setw(4) << r.ndav;
            if (r.energies.size() == 1)
                os << " E = " << setw(15) << r.energies[0];
            else if (r.quanta.size() == 0) {
                os << " E = ";
                for (auto x : r.energies)
                    os << setw(15) << setprecision(8) << x;
            }
            os << " Error = " << setw(15) << setprecision(12) << r.error
               << " FLOPS = " << scientific << setw(8) << setprecision(2)
               << (double)r.nflop / r.tdav << " Tdav = " << fixed
               << setprecision(2) << r.tdav;
            if (r.energies.size() != 1 && r.quanta.size() != 0) {
                for (size_t i = 0; i < r.energies.size(); i++) {
                    os << endl;
                    os << setw(15) << " .. E[" << setw(3) << i << "] = ";
                    os << setw(15) << setprecision(8) << r.energies[i];
                    for (size_t j = 0; j < r.quanta[i].size(); j++)
                        os << " " << setw(20) << r.quanta[i][j].first << " ("
                           << setw(8) << setprecision(6)
                           << r.quanta[i][j].second << ")";
                }
            }
            return os;
        }
    };
    Iteration update_two_dot(int i, bool forward, uint16_t bond_dim,
                             double noise, double davidson_conv_thrd) {
        frame->activate(0);
        if (me->ket->tensors[i] != nullptr &&
            me->ket->tensors[i + 1] != nullptr)
            MovingEnvironment<S>::contract_two_dot(i, me->ket);
        else {
            me->ket->load_tensor(i);
            me->ket->tensors[i + 1] = nullptr;
        }
        shared_ptr<SparseMatrix<S>> old_wfn = me->ket->tensors[i];
        shared_ptr<EffectiveHamiltonian<S>> h_eff =
            me->eff_ham(FuseTypes::FuseLR, true);
        auto pdi =
            h_eff->eigs(iprint >= 3, davidson_conv_thrd, davidson_max_iter);
        shared_ptr<SparseMatrix<S>> dm;
        double error = 0.0;
        if (decomp_type == DecompositionTypes::DensityMatrix) {
            if (noise_type == NoiseTypes::Perturbative && noise != 0) {
                shared_ptr<SparseMatrixGroup<S>> pket =
                    h_eff->perturbative_noise_two_dot(forward, i,
                                                      me->ket->info);
                h_eff->deallocate();
                dm = MovingEnvironment<
                    S>::density_matrix_with_perturbative_noise(h_eff->opdq,
                                                               h_eff->ket,
                                                               forward, noise,
                                                               pket);
                frame->activate(1);
                pket->deallocate();
                pket->deallocate_infos();
                frame->activate(0);
            } else {
                h_eff->deallocate();
                dm = MovingEnvironment<S>::density_matrix(
                    h_eff->opdq, h_eff->ket, forward, noise, noise_type);
            }
            error = MovingEnvironment<S>::split_density_matrix(
                dm, h_eff->ket, (int)bond_dim, forward, true,
                me->ket->tensors[i], me->ket->tensors[i + 1], cutoff,
                trunc_type);
        } else if (decomp_type == DecompositionTypes::SVD) {
            assert(noise_type == NoiseTypes::None ||
                   noise_type == NoiseTypes::Wavefunction);
            h_eff->deallocate();
            if (noise_type == NoiseTypes::Wavefunction && noise != 0)
                MovingEnvironment<S>::wavefunction_add_noise(h_eff->ket, noise);
            error = MovingEnvironment<S>::split_wavefunction_svd(
                h_eff->opdq, h_eff->ket, (int)bond_dim, forward, true,
                me->ket->tensors[i], me->ket->tensors[i + 1], cutoff,
                trunc_type);
        } else
            assert(false);
        shared_ptr<StateInfo<S>> info = nullptr;
        if (forward) {
            info = me->ket->tensors[i]->info->extract_state_info(forward);
            me->ket->info->left_dims[i + 1] = *info;
            me->ket->info->save_left_dims(i + 1);
            me->ket->canonical_form[i] = 'L';
            me->ket->canonical_form[i + 1] = 'C';
        } else {
            info = me->ket->tensors[i + 1]->info->extract_state_info(forward);
            me->ket->info->right_dims[i + 1] = *info;
            me->ket->info->save_right_dims(i + 1);
            me->ket->canonical_form[i] = 'C';
            me->ket->canonical_form[i + 1] = 'R';
        }
        info->deallocate();
        me->ket->save_tensor(i + 1);
        me->ket->save_tensor(i);
        me->ket->unload_tensor(i + 1);
        me->ket->unload_tensor(i);
        if (dm != nullptr) {
            dm->info->deallocate();
            dm->deallocate();
        }
        old_wfn->info->deallocate();
        old_wfn->deallocate();
        MovingEnvironment<S>::propagate_wfn(i, me->n_sites, me->ket, forward,
                                            me->mpo->tf->opf->cg);
        return Iteration(vector<double>{get<0>(pdi) + me->mpo->const_e}, error,
                         get<1>(pdi), get<2>(pdi), get<3>(pdi));
    }
    // State-averaged
    Iteration update_multi_two_dot(int i, bool forward, uint16_t bond_dim,
                                   double noise, double davidson_conv_thrd) {
        shared_ptr<MultiMPS<S>> mket =
            dynamic_pointer_cast<MultiMPS<S>>(me->ket);
        frame->activate(0);
        if (mket->tensors[i] != nullptr || mket->tensors[i + 1] != nullptr)
            MovingEnvironment<S>::contract_multi_two_dot(i, mket);
        else
            mket->load_tensor(i);
        mket->tensors[i] = mket->tensors[i + 1] = nullptr;
        vector<shared_ptr<SparseMatrixGroup<S>>> old_wfns = mket->wfns;
        shared_ptr<EffectiveHamiltonian<S, MultiMPS<S>>> h_eff =
            me->multi_eff_ham(FuseTypes::FuseLR, true);
        auto pdi =
            h_eff->eigs(iprint >= 3, davidson_conv_thrd, davidson_max_iter);
        vector<vector<pair<S, double>>> mps_quanta(mket->nroots);
        for (int i = 0; i < mket->nroots; i++) {
            mps_quanta[i] = h_eff->ket[i]->delta_quanta();
            mps_quanta[i].erase(
                remove_if(mps_quanta[i].begin(), mps_quanta[i].end(),
                          [this](const pair<S, double> &p) {
                              return p.second < this->quanta_cutoff;
                          }),
                mps_quanta[i].end());
        }
        shared_ptr<SparseMatrix<S>> dm;
        assert(noise_type != NoiseTypes::Perturbative);
        assert(decomp_type == DecompositionTypes::DensityMatrix);
        h_eff->deallocate();
        dm = MovingEnvironment<S>::density_matrix_with_multi_target(
            h_eff->opdq, h_eff->ket, mket->weights, forward, noise, noise_type);
        double error = MovingEnvironment<S>::multi_split_density_matrix(
            dm, h_eff->ket, (int)bond_dim, forward, true, mket->wfns,
            forward ? mket->tensors[i] : mket->tensors[i + 1], cutoff,
            trunc_type);
        shared_ptr<StateInfo<S>> info = nullptr;
        if (forward) {
            info = me->ket->tensors[i]->info->extract_state_info(forward);
            me->ket->info->left_dims[i + 1] = *info;
            me->ket->info->save_left_dims(i + 1);
            me->ket->canonical_form[i] = 'L';
            me->ket->canonical_form[i + 1] = 'M';
        } else {
            info = me->ket->tensors[i + 1]->info->extract_state_info(forward);
            me->ket->info->right_dims[i + 1] = *info;
            me->ket->info->save_right_dims(i + 1);
            me->ket->canonical_form[i] = 'M';
            me->ket->canonical_form[i + 1] = 'R';
        }
        info->deallocate();
        if (forward) {
            mket->save_wavefunction(i + 1);
            mket->save_tensor(i);
            mket->unload_wavefunction(i + 1);
            mket->unload_tensor(i);
        } else {
            mket->save_tensor(i + 1);
            mket->save_wavefunction(i);
            mket->unload_tensor(i + 1);
            mket->unload_wavefunction(i);
        }
        dm->info->deallocate();
        dm->deallocate();
        for (int k = mket->nroots - 1; k >= 0; k--)
            old_wfns[k]->deallocate();
        old_wfns[0]->deallocate_infos();
        MovingEnvironment<S>::propagate_multi_wfn(i, me->n_sites, mket, forward,
                                                  me->mpo->tf->opf->cg);
        for (auto &x : get<0>(pdi))
            x += me->mpo->const_e;
        Iteration r = Iteration(get<0>(pdi), error, get<1>(pdi), get<2>(pdi),
                                get<3>(pdi));
        r.quanta = mps_quanta;
        return r;
    }
    Iteration blocking(int i, bool forward, uint16_t bond_dim, double noise,
                       double davidson_conv_thrd) {
        me->move_to(i);
        if (me->dot == 2) {
            if (me->ket->canonical_form[i] == 'M' ||
                me->ket->canonical_form[i + 1] == 'M')
                return update_multi_two_dot(i, forward, bond_dim, noise,
                                            davidson_conv_thrd);
            else
                return update_two_dot(i, forward, bond_dim, noise,
                                      davidson_conv_thrd);
        } else
            throw runtime_error("1 site not yet implemented");
    }
    pair<vector<double>, vector<vector<pair<S, double>>>>
    sweep(bool forward, uint16_t bond_dim, double noise,
          double davidson_conv_thrd) {
        me->prepare();
        vector<vector<double>> energies;
        vector<vector<vector<pair<S, double>>>> quanta;
        vector<int> sweep_range;
        if (forward)
            for (int it = me->center; it < me->n_sites - me->dot + 1; it++)
                sweep_range.push_back(it);
        else
            for (int it = me->center; it >= 0; it--)
                sweep_range.push_back(it);

        Timer t;
        for (auto i : sweep_range) {
            check_signal_()();
            if (iprint >= 2) {
                if (me->dot == 2)
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << "-" << setw(4)
                         << i + 1 << " .. ";
                else
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << " .. ";
                cout.flush();
            }
            t.get_time();
            Iteration r =
                blocking(i, forward, bond_dim, noise, davidson_conv_thrd);
            if (iprint >= 2)
                cout << r << " T = " << setw(4) << fixed << setprecision(2)
                     << t.get_time() << endl;
            energies.push_back(r.energies);
            quanta.push_back(r.quanta);
        }
        size_t idx =
            min_element(energies.begin(), energies.end(),
                        [](const vector<double> &x, const vector<double> &y) {
                            return x[0] < y[0];
                        }) -
            energies.begin();
        return make_pair(energies[idx], quanta[idx]);
    }
    double solve(int n_sweeps, bool forward = true, double tol = 1E-6) {
        if (bond_dims.size() < n_sweeps)
            bond_dims.resize(n_sweeps, bond_dims.back());
        if (noises.size() < n_sweeps)
            noises.resize(n_sweeps, noises.back());
        if (davidson_conv_thrds.size() < n_sweeps)
            for (size_t i = davidson_conv_thrds.size(); i < noises.size(); i++)
                davidson_conv_thrds.push_back(
                    (noises[i] == 0 ? (tol == 0 ? 1E-9 : tol) : noises[i]) *
                    0.1);
        Timer start, current;
        start.get_time();
        energies.clear();
        mps_quanta.clear();
        for (int iw = 0; iw < n_sweeps; iw++) {
            if (iprint >= 1)
                cout << "Sweep = " << setw(4) << iw
                     << " | Direction = " << setw(8)
                     << (forward ? "forward" : "backward")
                     << " | Bond dimension = " << setw(4) << bond_dims[iw]
                     << " | Noise = " << scientific << setw(9)
                     << setprecision(2) << noises[iw]
                     << " | Dav threshold = " << scientific << setw(9)
                     << setprecision(2) << davidson_conv_thrds[iw] << endl;
            auto sweep_results = sweep(forward, bond_dims[iw], noises[iw],
                                       davidson_conv_thrds[iw]);
            energies.push_back(sweep_results.first);
            mps_quanta.push_back(sweep_results.second);
            bool converged = energies.size() >= 2 && tol > 0 &&
                             abs(energies[energies.size() - 1].back() -
                                 energies[energies.size() - 2].back()) < tol &&
                             noises[iw] == noises.back() &&
                             bond_dims[iw] == bond_dims.back();
            forward = !forward;
            current.get_time();
            if (iprint == 1) {
                cout << fixed << setprecision(8);
                if (sweep_results.first.size() == 1)
                    cout << " .. Energy = " << setw(15)
                         << sweep_results.first[0] << " ";
                else {
                    cout << " .. Energy[" << setw(3)
                         << sweep_results.first.size() << "] = ";
                    for (double x : sweep_results.first)
                        cout << setw(15) << x;
                    cout << " ";
                }
            }
            if (iprint >= 1)
                cout << "Time elapsed = " << setw(10) << setprecision(3)
                     << current.current - start.current << endl;
            if (converged)
                break;
        }
        this->forward = forward;
        return energies.back()[0];
    }
};

enum struct TETypes : uint8_t { TangentSpace, RK4 };

enum struct TruncPatternTypes : uint8_t { None, TruncAfterOdd, TruncAfterEven };

// Imaginary Time Evolution
template <typename S> struct ImaginaryTE {
    shared_ptr<MovingEnvironment<S>> me;
    vector<uint16_t> bond_dims;
    vector<double> noises;
    vector<double> errors;
    vector<double> energies;
    vector<double> normsqs;
    NoiseTypes noise_type = NoiseTypes::DensityMatrix;
    TruncationTypes trunc_type = TruncationTypes::Physical;
    TruncPatternTypes trunc_pattern = TruncPatternTypes::None;
    bool forward;
    TETypes mode;
    int n_sub_sweeps;
    vector<double> weights = {1.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0, 1.0 / 3.0};
    uint8_t iprint = 2;
    double cutoff = 1E-14;
    ImaginaryTE(const shared_ptr<MovingEnvironment<S>> &me,
                const vector<uint16_t> &bond_dims,
                TETypes mode = TETypes::TangentSpace, int n_sub_sweeps = 1)
        : me(me), bond_dims(bond_dims), noises(vector<double>{0.0}),
          forward(false), mode(mode), n_sub_sweeps(n_sub_sweeps) {}
    struct Iteration {
        double energy, normsq, error;
        int nexpo, nexpok;
        double texpo;
        size_t nflop;
        Iteration(double energy, double normsq, double error, int nexpo,
                  int nexpok, size_t nflop = 0, double texpo = 1.0)
            : energy(energy), normsq(normsq), error(error), nexpo(nexpo),
              nexpok(nexpok), nflop(nflop), texpo(texpo) {}
        friend ostream &operator<<(ostream &os, const Iteration &r) {
            os << fixed << setprecision(8);
            os << "Nexpo = " << setw(4) << r.nexpo << "/" << setw(4) << r.nexpok
               << " E = " << setw(15) << r.energy << " Error = " << setw(15)
               << setprecision(12) << r.error << " FLOPS = " << scientific
               << setw(8) << setprecision(2) << (double)r.nflop / r.texpo
               << " Texpo = " << fixed << setprecision(2) << r.texpo;
            return os;
        }
    };
    Iteration update_two_dot(int i, bool forward, bool advance, double beta,
                             uint16_t bond_dim, double noise) {
        frame->activate(0);
        if (me->ket->tensors[i] != nullptr &&
            me->ket->tensors[i + 1] != nullptr)
            MovingEnvironment<S>::contract_two_dot(i, me->ket);
        else {
            me->ket->load_tensor(i);
            me->ket->tensors[i + 1] = nullptr;
        }
        shared_ptr<EffectiveHamiltonian<S>> h_eff =
            me->eff_ham(FuseTypes::FuseLR, true);
        tuple<double, double, int, size_t, double> pdi;
        shared_ptr<SparseMatrix<S>> old_wfn = me->ket->tensors[i];
        TETypes effective_mode = mode;
        if (mode == TETypes::RK4 &&
            ((forward && i + 1 == me->n_sites - 1) || (!forward && i == 0)))
            effective_mode = TETypes::TangentSpace;
        shared_ptr<SparseMatrix<S>> dm;
        if (!advance &&
            ((forward && i + 1 == me->n_sites - 1) || (!forward && i == 0))) {
            assert(effective_mode == TETypes::TangentSpace);
            // TangentSpace method does not allow multiple sweeps for one time
            // step
            assert(mode == TETypes::RK4);
            MatrixRef tmp(nullptr, h_eff->ket->total_memory, 1);
            tmp.allocate();
            memcpy(tmp.data, h_eff->ket->data,
                   h_eff->ket->total_memory * sizeof(double));
            pdi = h_eff->expo_apply(-beta, me->mpo->const_e, iprint >= 3);
            memcpy(h_eff->ket->data, tmp.data,
                   h_eff->ket->total_memory * sizeof(double));
            tmp.deallocate();
            auto pdp = h_eff->rk4_apply(-beta, me->mpo->const_e, false);
            h_eff->deallocate();
            dm = MovingEnvironment<S>::density_matrix_with_weights(
                h_eff->opdq, h_eff->ket, forward, noise, pdp.first, weights,
                noise_type);
            frame->activate(1);
            for (int i = pdp.first.size() - 1; i >= 0; i--)
                pdp.first[i].deallocate();
            frame->activate(0);
        } else if (effective_mode == TETypes::TangentSpace) {
            pdi = h_eff->expo_apply(-beta, me->mpo->const_e, iprint >= 3);
            h_eff->deallocate();
            dm = MovingEnvironment<S>::density_matrix(
                h_eff->opdq, h_eff->ket, forward, noise, noise_type);
        } else if (effective_mode == TETypes::RK4) {
            auto pdp = h_eff->rk4_apply(-beta, me->mpo->const_e, false);
            pdi = pdp.second;
            h_eff->deallocate();
            dm = MovingEnvironment<S>::density_matrix_with_weights(
                h_eff->opdq, h_eff->ket, forward, noise, pdp.first, weights,
                noise_type);
            frame->activate(1);
            for (int i = pdp.first.size() - 1; i >= 0; i--)
                pdp.first[i].deallocate();
            frame->activate(0);
        }
        int bdim = bond_dim;
        if ((this->trunc_pattern == TruncPatternTypes::TruncAfterOdd &&
             i % 2 == 0) ||
            (this->trunc_pattern == TruncPatternTypes::TruncAfterEven &&
             i % 2 == 1))
            bdim = -1;
        double error = MovingEnvironment<S>::split_density_matrix(
            dm, h_eff->ket, bdim, forward, false, me->ket->tensors[i],
            me->ket->tensors[i + 1], cutoff, trunc_type);
        shared_ptr<StateInfo<S>> info = nullptr;
        if (forward) {
            if (mode == TETypes::RK4 && (i + 1 != me->n_sites - 1 || !advance))
                me->ket->tensors[i + 1]->normalize();
            info = me->ket->tensors[i]->info->extract_state_info(forward);
            me->ket->info->left_dims[i + 1] = *info;
            me->ket->info->save_left_dims(i + 1);
            me->ket->canonical_form[i] = 'L';
            me->ket->canonical_form[i + 1] = 'C';
        } else {
            if (mode == TETypes::RK4 && (i != 0 || !advance))
                me->ket->tensors[i]->normalize();
            info = me->ket->tensors[i + 1]->info->extract_state_info(forward);
            me->ket->info->right_dims[i + 1] = *info;
            me->ket->info->save_right_dims(i + 1);
            me->ket->canonical_form[i] = 'C';
            me->ket->canonical_form[i + 1] = 'R';
        }
        info->deallocate();
        me->ket->save_tensor(i + 1);
        me->ket->save_tensor(i);
        me->ket->unload_tensor(i + 1);
        me->ket->unload_tensor(i);
        dm->info->deallocate();
        dm->deallocate();
        old_wfn->info->deallocate();
        old_wfn->deallocate();
        int expok = 0;
        if (mode == TETypes::TangentSpace && forward &&
            i + 1 != me->n_sites - 1) {
            me->move_to(i + 1);
            me->ket->load_tensor(i + 1);
            shared_ptr<EffectiveHamiltonian<S>> k_eff =
                me->eff_ham(FuseTypes::FuseR, true);
            auto pdk = k_eff->expo_apply(beta, me->mpo->const_e, iprint >= 3);
            k_eff->deallocate();
            me->ket->tensors[i + 1]->normalize();
            me->ket->save_tensor(i + 1);
            me->ket->unload_tensor(i + 1);
            get<3>(pdi) += get<3>(pdk), get<4>(pdi) += get<4>(pdk);
            expok = get<2>(pdk);
        } else if (mode == TETypes::TangentSpace && !forward && i != 0) {
            me->move_to(i - 1);
            me->ket->load_tensor(i);
            shared_ptr<EffectiveHamiltonian<S>> k_eff =
                me->eff_ham(FuseTypes::FuseL, true);
            auto pdk = k_eff->expo_apply(beta, me->mpo->const_e, iprint >= 3);
            k_eff->deallocate();
            me->ket->tensors[i]->normalize();
            me->ket->save_tensor(i);
            me->ket->unload_tensor(i);
            get<3>(pdi) += get<3>(pdk), get<4>(pdi) += get<4>(pdk);
            expok = get<2>(pdk);
        }
        MovingEnvironment<S>::propagate_wfn(i, me->n_sites, me->ket, forward,
                                            me->mpo->tf->opf->cg);
        return Iteration(get<0>(pdi) + me->mpo->const_e,
                         get<1>(pdi) * get<1>(pdi), error, get<2>(pdi), expok,
                         get<3>(pdi), get<4>(pdi));
    }
    Iteration blocking(int i, bool forward, bool advance, double beta,
                       uint16_t bond_dim, double noise) {
        me->move_to(i);
        if (me->dot == 2)
            return update_two_dot(i, forward, advance, beta, bond_dim, noise);
        else
            throw runtime_error("1 site not yet implemented");
    }
    tuple<double, double, double> sweep(bool forward, bool advance, double beta,
                                        uint16_t bond_dim, double noise) {
        me->prepare();
        vector<double> energies, normsqs;
        vector<int> sweep_range;
        double largest_error = 0.0;
        if (forward)
            for (int it = me->center; it < me->n_sites - me->dot + 1; it++)
                sweep_range.push_back(it);
        else
            for (int it = me->center; it >= 0; it--)
                sweep_range.push_back(it);

        Timer t;
        for (auto i : sweep_range) {
            check_signal_()();
            if (iprint >= 2) {
                if (me->dot == 2)
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << "-" << setw(4)
                         << i + 1 << " .. ";
                else
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << " .. ";
                cout.flush();
            }
            t.get_time();
            Iteration r = blocking(i, forward, advance, beta, bond_dim, noise);
            if (iprint >= 2)
                cout << r << " T = " << setw(4) << fixed << setprecision(2)
                     << t.get_time() << endl;
            energies.push_back(r.energy);
            normsqs.push_back(r.normsq);
            largest_error = max(largest_error, r.error);
        }
        return make_tuple(energies.back(), normsqs.back(), largest_error);
    }
    void normalize() {
        size_t center = me->ket->canonical_form.find('C');
        assert(center != string::npos);
        me->ket->load_tensor(center);
        me->ket->tensors[center]->normalize();
        me->ket->save_tensor(center);
        me->ket->unload_tensor(center);
    }
    double solve(int n_sweeps, double beta, bool forward = true,
                 double tol = 1E-6) {
        if (bond_dims.size() < n_sweeps)
            bond_dims.resize(n_sweeps, bond_dims.back());
        if (noises.size() < n_sweeps)
            noises.resize(n_sweeps, noises.back());
        Timer start, current;
        start.get_time();
        energies.clear();
        normsqs.clear();
        for (int iw = 0; iw < n_sweeps; iw++) {
            for (int isw = 0; isw < n_sub_sweeps; isw++) {
                if (iprint >= 1) {
                    cout << "Sweep = " << setw(4) << iw;
                    if (n_sub_sweeps != 1)
                        cout << " (" << setw(2) << isw << "/" << setw(2)
                             << (int)n_sub_sweeps << ")";
                    cout << " | Direction = " << setw(8)
                         << (forward ? "forward" : "backward")
                         << " | Beta = " << fixed << setw(10) << setprecision(5)
                         << beta << " | Bond dimension = " << setw(4)
                         << bond_dims[iw] << " | Noise = " << scientific
                         << setw(9) << setprecision(2) << noises[iw] << endl;
                }
                auto r = sweep(forward, isw == n_sub_sweeps - 1, beta,
                               bond_dims[iw], noises[iw]);
                forward = !forward;
                current.get_time();
                if (iprint == 1) {
                    cout << fixed << setprecision(8);
                    cout << " .. Energy = " << setw(15) << get<0>(r)
                         << " Norm = " << setw(15) << sqrt(get<1>(r))
                         << " MaxError = " << setw(15) << setprecision(12)
                         << get<2>(r) << " ";
                }
                if (iprint >= 1)
                    cout << "Time elapsed = " << setw(10) << setprecision(3)
                         << current.current - start.current << endl;
                if (isw == n_sub_sweeps - 1) {
                    energies.push_back(get<0>(r));
                    normsqs.push_back(get<1>(r));
                }
            }
            normalize();
        }
        this->forward = forward;
        return energies.back();
    }
};

// Compression
template <typename S> struct Compress {
    shared_ptr<MovingEnvironment<S>> me;
    vector<uint16_t> bra_bond_dims, ket_bond_dims;
    vector<double> noises;
    vector<double> norms;
    NoiseTypes noise_type = NoiseTypes::DensityMatrix;
    TruncationTypes trunc_type = TruncationTypes::Physical;
    bool forward;
    uint8_t iprint = 2;
    double cutoff = 0.0;
    Compress(const shared_ptr<MovingEnvironment<S>> &me,
             const vector<uint16_t> &bra_bond_dims,
             const vector<uint16_t> &ket_bond_dims,
             const vector<double> &noises)
        : me(me), bra_bond_dims(bra_bond_dims), ket_bond_dims(ket_bond_dims),
          noises(noises), forward(false) {}
    struct Iteration {
        double norm, error;
        double tmult;
        size_t nflop;
        Iteration(double norm, double error, size_t nflop = 0,
                  double tmult = 1.0)
            : norm(norm), error(error), nflop(nflop), tmult(tmult) {}
        friend ostream &operator<<(ostream &os, const Iteration &r) {
            os << fixed << setprecision(8);
            os << " Norm = " << setw(15) << r.norm << " Error = " << setw(15)
               << setprecision(12) << r.error << " FLOPS = " << scientific
               << setw(8) << setprecision(2) << (double)r.nflop / r.tmult
               << " Tmult = " << fixed << setprecision(2) << r.tmult;
            return os;
        }
    };
    Iteration update_two_dot(int i, bool forward, uint16_t bra_bond_dim,
                             uint16_t ket_bond_dim, double noise) {
        assert(me->bra != me->ket);
        frame->activate(0);
        for (auto &mps : {me->bra, me->ket}) {
            if (mps->tensors[i] != nullptr && mps->tensors[i + 1] != nullptr)
                MovingEnvironment<S>::contract_two_dot(i, mps, mps == me->ket);
            else {
                mps->load_tensor(i);
                mps->tensors[i + 1] = nullptr;
            }
        }
        shared_ptr<EffectiveHamiltonian<S>> h_eff =
            me->eff_ham(FuseTypes::FuseLR, false);
        auto pdi = h_eff->multiply();
        h_eff->deallocate();
        shared_ptr<SparseMatrix<S>> old_bra = me->bra->tensors[i];
        shared_ptr<SparseMatrix<S>> old_ket = me->ket->tensors[i];
        double bra_error = 0.0;
        for (auto &mps : {me->bra, me->ket}) {
            shared_ptr<SparseMatrix<S>> old_wfn = mps->tensors[i];
            shared_ptr<SparseMatrix<S>> dm =
                MovingEnvironment<S>::density_matrix(
                    h_eff->opdq, old_wfn, forward, mps == me->bra ? noise : 0.0,
                    mps == me->bra ? noise_type : NoiseTypes::None);
            int bond_dim =
                mps == me->bra ? (int)bra_bond_dim : (int)ket_bond_dim;
            double error = MovingEnvironment<S>::split_density_matrix(
                dm, old_wfn, bond_dim, forward, false, mps->tensors[i],
                mps->tensors[i + 1], cutoff, trunc_type);
            if (mps == me->bra)
                bra_error = error;
            shared_ptr<StateInfo<S>> info = nullptr;
            if (forward) {
                info = mps->tensors[i]->info->extract_state_info(forward);
                mps->info->left_dims[i + 1] = *info;
                mps->info->save_left_dims(i + 1);
                mps->canonical_form[i] = 'L';
                mps->canonical_form[i + 1] = 'C';
            } else {
                info = mps->tensors[i + 1]->info->extract_state_info(forward);
                mps->info->right_dims[i + 1] = *info;
                mps->info->save_right_dims(i + 1);
                mps->canonical_form[i] = 'C';
                mps->canonical_form[i + 1] = 'R';
            }
            info->deallocate();
            mps->save_tensor(i + 1);
            mps->save_tensor(i);
            mps->unload_tensor(i + 1);
            mps->unload_tensor(i);
            dm->info->deallocate();
            dm->deallocate();
            MovingEnvironment<S>::propagate_wfn(i, me->n_sites, mps, forward,
                                                me->mpo->tf->opf->cg);
        }
        for (auto &old_wfn : {old_ket, old_bra}) {
            old_wfn->info->deallocate();
            old_wfn->deallocate();
        }
        return Iteration(get<0>(pdi), bra_error, get<1>(pdi), get<2>(pdi));
    }
    Iteration blocking(int i, bool forward, uint16_t bra_bond_dim,
                       uint16_t ket_bond_dim, double noise) {
        me->move_to(i);
        if (me->dot == 2)
            return update_two_dot(i, forward, bra_bond_dim, ket_bond_dim,
                                  noise);
        else
            throw runtime_error("1 site not yet implemented");
    }
    double sweep(bool forward, uint16_t bra_bond_dim, uint16_t ket_bond_dim,
                 double noise) {
        me->prepare();
        vector<double> norms;
        vector<int> sweep_range;
        if (forward)
            for (int it = me->center; it < me->n_sites - me->dot + 1; it++)
                sweep_range.push_back(it);
        else
            for (int it = me->center; it >= 0; it--)
                sweep_range.push_back(it);

        Timer t;
        for (auto i : sweep_range) {
            check_signal_()();
            if (iprint >= 2) {
                if (me->dot == 2)
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << "-" << setw(4)
                         << i + 1 << " .. ";
                else
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << " .. ";
                cout.flush();
            }
            t.get_time();
            Iteration r =
                blocking(i, forward, bra_bond_dim, ket_bond_dim, noise);
            if (iprint >= 2)
                cout << r << " T = " << setw(4) << fixed << setprecision(2)
                     << t.get_time() << endl;
            norms.push_back(r.norm);
        }
        return norms.back();
    }
    double solve(int n_sweeps, bool forward = true, double tol = 1E-6) {
        if (bra_bond_dims.size() < n_sweeps)
            bra_bond_dims.resize(n_sweeps, bra_bond_dims.back());
        if (ket_bond_dims.size() < n_sweeps)
            ket_bond_dims.resize(n_sweeps, ket_bond_dims.back());
        if (noises.size() < n_sweeps)
            noises.resize(n_sweeps, noises.back());
        Timer start, current;
        start.get_time();
        norms.clear();
        for (int iw = 0; iw < n_sweeps; iw++) {
            if (iprint >= 1)
                cout << "Sweep = " << setw(4) << iw
                     << " | Direction = " << setw(8)
                     << (forward ? "forward" : "backward")
                     << " | BRA bond dimension = " << setw(4)
                     << bra_bond_dims[iw] << " | Noise = " << scientific
                     << setw(9) << setprecision(2) << noises[iw] << endl;
            double norm = sweep(forward, bra_bond_dims[iw], ket_bond_dims[iw],
                                noises[iw]);
            norms.push_back(norm);
            bool converged =
                norms.size() >= 2 && tol > 0 &&
                abs(norms[norms.size() - 1] - norms[norms.size() - 2]) < tol &&
                noises[iw] == noises.back() &&
                bra_bond_dims[iw] == bra_bond_dims.back();
            forward = !forward;
            current.get_time();
            if (iprint == 1) {
                cout << fixed << setprecision(8);
                cout << " .. Norm = " << setw(15) << norm << " ";
            }
            if (iprint >= 1)
                cout << "Time elapsed = " << setw(10) << setprecision(3)
                     << current.current - start.current << endl;
            if (converged)
                break;
        }
        this->forward = forward;
        return norms.back();
    }
};

inline vector<long double>
get_partition_weights(double beta, const vector<double> &energies,
                      const vector<int> &multiplicities) {
    vector<long double> partition_weights(energies.size());
    for (size_t i = 0; i < energies.size(); i++)
        partition_weights[i] =
            multiplicities[i] *
            expl(-(long double)beta *
                 ((long double)energies[i] - (long double)energies[0]));
    long double psum =
        accumulate(partition_weights.begin(), partition_weights.end(), 0.0L);
    for (size_t i = 0; i < energies.size(); i++)
        partition_weights[i] /= psum;
    return partition_weights;
}

// Expectation value
template <typename S> struct Expect {
    shared_ptr<MovingEnvironment<S>> me;
    uint16_t bra_bond_dim, ket_bond_dim;
    vector<vector<pair<shared_ptr<OpExpr<S>>, double>>> expectations;
    bool forward;
    TruncationTypes trunc_type = TruncationTypes::Physical;
    uint8_t iprint = 2;
    double cutoff = 0.0;
    double beta = 0.0;
    // partition function (for thermal-averaged MultiMPS)
    vector<long double> partition_weights;
    Expect(const shared_ptr<MovingEnvironment<S>> &me, uint16_t bra_bond_dim,
           uint16_t ket_bond_dim)
        : me(me), bra_bond_dim(bra_bond_dim), ket_bond_dim(ket_bond_dim),
          forward(false) {
        expectations.resize(me->n_sites - me->dot + 1);
        partition_weights = vector<long double>{1.0L};
    }
    Expect(const shared_ptr<MovingEnvironment<S>> &me, uint16_t bra_bond_dim,
           uint16_t ket_bond_dim, double beta, const vector<double> &energies,
           const vector<int> &multiplicities)
        : Expect(me, bra_bond_dim, ket_bond_dim) {
        this->beta = beta;
        this->partition_weights =
            get_partition_weights(beta, energies, multiplicities);
    }
    struct Iteration {
        vector<pair<shared_ptr<OpExpr<S>>, double>> expectations;
        double bra_error, ket_error;
        double tmult;
        size_t nflop;
        Iteration(
            const vector<pair<shared_ptr<OpExpr<S>>, double>> &expectations,
            double bra_error, double ket_error, size_t nflop = 0,
            double tmult = 1.0)
            : expectations(expectations), bra_error(bra_error),
              ket_error(ket_error), nflop(nflop), tmult(tmult) {}
        friend ostream &operator<<(ostream &os, const Iteration &r) {
            os << fixed << setprecision(8);
            if (r.expectations.size() == 1)
                os << " " << setw(14) << r.expectations[0].second;
            else
                os << " Nterms = " << setw(6) << r.expectations.size();
            os << " Error = " << setw(15) << setprecision(12) << r.bra_error
               << "/" << setw(15) << setprecision(12) << r.ket_error
               << " FLOPS = " << scientific << setw(8) << setprecision(2)
               << (double)r.nflop / r.tmult << " Tmult = " << fixed
               << setprecision(2) << r.tmult;
            return os;
        }
    };
    Iteration update_two_dot(int i, bool forward, bool propagate,
                             uint16_t bra_bond_dim, uint16_t ket_bond_dim) {
        frame->activate(0);
        vector<shared_ptr<MPS<S>>> mpss =
            me->bra == me->ket ? vector<shared_ptr<MPS<S>>>{me->bra}
                               : vector<shared_ptr<MPS<S>>>{me->bra, me->ket};
        for (auto &mps : mpss) {
            if (mps->tensors[i] != nullptr && mps->tensors[i + 1] != nullptr)
                MovingEnvironment<S>::contract_two_dot(i, mps, mps == me->ket);
            else {
                mps->load_tensor(i);
                mps->tensors[i + 1] = nullptr;
            }
        }
        shared_ptr<EffectiveHamiltonian<S>> h_eff =
            me->eff_ham(FuseTypes::FuseLR, false);
        auto pdi = h_eff->expect();
        h_eff->deallocate();
        vector<shared_ptr<SparseMatrix<S>>> old_wfns =
            me->bra == me->ket
                ? vector<shared_ptr<SparseMatrix<S>>>{me->bra->tensors[i]}
                : vector<shared_ptr<SparseMatrix<S>>>{me->ket->tensors[i],
                                                      me->bra->tensors[i]};
        double bra_error = 0.0, ket_error = 0.0;
        if (propagate) {
            for (auto &mps : mpss) {
                shared_ptr<SparseMatrix<S>> old_wfn = mps->tensors[i];
                shared_ptr<SparseMatrix<S>> dm =
                    MovingEnvironment<S>::density_matrix(
                        h_eff->opdq, old_wfn, forward, 0.0, NoiseTypes::None);
                int bond_dim =
                    mps == me->bra ? (int)bra_bond_dim : (int)ket_bond_dim;
                double error = MovingEnvironment<S>::split_density_matrix(
                    dm, old_wfn, bond_dim, forward, false, mps->tensors[i],
                    mps->tensors[i + 1], cutoff, trunc_type);
                if (mps == me->bra)
                    bra_error = error;
                else
                    ket_error = error;
                shared_ptr<StateInfo<S>> info = nullptr;
                if (forward) {
                    info = mps->tensors[i]->info->extract_state_info(forward);
                    mps->info->left_dims[i + 1] = *info;
                    mps->info->save_left_dims(i + 1);
                    mps->canonical_form[i] = 'L';
                    mps->canonical_form[i + 1] = 'C';
                } else {
                    info =
                        mps->tensors[i + 1]->info->extract_state_info(forward);
                    mps->info->right_dims[i + 1] = *info;
                    mps->info->save_right_dims(i + 1);
                    mps->canonical_form[i] = 'C';
                    mps->canonical_form[i + 1] = 'R';
                }
                info->deallocate();
                mps->save_tensor(i + 1);
                mps->save_tensor(i);
                mps->unload_tensor(i + 1);
                mps->unload_tensor(i);
                dm->info->deallocate();
                dm->deallocate();
                MovingEnvironment<S>::propagate_wfn(
                    i, me->n_sites, mps, forward, me->mpo->tf->opf->cg);
            }
        }
        for (auto &old_wfn : old_wfns) {
            old_wfn->info->deallocate();
            old_wfn->deallocate();
        }
        return Iteration(get<0>(pdi), bra_error, ket_error, get<1>(pdi),
                         get<2>(pdi));
    }
    Iteration update_multi_two_dot(int i, bool forward, bool propagate,
                                   uint16_t bra_bond_dim,
                                   uint16_t ket_bond_dim) {
        shared_ptr<MultiMPS<S>> mket =
                                    dynamic_pointer_cast<MultiMPS<S>>(me->ket),
                                mbra =
                                    dynamic_pointer_cast<MultiMPS<S>>(me->bra);
        if (me->bra == me->ket)
            assert(mbra == mket);
        frame->activate(0);
        vector<shared_ptr<MultiMPS<S>>> mpss =
            me->bra == me->ket ? vector<shared_ptr<MultiMPS<S>>>{mbra}
                               : vector<shared_ptr<MultiMPS<S>>>{mbra, mket};
        for (auto &mps : mpss) {
            if (mps->tensors[i] != nullptr || mps->tensors[i + 1] != nullptr)
                MovingEnvironment<S>::contract_multi_two_dot(i, mps,
                                                             mps == mket);
            else
                mps->load_tensor(i);
            mps->tensors[i] = mps->tensors[i + 1] = nullptr;
        }
        shared_ptr<EffectiveHamiltonian<S, MultiMPS<S>>> h_eff =
            me->multi_eff_ham(FuseTypes::FuseLR, false);
        auto pdi = h_eff->expect();
        h_eff->deallocate();
        vector<vector<shared_ptr<SparseMatrixGroup<S>>>> old_wfnss =
            me->bra == me->ket
                ? vector<vector<shared_ptr<SparseMatrixGroup<S>>>>{mbra->wfns}
                : vector<vector<shared_ptr<SparseMatrixGroup<S>>>>{mket->wfns,
                                                                   mbra->wfns};
        double bra_error = 0.0, ket_error = 0.0;
        if (propagate) {
            for (auto &mps : mpss) {
                vector<shared_ptr<SparseMatrixGroup<S>>> old_wfn = mps->wfns;
                shared_ptr<SparseMatrix<S>> dm =
                    MovingEnvironment<S>::density_matrix_with_multi_target(
                        h_eff->opdq, old_wfn, mps->weights, forward, 0.0,
                        NoiseTypes::None);
                int bond_dim =
                    mps == mbra ? (int)bra_bond_dim : (int)ket_bond_dim;
                double error = MovingEnvironment<S>::multi_split_density_matrix(
                    dm, old_wfn, bond_dim, forward, false, mps->wfns,
                    forward ? mps->tensors[i] : mps->tensors[i + 1], cutoff,
                    trunc_type);
                if (mps == mbra)
                    bra_error = error;
                else
                    ket_error = error;
                shared_ptr<StateInfo<S>> info = nullptr;
                if (forward) {
                    info = mps->tensors[i]->info->extract_state_info(forward);
                    mps->info->left_dims[i + 1] = *info;
                    mps->info->save_left_dims(i + 1);
                    mps->canonical_form[i] = 'L';
                    mps->canonical_form[i + 1] = 'M';
                } else {
                    info =
                        mps->tensors[i + 1]->info->extract_state_info(forward);
                    mps->info->right_dims[i + 1] = *info;
                    mps->info->save_right_dims(i + 1);
                    mps->canonical_form[i] = 'M';
                    mps->canonical_form[i + 1] = 'R';
                }
                info->deallocate();
                if (forward) {
                    mps->save_wavefunction(i + 1);
                    mps->save_tensor(i);
                    mps->unload_wavefunction(i + 1);
                    mps->unload_tensor(i);
                } else {
                    mps->save_tensor(i + 1);
                    mps->save_wavefunction(i);
                    mps->unload_tensor(i + 1);
                    mps->unload_wavefunction(i);
                }
                dm->info->deallocate();
                dm->deallocate();
                MovingEnvironment<S>::propagate_multi_wfn(
                    i, me->n_sites, mps, forward, me->mpo->tf->opf->cg);
            }
        }
        for (auto &old_wfns : old_wfnss) {
            for (int k = mket->nroots - 1; k >= 0; k--)
                old_wfns[k]->deallocate();
            old_wfns[0]->deallocate_infos();
        }
        vector<pair<shared_ptr<OpExpr<S>>, double>> expectations(
            get<0>(pdi).size());
        for (size_t k = 0; k < get<0>(pdi).size(); k++) {
            long double x = 0.0;
            for (size_t l = 0; l < partition_weights.size(); l++)
                x += partition_weights[l] * get<0>(pdi)[k].second[l];
            expectations[k] = make_pair(get<0>(pdi)[k].first, (double)x);
        }
        return Iteration(expectations, bra_error, ket_error, get<1>(pdi),
                         get<2>(pdi));
    }
    Iteration blocking(int i, bool forward, bool propagate,
                       uint16_t bra_bond_dim, uint16_t ket_bond_dim) {
        me->move_to(i);
        if (me->dot == 2) {
            if (me->ket->canonical_form[i] == 'M' ||
                me->ket->canonical_form[i + 1] == 'M')
                return update_multi_two_dot(i, forward, propagate, bra_bond_dim,
                                            ket_bond_dim);
            else
                return update_two_dot(i, forward, propagate, bra_bond_dim,
                                      ket_bond_dim);
        } else
            throw runtime_error("1 site not yet implemented");
    }
    void sweep(bool forward, uint16_t bra_bond_dim, uint16_t ket_bond_dim) {
        me->prepare();
        vector<int> sweep_range;
        if (forward)
            for (int it = me->center; it < me->n_sites - me->dot + 1; it++)
                sweep_range.push_back(it);
        else
            for (int it = me->center; it >= 0; it--)
                sweep_range.push_back(it);

        Timer t;
        for (auto i : sweep_range) {
            check_signal_()();
            if (iprint >= 2) {
                if (me->dot == 2)
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << "-" << setw(4)
                         << i + 1 << " .. ";
                else
                    cout << " " << (forward ? "-->" : "<--")
                         << " Site = " << setw(4) << i << " .. ";
                cout.flush();
            }
            t.get_time();
            Iteration r =
                blocking(i, forward, true, bra_bond_dim, ket_bond_dim);
            if (iprint >= 2)
                cout << r << " T = " << setw(4) << fixed << setprecision(2)
                     << t.get_time() << endl;
            expectations[i] = r.expectations;
        }
    }
    double solve(bool propagate, bool forward = true) {
        Timer start, current;
        start.get_time();
        for (auto &x : expectations)
            x.clear();
        if (propagate) {
            if (iprint >= 1) {
                cout << "Expectation | Direction = " << setw(8)
                     << (forward ? "forward" : "backward")
                     << " | BRA bond dimension = " << setw(4) << bra_bond_dim
                     << " | KET bond dimension = " << setw(4) << ket_bond_dim;
                if (beta != 0.0)
                    cout << " | 1/T = " << fixed << setw(10) << setprecision(5)
                         << beta;
                cout << endl;
            }
            sweep(forward, bra_bond_dim, ket_bond_dim);
            forward = !forward;
            current.get_time();
            if (iprint >= 1)
                cout << "Time elapsed = " << setw(10) << setprecision(3)
                     << current.current - start.current << endl;
            this->forward = forward;
            return 0.0;
        } else {
            Iteration r = blocking(me->center, forward, false, bra_bond_dim,
                                   ket_bond_dim);
            assert(r.expectations.size() != 0);
            return r.expectations[0].second;
        }
    }
    // only works for SU2
    MatrixRef get_1pdm_spatial(uint16_t n_physical_sites = 0U) {
        if (n_physical_sites == 0U)
            n_physical_sites = me->n_sites;
        MatrixRef r(nullptr, n_physical_sites, n_physical_sites);
        r.allocate();
        r.clear();
        for (auto &v : expectations)
            for (auto &x : v) {
                shared_ptr<OpElement<S>> op =
                    dynamic_pointer_cast<OpElement<S>>(x.first);
                assert(op->name == OpNames::PDM1);
                r(op->site_index[0], op->site_index[1]) = x.second;
            }
        return r;
    }
    // only works for SZ
    MatrixRef get_1pdm(uint16_t n_physical_sites = 0U) {
        if (n_physical_sites == 0U)
            n_physical_sites = me->n_sites;
        MatrixRef r(nullptr, n_physical_sites * 2, n_physical_sites * 2);
        r.allocate();
        r.clear();
        for (auto &v : expectations)
            for (auto &x : v) {
                shared_ptr<OpElement<S>> op =
                    dynamic_pointer_cast<OpElement<S>>(x.first);
                assert(op->name == OpNames::PDM1);
                r(2 * op->site_index[0] + op->site_index.s(0),
                  2 * op->site_index[1] + op->site_index.s(1)) = x.second;
            }
        return r;
    }
    // only works for SZ
    shared_ptr<Tensor> get_2pdm(uint16_t n_physical_sites = 0U) {
        if (n_physical_sites == 0U)
            n_physical_sites = me->n_sites;
        shared_ptr<Tensor> r = make_shared<Tensor>(
            vector<int>{n_physical_sites * 2, n_physical_sites * 2,
                        n_physical_sites * 2, n_physical_sites * 2});
        r->clear();
        for (auto &v : expectations)
            for (auto &x : v) {
                shared_ptr<OpElement<S>> op =
                    dynamic_pointer_cast<OpElement<S>>(x.first);
                assert(op->name == OpNames::PDM2);
                (*r)({op->site_index[0] * 2 + op->site_index.s(0),
                      op->site_index[1] * 2 + op->site_index.s(1),
                      op->site_index[2] * 2 + op->site_index.s(2),
                      op->site_index[3] * 2 + op->site_index.s(3)}) = x.second;
            }
        return r;
    }
    // only works for SU2
    // number of particle correlation
    // s == 0: pure spin; s == 1: mixed spin
    MatrixRef get_1npc_spatial(uint8_t s, uint16_t n_physical_sites = 0U) {
        if (n_physical_sites == 0U)
            n_physical_sites = me->n_sites;
        MatrixRef r(nullptr, n_physical_sites, n_physical_sites);
        r.allocate();
        r.clear();
        for (auto &v : expectations)
            for (auto &x : v) {
                shared_ptr<OpElement<S>> op =
                    dynamic_pointer_cast<OpElement<S>>(x.first);
                assert(op->name == OpNames::PDM1);
                assert(op->site_index.ss() < 2);
                if (s == op->site_index.ss())
                    r(op->site_index[0], op->site_index[1]) = x.second;
            }
        return r;
    }
    // only works for SZ
    // number of particle correlation
    // s == 0: pure spin; s == 1: mixed spin
    MatrixRef get_1npc(uint8_t s, uint16_t n_physical_sites = 0U) {
        if (n_physical_sites == 0U)
            n_physical_sites = me->n_sites;
        MatrixRef r(nullptr, n_physical_sites * 2, n_physical_sites * 2);
        r.allocate();
        r.clear();
        for (auto &v : expectations)
            for (auto &x : v) {
                shared_ptr<OpElement<S>> op =
                    dynamic_pointer_cast<OpElement<S>>(x.first);
                assert(op->name == OpNames::PDM1);
                if (s == 0 && op->site_index.s(2) == 0)
                    r(2 * op->site_index[0] + op->site_index.s(0),
                      2 * op->site_index[1] + op->site_index.s(1)) = x.second;
                else if (s == 1 && op->site_index.s(2) == 1)
                    r(2 * op->site_index[0] + op->site_index.s(0),
                      2 * op->site_index[1] + !op->site_index.s(0)) = x.second;
            }
        return r;
    }
};

} // namespace block2