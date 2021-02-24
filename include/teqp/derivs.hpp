#pragma once

#include <optional>
#include <complex>
#include <tuple>

#include "MultiComplex/MultiComplex.hpp"

template <typename TType, typename ContainerType, typename FuncType>
typename std::enable_if<is_container<ContainerType>::value, typename ContainerType::value_type>::type
caller(const FuncType& f, TType T, const ContainerType& rho) {
    return f(T, rho);
}

/***
* \brief Given a function, use complex step derivatives to calculate the derivative with 
* respect to the first variable which here is temperature
*/
template <typename TType, typename ContainerType, typename FuncType>
typename ContainerType::value_type derivT(const FuncType& f, TType T, const ContainerType& rho) {
    double h = 1e-100;
    return f(std::complex<TType>(T, h), rho).imag() / h;
}

/***
* \brief Given a function, use multicomplex derivatives to calculate the derivative with
* respect to the first variable which here is temperature
*/
template <typename TType, typename ContainerType, typename FuncType>
typename ContainerType::value_type derivTmcx(const FuncType& f, TType T, const ContainerType& rho) {
    using fcn_t = std::function<MultiComplex<double>(const MultiComplex<double>&)>;
    fcn_t wrapper = [&rho, &f](const MultiComplex<TType>& T_) {return f(T_, rho); };
    auto ders = diff_mcx1(wrapper, T, 1);
    return ders[0];
}

/***
* \brief Given a function, use complex step derivatives to calculate the derivative with respect 
* to the given composition variable
*/
template <typename TType, typename ContainerType, typename FuncType, typename Integer>
typename ContainerType::value_type derivrhoi(const FuncType& f, TType T, const ContainerType& rho, Integer i) {
    double h = 1e-100;
    using comtype = std::complex<ContainerType::value_type>;
    std::valarray<comtype> rhocom(rho.size());
    for (auto j = 0; j < rho.size(); ++j) {
        rhocom[j] = comtype(rho[j], 0.0);
    }
    rhocom[i] = comtype(rho[i], h);
    return f(T, rhocom).imag() / h;
}

/***
* \brief Calculate the Psir=ar*rho
*/
template <typename TType, typename ContainerType, typename Model>
typename ContainerType::value_type get_Psir(const Model& model, const TType T, const ContainerType& rhovec) {
    using container = decltype(rhovec);
    auto rhotot_ = std::accumulate(std::begin(rhovec), std::end(rhovec), (decltype(rhovec[0]))0.0);
    return model.alphar(T, rhovec)*model.R*T*rhotot_;
}

/***
* \brief Calculate the residual pressure from derivatives of alphar
*/
template <typename Model, typename TType, typename ContainerType>
typename ContainerType::value_type get_pr(const Model& model, const TType T, const ContainerType& rhovec)
{
    auto rhotot_ = std::accumulate(std::begin(rhovec), std::end(rhovec), (decltype(rhovec[0]))0.0);
    decltype(rhovec[0] * T) pr = 0.0;
    for (auto i = 0; i < rhovec.size(); ++i) {
        pr += rhovec[i]*derivrhoi([&model](const auto& T, const auto& rhovec){ return model.alphar(T, rhovec); }, T, rhovec, i);
    }
    return pr*rhotot_*model.R*T;
}

template <typename Model, typename TType, typename ContainerType>
typename ContainerType::value_type get_Ar10(const Model& model, const TType T, const ContainerType& rhovec){
    return T*derivT([&model](const auto& T, const auto& rhovec) { return model.alphar(T, rhovec); }, T, rhovec);
}

/***
* \brief Calculate the residual entropy (s^+ = -sr/R) from derivatives of alphar
*/
template <typename Model, typename TType, typename ContainerType>
typename ContainerType::value_type get_splus(const Model& model, const TType T, const ContainerType& rhovec){
    return model.alphar(T, rhovec) - get_Ar10(model, T, rhovec);
}

/***
* \brief Calculate the Hessian of Psir = ar*rho w.r.t. the molar concentrations
* 
* Requires the use of multicomplex derivatives to calculate second partial derivatives
*/
template<typename Model, typename TType, typename RhoType>
auto build_Psir_Hessian(const Model& model, const TType T, const RhoType& rho) {
    // Double derivatives in each component's concentration
    // N^N matrix (symmetric)
    for (auto i = 0; i < rho.size(); ++i) {
        for (auto j = i; j < rho.size(); ++j) {
            auto val = diff_mcxN();
            H(i,j) = val;
            H(j,i) = val;
        }
    }
    return H;
}