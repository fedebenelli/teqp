#pragma once

#include "teqp/models/fwd.hpp"
#include "teqp/models/mie/lennardjones.hpp"

#include "teqp/exceptions.hpp"

#include "nlohmann/json.hpp"

namespace teqp {

    inline AllowedModels build_model(const nlohmann::json& json) {
        
        auto build_square_matrix = [](const std::valarray<std::valarray<double>>& m){
            // First assume that the matrix is square, resize
            Eigen::ArrayXXd mat(m.size(), m.size());
            if (m.size() == 0){
                return mat;
            }
            // Then copy elements over
            for (auto i = 0; i < m.size(); ++i){
                auto row = m[i];
                if (row.size() != mat.rows()){
                    throw std::invalid_argument("provided matrix is not square");
                }
                for (auto j = 0; j < row.size(); ++j){
                    mat(i, j) = row[j];
                }
            }
            return mat;
        };

        // Extract the name of the model and the model parameters
        std::string kind = json.at("kind");
        auto spec = json.at("model");

        if (kind == "vdW1") {
            return vdWEOS1(spec.at("a"), spec.at("b"));
        }
        else if (kind == "vdW") {
            return vdWEOS<double>(spec.at("Tcrit / K"), spec.at("pcrit / Pa"));
        }
        else if (kind == "PR") {
            std::valarray<double> Tc_K = spec.at("Tcrit / K"), pc_Pa = spec.at("pcrit / Pa"), acentric = spec.at("acentric");
            Eigen::ArrayXXd kmat(0, 0);
            if (spec.contains("kmat")){
                kmat = build_square_matrix(spec["kmat"]);
            }
            return canonical_PR(Tc_K, pc_Pa, acentric, kmat);
        }
        else if (kind == "SRK") {
            std::valarray<double> Tc_K = spec.at("Tcrit / K"), pc_Pa = spec.at("pcrit / Pa"), acentric = spec.at("acentric");
            Eigen::ArrayXXd kmat(0, 0);
            if (spec.contains("kmat")){
                kmat = build_square_matrix(spec["kmat"]);
            }
            return canonical_SRK(Tc_K, pc_Pa, acentric, kmat);
        }
        else if (kind == "CPA") {
            return CPA::CPAfactory(spec);
        }
        else if (kind == "PCSAFT") {
            using namespace PCSAFT;
            Eigen::ArrayXXd kmat(0, 0);
            if (spec.contains("kmat")){
                kmat = build_square_matrix(spec["kmat"]);
            }
            
            if (spec.contains("names")){
                return PCSAFTMixture(spec["names"], kmat);
            }
            else if (spec.contains("coeffs")){
                std::vector<SAFTCoeffs> coeffs;
                for (auto j : spec["coeffs"]) {
                    SAFTCoeffs c;
                    c.name = j.at("name");
                    c.m = j.at("m");
                    c.sigma_Angstrom = j.at("sigma_Angstrom");
                    c.epsilon_over_k = j.at("epsilon_over_k");
                    c.BibTeXKey = j.at("BibTeXKey");
                    coeffs.push_back(c);
                }
                return PCSAFTMixture(coeffs, kmat);
            }
            else{
                throw std::invalid_argument("you must provide names or coeffs, but not both");
            }
        }
        else if (kind == "multifluid") {
            return multifluidfactory(spec);
        }
        else if (kind == "SW_EspindolaHeredia2009"){
            return squarewell::EspindolaHeredia2009(spec.at("lambda"));
        }
        else if (kind == "EXP6_Kataoka1992"){
            return exp6::Kataoka1992(spec.at("alpha"));
        }
        else if (kind == "AmmoniaWaterTillnerRoth"){
            return AmmoniaWaterTillnerRoth();
        }
        else if (kind == "LJ126_TholJPCRD2016"){
            return build_LJ126_TholJPCRD2016();
        }
        else if (kind == "IdealHelmholtz"){
            return IdealHelmholtz(spec);
        }
        else {
            throw teqpcException(30, "Unknown kind:" + kind);
        }
    }

};
