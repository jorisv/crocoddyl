///////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (C) 2018-2019, LAAS-CNRS, New York University, Max Planck Gesellschaft
// Copyright note valid unless otherwise stated in individual files.
// All rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "crocoddyl/core/solvers/kkt.hpp"

namespace crocoddyl {

SolverKKT::SolverKKT(ShootingProblem& problem)
    : SolverAbstract(problem),
      regfactor_(10.),
      regmin_(1e-9),
      regmax_(1e9),
      cost_try_(0.),
      th_grad_(1e-12),
      th_step_(0.5),
      was_feasible_(false) {
  allocateData();
  //
  const unsigned int& n_alphas = 10;
  alphas_.resize(n_alphas);
  for (unsigned int n = 0; n < n_alphas; ++n) {
    alphas_[n] = 1. / pow(2., (double)n);
  }
}

SolverKKT::~SolverKKT() {}

double SolverKKT::calc() {
  unsigned int const& T = problem_.get_T();
  // problem calc diff
  cost_ = problem_.calcDiff(xs_, us_);
  // some indices
  int ix = 0;
  int iu = 0;
  // offset on constraint xnext = f(x,u) due to x0 = ref.
  unsigned int const& cx0 = problem_.get_runningModels()[0]->get_state().get_ndx();
  // fill diagonals of dynamics gradient as identity
  kkt_.block(ndx_ + nu_, 0, ndx_, ndx_) = Eigen::MatrixXd::Identity(ndx_, ndx_);
  // loop over models and fill out kkt matrix
  for (unsigned int t = 0; t < T; ++t) {
    ActionModelAbstract* m = problem_.running_models_[t];
    boost::shared_ptr<ActionDataAbstract>& d = problem_.running_datas_[t];
    unsigned int const& ndxi = m->get_state().get_ndx();
    unsigned int const& nui = m->get_nu();
    // compute gap at initial state
    if (t == 0) {
      m->get_state().diff(problem_.get_x0(), xs_[0], kktref_.segment(ndx_ + nu_, ndxi));
    }
    // hessian
    kkt_.block(ix, ix, ndxi, ndxi) = d->get_Lxx();
    kkt_.block(ix, ndx_ + iu, ndxi, nui) = d->get_Lxu();
    kkt_.block(ndx_ + iu, ix, nui, ndxi) = d->get_Lxu().transpose();
    kkt_.block(ndx_ + iu, ndx_ + iu, nui, nui) = d->get_Luu();
    // jacobian
    kkt_.block(ndx_ + nu_ + cx0 + ix, ix, ndxi, ndxi) = -d->get_Fx();
    kkt_.block(ndx_ + nu_ + cx0 + ix, ndx_ + iu, ndxi, nui) = -d->get_Fu();
    kktref_.segment(ix, ndxi) = d->get_Lx();
    kktref_.segment(ndx_ + iu, nui) = d->get_Lu();
    // constraint value = x_guess - x_ref = diff(x_ref,x_guess)
    m->get_state().diff(d->get_xnext(), xs_[t + 1], kktref_.segment(ndx_ + nu_ + cx0 + ix, ndxi));
    ix += ndxi;
    iu += nui;
  }
  boost::shared_ptr<ActionDataAbstract>& df = problem_.terminal_data_;
  unsigned int const& ndxf = problem_.terminal_model_->get_state().get_ndx();
  kkt_.block(ix, ix, ndxf, ndxf) = df->get_Lxx();
  kktref_.segment(ix, ndxf) = df->get_Lx();
  kkt_.block(0, ndx_+ nu_, ndx_ + nu_, ndx_).noalias() = kkt_.block(ndx_+ nu_, 0, ndx_, ndx_ + nu_).transpose();
  if (!std::isnan(xreg_)) {
    kkt_.block(0, 0, ndx_, ndx_).diagonal().array() += xreg_;
  }
  if (!std::isnan(ureg_)) {
    kkt_.block(ndx_, ndx_, nu_, nu_).diagonal().array() += ureg_;
  }
  return cost_;
}

void SolverKKT::computePrimalDual() {
  // Cholesky decomposition and positive definiteness check
  // 
  kkt_llt_.compute(kkt_);
  primaldual_ = -kktref_;
  kkt_llt_.solveInPlace(primaldual_);
  primal_ = primaldual_.segment(0, ndx_ + nu_);
  dual_ = primaldual_.segment(ndx_ + nu_, ndx_);

}

void SolverKKT::computeDirection(const bool& recalc) {
  unsigned int const& T = problem_.get_T();
  if (recalc) {
    calc();
  }
  computePrimalDual();
  const Eigen::VectorBlock<Eigen::VectorXd, Eigen::Dynamic> p_x = primal_.segment(0, ndx_);
  const Eigen::VectorBlock<Eigen::VectorXd, Eigen::Dynamic> p_u = primal_.segment(ndx_, nu_);

  int ix = 0;
  int iu = 0;

  for (unsigned int t = 0; t < T; ++t) {
    unsigned int const& ndxi = problem_.running_models_[t]->get_state().get_ndx();
    unsigned int const& nui = problem_.running_models_[t]->get_nu();
    dxs_[t] = p_x.segment(ix, ndxi);
    dus_[t] = p_u.segment(iu, nui);
    lambdas_[t] = dual_.segment(ix, ndxi);
    ix += ndxi;
    iu += nui;
  }
  const unsigned int ndxi = problem_.terminal_model_->get_state().get_ndx();
  dxs_.back() = p_x.segment(ix, ndxi);
  lambdas_.back() = dual_.segment(ix, ndxi);
}

const Eigen::Vector2d& SolverKKT::expectedImprovement() {
  d_ = Eigen::Vector2d::Zero();
  // -grad^T.primal
  d_(0) = -kktref_.segment(0, ndx_ + nu_).dot(primal_);
  // -(hessian.primal)^T.primal
  kkt_primal_.noalias() = kkt_.block(0, 0, ndx_ + nu_, ndx_ + nu_) * primal_;
  d_(1) = - kkt_primal_.dot(primal_);
  return d_;
}

double SolverKKT::stoppingCriteria() {
  stop_ = 0.;
  unsigned int const& T = problem_.get_T();
  Eigen::VectorXd dL = kktref_.segment(0, ndx_ + nu_);
  Eigen::VectorXd dF;
  dF.resize(ndx_ + nu_);
  dF.setZero();
  int ix = 0;
  int iu = 0;

  for (unsigned int t = 0; t < T; ++t) {
    boost::shared_ptr<ActionDataAbstract>& d = problem_.running_datas_[t];
    unsigned int const& ndxi = problem_.running_models_[t]->get_state().get_ndx();
    unsigned int const& nui = problem_.running_models_[t]->get_nu();
    dF.segment(ix, ndxi).noalias() = lambdas_[t] - d->get_Fx() * lambdas_[t + 1];
    dF.segment(ndx_ + iu, nui).noalias() = -d->get_Fu() * lambdas_[t + 1];
    ix += ndxi;
    iu += nui;
  }
  unsigned int const& ndxi = problem_.terminal_model_->get_state().get_ndx();
  dF.segment(ix, ndxi) = lambdas_.back();
  stop_ = (dL + dF).squaredNorm() + kktref_.segment(ndx_ + nu_, ndx_).squaredNorm();

  return stop_;
}

double SolverKKT::tryStep(const double& steplength) {
  unsigned int const& T = problem_.get_T();
  for (unsigned int t = 0; t < T; ++t) {
    ActionModelAbstract* m = problem_.running_models_[t];
    m->get_state().integrate(xs_[t], steplength * dxs_[t], xs_try_[t + 1]);
    us_try_[t] = us_[t] + steplength * dus_[t];
  }
  cost_try_ = problem_.calc(xs_try_, us_try_);
  return cost_ - cost_try_;
}

bool SolverKKT::solve(const std::vector<Eigen::VectorXd>& init_xs, const std::vector<Eigen::VectorXd>& init_us,
                      const unsigned int& maxiter, const bool& is_feasible, const double& reginit) {
  setCandidate(init_xs, init_us, is_feasible);
  if (std::isnan(reginit)) {
    xreg_ = 0.;
    ureg_ = 0.;
  } else {
    xreg_ = reginit;
    ureg_ = reginit;
  }

  for (iter_ = 0; iter_ < maxiter; ++iter_) {
    bool recalc = true;
    while (true) {
      try {
        computeDirection(recalc);
      } catch (const char* msg) {
        recalc = false;
        if (xreg_ == regmax_) {
          return false;
        } else {
          continue;
        }
      }
      break;
    }
    //
    expectedImprovement();
    //
    for (std::vector<double>::const_iterator it = alphas_.begin(); it != alphas_.end(); ++it) {
      steplength_ = *it;

      try {
        dV_ = tryStep(steplength_);
      } catch (const char* msg) {
        continue;
      }
      dVexp_ = steplength_ * (d_[0] + 0.5 * steplength_ * d_[1]);

      if (d_[0] < th_grad_ || !is_feasible_ || dV_ > th_acceptstep_ * dVexp_) {
        was_feasible_ = is_feasible_;
        setCandidate(xs_try_, us_try_, true);
        cost_ = cost_try_;
        break;
      }
    }

    if (steplength_ > th_step_) {
      decreaseRegularization();
    }
    if (steplength_ == alphas_.back()) {
      increaseRegularization();
      if (xreg_ == regmax_) {
        return false;
      }
    }
    stoppingCriteria();

    const long unsigned int& n_callbacks = callbacks_.size();
    if (n_callbacks != 0) {
      for (long unsigned int c = 0; c < n_callbacks; ++c) {
        CallbackAbstract& callback = *callbacks_[c];
        callback(*this);
      }
    }

    if (was_feasible_ && stop_ < th_stop_) {
      return true;
    }
  }
  return false;
}

void SolverKKT::increaseRegularization() {
  xreg_ *= regfactor_;
  if (xreg_ > regmax_) {
    xreg_ = regmax_;
  }
  ureg_ = xreg_;
}

void SolverKKT::decreaseRegularization() {
  xreg_ /= regfactor_;
  if (xreg_ < regmin_) {
    xreg_ = regmin_;
  }
  ureg_ = xreg_;
}

void SolverKKT::allocateData() {
  unsigned int const& T = problem_.get_T();
  //
  dxs_.resize(T + 1);
  dus_.resize(T);
  lambdas_.resize(T + 1);
  //
  xs_try_.resize(T + 1);
  us_try_.resize(T);
  nx_ = 0;
  ndx_ = 0;
  nu_ = 0;

  for (unsigned int t = 0; t < T; ++t) {
    ActionModelAbstract* model = problem_.running_models_[t];
    const int& nx = model->get_state().get_nx();
    const int& ndx = model->get_state().get_ndx();
    const int& nu = model->get_nu();

    if (t == 0) {
      xs_try_[t] = problem_.get_x0();
    } else {
      xs_try_[t] = Eigen::VectorXd::Constant(nx, NAN);
    }
    us_try_[t] = Eigen::VectorXd::Constant(nu, NAN);
    dxs_[t] = Eigen::VectorXd::Zero(ndx);
    dus_[t] = Eigen::VectorXd::Zero(nu);
    lambdas_[t] = Eigen::VectorXd::Zero(ndx);

    nx_ += nx;
    ndx_ += ndx;
    nu_ += nu;
  }
  // add terminal model
  ActionModelAbstract* model = problem_.get_terminalModel();
  nx_ += model->get_state().get_nx();
  ndx_ += model->get_state().get_ndx();
  xs_try_.back() = problem_.terminal_model_->get_state().zero();
  dxs_.back() = Eigen::VectorXd::Zero(model->get_state().get_ndx());
  lambdas_.back() = Eigen::VectorXd::Zero(model->get_state().get_ndx());
  // set dimensions for kkt matrix and kkt_ref vector
  kkt_.resize(2 * ndx_ + nu_, 2 * ndx_ + nu_);
  kkt_.setZero();
  kktref_.resize(2 * ndx_ + nu_);
  kktref_.setZero();
  //
  primaldual_.resize(2 * ndx_ + nu_);
  primaldual_.setZero();
  primal_.resize(ndx_ + nu_);
  primal_.setZero();
  kkt_primal_.resize(ndx_ + nu_);
  kkt_primal_.setZero();
  dual_.resize(ndx_);
  dual_.setZero();
  kkt_llt_ = Eigen::LLT<Eigen::MatrixXd>(2 * ndx_ + nu_);
}

}  // namespace crocoddyl