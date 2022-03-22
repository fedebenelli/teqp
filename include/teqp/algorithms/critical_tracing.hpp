#pragma once

#include <fstream>
#include <optional>

#include "nlohmann/json.hpp"

#include <Eigen/Dense>
#include "teqp/algorithms/rootfinding.hpp"

// Imports from boost
#include <boost/numeric/odeint/stepper/controlled_runge_kutta.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta_cash_karp54.hpp>
#include <boost/numeric/odeint/stepper/euler.hpp>

namespace teqp {
// This has to be outside the critical tracing struct so that the pybind11 wrapper doesn't fight with the types
struct TCABOptions {
    double abs_err = 1.0e-6,
        rel_err = 1.0e-6,
        init_dt = 10, ///< The initial step size
        max_dt = 10000000000,
        T_tol = 1e-6, ///< The tolerance on temperature to indicate that it is converged
        init_c = 1.0; ///< The c parameter which controls the initial search direction for the first step. Choices are 1 or -1
    int small_T_count = 5; ///< How many small temperature steps indicates convergence
    int integration_order = 5; ///< The order of integration, either 1 for simple Euler or 5 for adaptive RK45
    int max_step_count = 1000; ///< Maximum number of steps allowed
    int skip_dircheck_count = 1; ///< Only start checking the direction dot product after this many steps
    bool polish = false; ///< If true, polish the solution at every step
};

template<typename Model, typename Scalar = double, typename VecType = Eigen::ArrayXd>
struct CriticalTracing {
    /***
    * \brief Simple wrapper to sort the eigenvalues(and associated eigenvectors) in increasing order
    * \param H The matrix, in this case, the Hessian matrix of Psi w.r.t.the molar concentrations
    * \param values The eigenvalues
    * \returns vectors The eigenvectors, as columns
    *
    * See https://stackoverflow.com/a/56327853
    *
    * \note The input Matrix is symmetric, thus the SelfAdjointEigenSolver can be used, and returned eigenvalues
    * will be real and sorted already with corresponding eigenvectors as columns
    */
    static auto sorted_eigen(const Eigen::MatrixXd& H) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(H);
        return std::make_tuple(es.eigenvalues(), es.eigenvectors());
    }

    struct EigenData {
        Eigen::ArrayXd v0, v1, eigenvalues;
        Eigen::MatrixXd eigenvectorscols;
    };

    static auto eigen_problem(const Model& model, const Scalar T, const VecType& rhovec, const VecType& alignment_v0 = {}) {

        EigenData ed;

        auto N = rhovec.size();
        Eigen::ArrayX<bool> mask = (rhovec != 0).eval();

        using id = IsochoricDerivatives<decltype(model)>;

        // Build the Hessian for the residual part;
#if defined(USE_AUTODIFF)
        auto H = id::build_Psir_Hessian_autodiff(model, T, rhovec);
#else
        auto H = id::build_Psir_Hessian_mcx(model, T, rhovec);
#endif
        // ... and add ideal-gas terms to H
        for (auto i = 0; i < N; ++i) {
            if (mask[i]) {
                H(i, i) += model.R(rhovec/rhovec.sum()) * T / rhovec[i];
            }
        }

        int nonzero_count = mask.count();
        auto zero_count = N - nonzero_count;

        if (zero_count == 0) {
            // Not an infinitely dilute mixture, nothing special
            std::tie(ed.eigenvalues, ed.eigenvectorscols) = sorted_eigen(H);

            // Align with the eigenvector of the component with the smallest density, and make that one positive
            Eigen::Index ind;
            rhovec.minCoeff(&ind);
            if (ed.eigenvectorscols.col(ind).minCoeff() < 0) {
                ed.eigenvectorscols *= -1.0;
            }
        }
        else if (zero_count == 1) {
            // Extract Hessian matrix without entries where rho is exactly zero
            std::vector<int> indicesToKeep;

            int badindex = -1;
            for (auto i = 0; i < N; ++i) {
                if (mask[i]) {
                    indicesToKeep.push_back(i);
                }
                else {
                    badindex = i;
                }
            }
            Eigen::MatrixXd Hprime = H(indicesToKeep, indicesToKeep);

            auto [eigenvalues, eigenvectors] = sorted_eigen(Hprime);

            // Inject values into the U^T and v0 vectors
            //
            // Make a padded matrix for U (with eigenvectors as rows)
            Eigen::MatrixXd U = H; U.setZero();

            // Fill in the associated elements corresponding to eigenvectors 
            for (auto i = 0; i < N - nonzero_count; ++i) {
                U(i, indicesToKeep) = eigenvectors.col(i); // Put in the row, leaving a hole for the zero value
            }

            // The last row has a 1 in the column corresponding to the pure fluid entry
            // We insist that there must be only one non-zero entry
            U.row(U.rows() - 1)(badindex) = 1.0;

            ed.eigenvalues = eigenvalues;
            ed.eigenvectorscols = U.transpose();
        }
        else {
            throw std::invalid_argument("More than one non-zero concentration value found; not currently supported");
        }
        if (alignment_v0.size() > 0 && ed.eigenvectorscols.col(0).matrix().dot(alignment_v0.matrix()) < 0) {
            ed.eigenvectorscols.col(0) *= -1;
        }
        
        ed.v0 = ed.eigenvectorscols.col(0);
        ed.v1 = ed.eigenvectorscols.col(1);
        return ed;
    }

    struct psi1derivs {
        Eigen::ArrayXd psir, psi0, tot;
        EigenData ei;
    };

    static auto get_minimum_eigenvalue_Psi_Hessian(const Model& model, const Scalar T, const VecType& rhovec) {
        return eigen_problem(model, T, rhovec).eigenvalues[0];
    }

    static auto get_derivs(const Model& model, const Scalar T, const VecType& rhovec, const VecType& alignment_v0 = {}) {
        auto molefrac = rhovec / rhovec.sum();
        auto R = model.R(molefrac);

        // Solve the complete eigenvalue problem
        auto ei = eigen_problem(model, T, rhovec, alignment_v0);

        // Ideal-gas contributions of psi0 w.r.t. sigma_1, in the same form as the residual part
        Eigen::ArrayXd psi0_derivs(5); psi0_derivs.setZero();
        psi0_derivs[0] = -1; // Placeholder, not needed
        psi0_derivs[1] = -1; // Placeholder, not needed
        for (auto i = 0; i < rhovec.size(); ++i) {
            if (rhovec[i] != 0) {
                psi0_derivs[2] += R * T * pow(ei.v0[i], 2) / rhovec[i];  // second derivative
                psi0_derivs[3] += -R * T * pow(ei.v0[i], 3) / pow(rhovec[i], 2); // third derivative
                psi0_derivs[4] += 2 * R * T * pow(ei.v0[i], 4) / pow(rhovec[i], 3); // fourth derivative
            }
        }

#if defined(USE_AUTODIFF)
        // Calculate the first through fourth derivative of Psi^r w.r.t. sigma_1
        ArrayXdual4th v0(ei.v0.size()); for (auto i = 0; i < ei.v0.size(); ++i) { v0[i] = ei.v0[i]; }
        ArrayXdual4th rhovecad(rhovec.size());  for (auto i = 0; i < rhovec.size(); ++i) { rhovecad[i] = rhovec[i]; }
        dual4th varsigma{ 0.0 };
        auto wrapper = [&rhovecad, &v0, &T, &model](const auto& sigma_1) {
            auto rhovecused = (rhovecad + sigma_1 * v0).eval();
            auto rhotot = rhovecused.sum();
            auto molefrac = (rhovecused / rhotot).eval();
            return eval(model.alphar(T, rhotot, molefrac) * model.R(molefrac) * T * rhotot);
        };
        auto psir_derivs_ = derivatives(wrapper, wrt(varsigma), at(varsigma));
        VecType psir_derivs; psir_derivs.resize(5);
        for (auto i = 0; i < 5; ++i) { psir_derivs[i] = psir_derivs_[i]; }

#else
        using namespace mcx;
        // Calculate the first through fourth derivative of Psi^r w.r.t. sigma_1
        Eigen::Vector<MultiComplex<double>, Eigen::Dynamic> v0(ei.v0.size()); for (auto i = 0; i < ei.v0.size(); ++i) { v0[i] = ei.v0[i]; }
        Eigen::Vector<MultiComplex<double>, Eigen::Dynamic> rhovecmcx(rhovec.size());  for (auto i = 0; i < rhovec.size(); ++i) { rhovecmcx[i] = rhovec[i]; }
        using fcn_t = std::function<MultiComplex<double>(const MultiComplex<double>&)>;
        fcn_t wrapper = [&rhovecmcx, &v0, &T, &model](const MultiComplex<double>& sigma_1) {
            Eigen::Vector<MultiComplex<double>, Eigen::Dynamic> rhovecused = rhovecmcx + sigma_1 * v0;
            auto rhotot = rhovecused.sum();
            auto molefrac = rhovecused / rhotot;
            return model.alphar(T, rhotot, molefrac) * model.R(molefrac) * T * rhotot;
        };
        auto psir_derivs_ = diff_mcx1(wrapper, 0.0, 4, true);
        VecType psir_derivs; psir_derivs.resize(5);
        for (auto i = 0; i < 5; ++i) { psir_derivs[i] = psir_derivs_[i]; }
#endif

        // As a sanity check, the minimum eigenvalue of the Hessian constructed based on the molar concentrations
        // must match the second derivative of psi_tot w.r.t. sigma_1. This is not always satisfied for derivatives
        // with Cauchy method
        //if (abs(np.min(ei.eigvals) - psitot_derivs[2]) > 1e-3){
        //    print(np.min(ei.eigvals), psitot_derivs[2], rhovec)
        //}

        psi1derivs psi1;
        psi1.psir = psir_derivs;
        psi1.psi0 = psi0_derivs;
        psi1.tot = psi0_derivs + psir_derivs;
        psi1.ei = ei;
        return psi1;
    }

    template <typename Iterable>
    static bool all(const Iterable& foo) {
        return std::all_of(std::begin(foo), std::end(foo), [](const auto x) { return x; });
    }
    template <typename Iterable>
    static bool any(const Iterable& foo) {
        return std::any_of(std::begin(foo), std::end(foo), [](const auto x) { return x; });
    }

    static auto get_drhovec_dT_crit(const Model& model, const Scalar T, const VecType& rhovec) {

        // The derivatives of total Psi w.r.t.sigma_1 (numerical for residual, analytic for ideal)
        // Returns a tuple, with residual, ideal, total dicts with of number of derivatives, value of derivative
        auto all_derivs = get_derivs(model, T, rhovec);
        auto derivs = all_derivs.tot;

        // The temperature derivative of total Psi w.r.t.T from a centered finite difference in T
        auto dT = 1e-7;
        auto plusT = get_derivs(model, T + dT, rhovec).tot;
        auto minusT = get_derivs(model, T - dT, rhovec).tot;
        auto derivT = (plusT - minusT) / (2.0 * dT);

        // Solve the eigenvalue problem for the given T & rho
        auto ei = all_derivs.ei;

        auto sigma2 = 2e-5 * rhovec.sum(); // This is the perturbation along the second eigenvector

        auto rhovec_plus = (rhovec + ei.v1 * sigma2).eval();
        auto rhovec_minus = (rhovec - ei.v1 * sigma2).eval();
        std::string stepping_desc = "";
        auto deriv_sigma2 = all_derivs.tot;
        auto eval = [](const auto& ex) { return ex.eval(); };
        if (all(eval(rhovec_minus > 0)) && all(eval(rhovec_plus > 0))) {
            // Conventional centered derivative
            auto plus_sigma2 = get_derivs(model, T, rhovec_plus, ei.v0);
            auto minus_sigma2 = get_derivs(model, T, rhovec_minus, ei.v0);
            deriv_sigma2 = (plus_sigma2.tot - minus_sigma2.tot) / (2.0 * sigma2);
            stepping_desc = "conventional centered";
        }
        else if (all(eval(rhovec_plus > 0))) {
            // Forward derivative in the direction of v1
            auto plus_sigma2 = get_derivs(model, T, rhovec_plus, ei.v0);
            auto rhovec_2plus = (rhovec + 2 * ei.v1 * sigma2).eval();
            auto plus2_sigma2 = get_derivs(model, T, rhovec_2plus, ei.v0);
            deriv_sigma2 = (-3 * derivs + 4 * plus_sigma2.tot - plus2_sigma2.tot) / (2.0 * sigma2);
            stepping_desc = "forward";
        }
        else if (all(eval(rhovec_minus > 0))) {
            // Negative derivative in the direction of v1
            auto minus_sigma2 = get_derivs(model, T, rhovec_minus, ei.v0);
            auto rhovec_2minus = (rhovec - 2 * ei.v1 * sigma2).eval();
            auto minus2_sigma2 = get_derivs(model, T, rhovec_2minus, ei.v0);
            deriv_sigma2 = (-3 * derivs + 4 * minus_sigma2.tot - minus2_sigma2.tot) / (-2.0 * sigma2);
            stepping_desc = "backwards";
        }
        else {
            throw std::invalid_argument("This is not possible I think.");
        }

        // The columns of b are from Eq. 31 and Eq. 33
        Eigen::MatrixXd b(2, 2);
        b << derivs[3], derivs[4],             // row is d^3\Psi/d\sigma_1^3, d^4\Psi/d\sigma_1^4
            deriv_sigma2[2], deriv_sigma2[3]; // row is d/d\sigma_2(d^3\Psi/d\sigma_1^3), d/d\sigma_2(d^3\Psi/d\sigma_1^3)

        auto LHS = (ei.eigenvectorscols * b).transpose();
        Eigen::MatrixXd RHS(2, 1); RHS << -derivT[2], -derivT[3];
        Eigen::MatrixXd drhovec_dT = LHS.colPivHouseholderQr().solve(RHS);

#if defined(DEBUG_get_drhovec_dT_crit)
        std::cout << "LHS: " << LHS << std::endl;
        std::cout << "RHS: " << RHS << std::endl;
        std::cout << "b: " << b << std::endl;
        std::cout << "stepping_desc: " << stepping_desc << std::endl;
        std::cout << "deriv_sigma2: " << deriv_sigma2 << std::endl;
        std::cout << "rhovec_plus: " << rhovec_plus << std::endl;
        std::cout << "all_derivs.tot:" << all_derivs.tot << std::endl;
        std::cout << "all_derivs.psir:" << all_derivs.psir << std::endl; 
        auto plus_sigma2 = get_derivs(model, T, rhovec_plus, ei.v0);
        std::cout << "plus_sigma2.tot:" << plus_sigma2.tot << std::endl;
        std::cout << "plus_sigma2.psir:" << plus_sigma2.psir << std::endl;
        std::cout << "dot of v0: " << plus_sigma2.ei.v0 * ei.v0 << std::endl;
#endif
        return drhovec_dT;
    }

    static auto get_criticality_conditions(const Model& model, const Scalar T, const VecType& rhovec) {
        auto derivs = get_derivs(model, T, rhovec);
        return (Eigen::ArrayXd(2) << derivs.tot[2], derivs.tot[3]).finished();
    }

    /// Polish a critical point while keeping the overall composition constant and iterating for temperature and overall density
    static auto critical_polish_molefrac(const Model& model, const Scalar T, const VecType& rhovec, const Scalar z0) {
        auto polish_x_resid = [&model, &z0](const auto& x) {
            auto T = x[0];
            Eigen::ArrayXd rhovec(2); rhovec << z0*x[1], (1-z0)*x[1];
            //auto z0new = rhovec[0] / rhovec.sum();
            auto derivs = get_derivs(model, T, rhovec);
            // First two are residuals on critical point
            return (Eigen::ArrayXd(2) << derivs.tot[2], derivs.tot[3]).finished();
        };
        Eigen::ArrayXd x0(2); x0 << T, rhovec[0]+rhovec[1];
        auto r0 = polish_x_resid(x0);
        auto x = NewtonRaphson(polish_x_resid, x0, 1e-10);
        auto r = polish_x_resid(x);
        Eigen::ArrayXd change = x0 - x;
        if (!std::isfinite(x[0]) || !std::isfinite(x[1])) {
            throw std::invalid_argument("Something not finite; aborting polishing");
        }
        Eigen::ArrayXd rhovecsoln(2); rhovecsoln << x[1]*z0, x[1] * (1 - z0);
        return std::make_tuple(x[0], rhovecsoln);
    }
    static auto critical_polish_fixedrho(const Model& model, const Scalar T, const VecType& rhovec, const int i) {
        Scalar rhoval = rhovec[i];
        auto polish_x_resid = [&model, &i, &rhoval](const auto& x) {
            auto T = x[0];
            Eigen::ArrayXd rhovec(2); rhovec << x[1], x[2];
            //auto z0new = rhovec[0] / rhovec.sum();
            auto derivs = get_derivs(model, T, rhovec);
            // First two are residuals on critical point, third is residual on the molar concentration to be held constant
            return (Eigen::ArrayXd(3) << derivs.tot[2], derivs.tot[3], rhovec[i] - rhoval).finished();
        };
        Eigen::ArrayXd x0(3); x0 << T, rhovec[0], rhovec[1];
        auto r0 = polish_x_resid(x0);
        auto x = NewtonRaphson(polish_x_resid, x0, 1e-10);
        auto r = polish_x_resid(x);
        Eigen::ArrayXd change = x0 - x;
        if (!std::isfinite(T) || !std::isfinite(x[1]) || !std::isfinite(x[2])) {
            throw std::invalid_argument("Something not finite; aborting polishing");
        }
        Eigen::ArrayXd rho = x.tail(x.size() - 1);
        return std::make_tuple(x[0], rho);
    }
    static auto critical_polish_fixedT(const Model& model, const Scalar T, const VecType& rhovec) {
        auto polish_T_resid = [&model, &T](const auto& x) {
            auto derivs = get_derivs(model, T, x);
            return (Eigen::ArrayXd(2) << derivs.tot[2], derivs.tot[3]).finished();
        };
        Eigen::ArrayXd x0 = rhovec;
        auto r0 = polish_T_resid(x0);
        auto x = NewtonRaphson(polish_T_resid, x0, 1e-10);
        auto r = polish_T_resid(x);
        Eigen::ArrayXd change = x0 - x;
        if (!std::isfinite(T) || !std::isfinite(x[1])) {
            throw std::invalid_argument("Something not finite; aborting polishing");
        }
        return x;
    }

    static auto trace_critical_arclength_binary(const Model& model, const Scalar& T0, const VecType& rhovec0, const std::optional<std::string>& filename_ = std::nullopt, const std::optional<TCABOptions> &options_ = std::nullopt) -> nlohmann::json {
        std::string filename = filename_.value_or("");
        TCABOptions options = options_.value_or(TCABOptions{});

        VecType last_drhodt;

        // Typedefs for the types for odeint for simple Euler and RK45 integrators
        using state_type = std::vector<double>; 
        using namespace boost::numeric::odeint;
        euler<state_type> eul; // Class for simple Euler integration
        typedef runge_kutta_cash_karp54< state_type > error_stepper_type;
        typedef controlled_runge_kutta< error_stepper_type > controlled_stepper_type;

        auto dot = [](const auto& v1, const auto& v2) { return (v1 * v2).sum(); };
        auto norm = [](const auto& v) { return sqrt((v * v).sum()); };

        auto JSONdata = nlohmann::json::array();
        std::ofstream ofs = (filename.empty()) ? std::ofstream() : std::ofstream(filename);
        
        double c = options.init_c; 

        // The function for the derivative in the form of odeint
        // x is [T, rhovec]
        auto xprime = [&](const state_type& x, state_type& dxdt, const double /* t */)
        {
            // Unpack the inputs
            const double T = x[0];
            const auto rhovec = Eigen::Map<const Eigen::ArrayXd>(&(x[0]) + 1, x.size() - 1);
            
            auto drhodT = get_drhovec_dT_crit(model, T, rhovec).array().eval();
            auto dTdt = 1.0 / norm(drhodT);
            Eigen::ArrayXd drhodt = c * (drhodT * dTdt).eval();

            dxdt[0] = c*dTdt;

            auto drhodtview = Eigen::Map<Eigen::ArrayXd>(&(dxdt[0]) + 1, dxdt.size() - 1); 
            drhodtview = drhodt; // Copy values into the view

            if (last_drhodt.size() > 0) {
                auto rhodot = dot(drhodt, last_drhodt);
                if (rhodot < 0) {
                    Eigen::Map<Eigen::ArrayXd>(&(dxdt[0]), dxdt.size()) *= -1;
                }
            }
        };
        auto get_dxdt = [&](const state_type& x) {
            auto dxdt = state_type(x.size());
            xprime(x, dxdt, -1);
            return dxdt;
        };
        // Pull out the drhodt from dxdt, just a convenience function
        auto extract_drhodt = [](const state_type& dxdt) -> Eigen::ArrayXd {
            return Eigen::Map<const Eigen::ArrayXd>(&(dxdt[0]) + 1, dxdt.size() - 1);
        };

        // Define the tolerances
        double abs_err = options.abs_err, rel_err = options.rel_err, a_x = 1.0, a_dxdt = 1.0;
        controlled_stepper_type controlled_stepper(default_error_checker< double, range_algebra, default_operations >(abs_err, rel_err, a_x, a_dxdt));

        double t = 0, dt = options.init_dt;

        // Build the initial state array, with T followed by rhovec
        std::vector<double> x0(rhovec0.size() + 1); 
        x0[0] = T0;
        Eigen::Map<Eigen::ArrayXd>(&(x0[0]) + 1, x0.size() - 1) = rhovec0;

        // Make variables T and rhovec references to the contents of x0 vector
        // The views are mutable (danger!)
        double& T = x0[0];
        auto rhovec = Eigen::Map<Eigen::ArrayXd>(&(x0[0]) + 1, x0.size() - 1);

        auto store_drhodt = [&](const state_type& x0) {
            last_drhodt = extract_drhodt(get_dxdt(x0));
        };

        auto store_point = [&]() {

            // Calculate some other parameters, for debugging, or scientific interest
            auto rhotot = rhovec.sum();
            using id = IsochoricDerivatives<decltype(model), Scalar, VecType>;
            double p = rhotot * model.R(rhovec / rhovec.sum()) * T + id::get_pr(model, T, rhovec);
            auto conditions = get_criticality_conditions(model, T, rhovec);
            double splus = id::get_splus(model, T, rhovec);
            auto dxdt = x0;
            xprime(x0, dxdt, -1.0);

            // Store the data in a JSON structure
            nlohmann::json point = {
                {"t", t},
                {"T / K", T},
                {"rho0 / mol/m^3", static_cast<double>(rhovec[0])},
                {"rho1 / mol/m^3", static_cast<double>(rhovec[1])},
                {"c", c},
                {"s^+", splus},
                {"p / Pa", p},
                {"dT/dt", dxdt[0]},
                {"drho0/dt", dxdt[1]},
                {"drho1/dt", dxdt[2]},
                {"lambda1", conditions[0]},
                {"dirderiv(lambda1)/dalpha", conditions[1]},
            };
            JSONdata.push_back(point);
        };

        // Line writer
        auto write_line = [&]() {
            std::stringstream out;
            auto rhotot = rhovec.sum();
            double z0 = rhovec[0] / rhotot;
            using id = IsochoricDerivatives<decltype(model)>;
            auto conditions = get_criticality_conditions(model, T, rhovec);
            out << z0 << "," << rhovec[0] << "," << rhovec[1] << "," << T << "," << rhotot * model.R(rhovec / rhovec.sum()) * T + id::get_pr(model, T, rhovec) << "," << c << "," << dt << "," << conditions(0) << "," << conditions(1) << std::endl;
            std::string sout(out.str());
            std::cout << sout;
            if (ofs.is_open()) {
                ofs << sout;
            }
        };
        
        int counter_T_converged = 0, retry_count = 0;
        ofs << "z0 / mole frac.,rho0 / mol/m^3,rho1 / mol/m^3,T / K,p / Pa,c,dt,condition(1),condition(2)" << std::endl;
        
        // Determine the initial direction of integration
        {
            const auto step = (rhovec + extract_drhodt(get_dxdt(x0))*dt).eval();
            Eigen::ArrayX<bool> negativestepvals = (step < 0).eval();
            // Flip the sign if the first step would yield any negative concentrations
            if (negativestepvals.any()) {
                c *= -1;
            }
        }
        //store_drhodt(x0);
        if (!filename.empty()) {
            write_line();
        }

        for (auto iter = 0; iter < options.max_step_count; ++iter) {
            
            // Calculate the derivatives at the beginning of the step
            auto dxdt_start_step = get_dxdt(x0);
            auto x_start_step = x0;

            if (iter == 0 && retry_count == 0) { 
                store_point(); }
            
            if (options.integration_order == 5) {
                auto res = controlled_step_result::fail;
                try {
                    res = controlled_stepper.try_step(xprime, x0, t, dt);
                }
                catch (const std::exception &e) {
                    std::cout << e.what() << std::endl;
                    break;
                }

                if (res != controlled_step_result::success) {
                    // Try again, with a smaller step size
                    iter--;
                    retry_count++;
                    continue;
                }
                else {
                    retry_count = 0;
                    /*auto dxdt = x0;
                    xprime(x0, dxdt, -1.0);
                    auto drhodt = Eigen::Map<Eigen::ArrayXd>(&(dxdt[0]) + 1, dxdt.size() - 1);
                    auto rhodotproduct = dot(drhodt, last_drhodt);
                    int rr = 0;*/
                }
                // Reduce step size if greater than the specified max step size
                dt = std::min(dt, options.max_dt);
            }
            else if (options.integration_order == 1) {
                try {
                    eul.do_step(xprime, x0, t, dt);
                    t += dt;
                }
                catch (const std::exception &e) {
                    std::cout << e.what() << std::endl;
                    break;
                }
            }
            else {
                throw std::invalid_argument("integration order is invalid:" + std::to_string(options.integration_order));
            }

            auto z0 = rhovec[0] / rhovec.sum();
            if (z0 < 0 || z0 > 1) {
                break;
            }
            
            if (options.polish) {
                try {
                    int i = 0;
                    //auto [Tnew, rhovecnew] = critical_polish_fixedrho(model, T, rhovec, i);
                    auto [Tnew, rhovecnew] = critical_polish_molefrac(model, T, rhovec, z0);
                    T = Tnew; rhovec = rhovecnew;
                }
                catch (std::exception& e) {
                    //int i = 0;
                    //auto [Tnew, rhovecnew] = critical_polish_fixedrho(model, T, rhovec, i);
                    std::cout << e.what() << std::endl;
                }
            }

            // Store the derivative vector from the beginning of the step, before
            // the actual step is taken.  We don't want to use the values at the end
            // because otherwise simple Euler will never consider the possible change in direction
            //
            // Also, we only do this after two completed steps because sometimes the infinite
            // dilution derivatives seem to be not quite right. There is still a risk that the first
            // step will try to turn around...
            if (iter >= options.skip_dircheck_count) {
                store_drhodt(x_start_step); }

            auto actualstep = (Eigen::Map<Eigen::ArrayXd>(&(x0[0]), x0.size()) - Eigen::Map<Eigen::ArrayXd>(&(x_start_step[0]), x_start_step.size())).eval();
            
            // Check if T has stopped changing
            if (std::abs(actualstep[0]) < options.T_tol) {
                counter_T_converged++;
            }
            else {
                counter_T_converged = 0;
            }

            auto rhotot = rhovec.sum();
            z0 = rhovec[0] / rhotot;
            if (z0 < 0 || z0 > 1) {
                break;
            }

            if (!filename.empty()) { write_line(); }
            store_point();

            if (counter_T_converged > options.small_T_count) {
                break;
            }
        }
        //auto N = JSONdata.size();
        return JSONdata;
    }

}; // namespace VecType

}; // namespace teqp