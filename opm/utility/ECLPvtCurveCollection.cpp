/*
  Copyright 2017 Statoil ASA.

  This file is part of the Open Porous Media Project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <opm/utility/ECLPvtCurveCollection.hpp>

#include <opm/utility/ECLPropertyUnitConversion.hpp>
#include <opm/utility/ECLResultData.hpp>

#include <cassert>
#include <initializer_list>
#include <vector>

#include <ert/ecl/ecl_kw_magic.h>

namespace {
    std::vector<int>
    pvtnumVector(const ::Opm::ECLGraph&        G,
                 const ::Opm::ECLInitFileData& init)
    {
        auto pvtnum = G.rawLinearisedCellData<int>(init, "PVTNUM");

        if (pvtnum.empty()) {
            // PVTNUM missing in one or more of the grids managed by 'G'.
            // Put all cells in PVTNUM region 1.
            pvtnum.assign(G.numCells(), 1);
        }

        return pvtnum;
    }

    std::vector<Opm::FlowDiagnostics::Graph> emptyFDGraph()
    {
        return { Opm::FlowDiagnostics::Graph{} };
    }

    template <class PVTInterp>
    std::vector<Opm::FlowDiagnostics::Graph>
    rawPvtCurve(const PVTInterp*            pvt,
                const Opm::ECLPVT::RawCurve curve,
                const int                   regID)
    {
        if (pvt == nullptr) {
            // Result set does not provide requisite tabulated properties.
            // Return empty.
            return emptyFDGraph();
        }

        return pvt->getPvtCurve(curve, regID);
    }

    template <class PVTInterp, class Pressure, class MixRatio>
    std::vector<double>
    formationVolumeFactor(const PVTInterp* pvt,
                          const int        regID,
                          const Pressure&  press,
                          const MixRatio&  R)
    {
        assert (pvt != nullptr);

        return pvt->formationVolumeFactor(regID, R, press);
    }

    template <class PVTInterp, class Pressure, class MixRatio>
    std::vector<double>
    viscosity(const PVTInterp* pvt,
              const int        regID,
              const Pressure&  press,
              const MixRatio&  R)
    {
        assert (pvt != nullptr);

        return pvt->viscosity(regID, R, press);
    }

    std::vector<double>
    gasProperty(const Opm::ECLPVT::Gas*     pvt,
                const Opm::ECLPVT::RawCurve property,
                const int                   regID,
                const std::vector<double>&  Pg,
                const std::vector<double>&  Rv)
    {
        if (pvt == nullptr) {
            // No such property interpolant.  Return empty.
            return {};
        }

        assert ((property == Opm::ECLPVT::RawCurve::FVF) ||
                (property == Opm::ECLPVT::RawCurve::Viscosity));

        const auto pg = Opm::ECLPVT::Gas::GasPressure { Pg };
        auto       rv = Opm::ECLPVT::Gas::VaporizedOil{ Rv };
        if (rv.data.empty()) {
            rv.data.assign(pg.data.size(), 0.0);
        }

        if (property == Opm::ECLPVT::RawCurve::FVF) {
            return formationVolumeFactor(pvt, regID, pg, rv);
        }

        return viscosity(pvt, regID, pg, rv);
    }

    std::vector<double>
    oilProperty(const Opm::ECLPVT::Oil*     pvt,
                const Opm::ECLPVT::RawCurve property,
                const int                   regID,
                const std::vector<double>&  Po,
                const std::vector<double>&  Rs)
    {
        if (pvt == nullptr) {
            // No such property interpolant.  Return empty.
            return {};
        }

        assert ((property == Opm::ECLPVT::RawCurve::FVF) ||
                (property == Opm::ECLPVT::RawCurve::Viscosity));

        const auto po = Opm::ECLPVT::Oil::OilPressure { Po };
        auto       rs = Opm::ECLPVT::Oil::DissolvedGas{ Rs };
        if (rs.data.empty()) {
            rs.data.assign(po.data.size(), 0.0);
        }

        if (property == Opm::ECLPVT::RawCurve::FVF) {
            return formationVolumeFactor(pvt, regID, po, rs);
        }

        return viscosity(pvt, regID, po, rs);
    }

    std::vector<Opm::FlowDiagnostics::Graph>
    convertCurve(std::vector<Opm::FlowDiagnostics::Graph>&&      curves,
                 const Opm::ECLUnits::Convert::PhysicalQuantity& cvrt_x,
                 const Opm::ECLUnits::Convert::PhysicalQuantity& cvrt_y)
    {
        for (auto& curve : curves) {
            cvrt_x.appliedTo(curve.first);
            cvrt_y.appliedTo(curve.second);
        }

        return curves;
    }

    std::vector<Opm::FlowDiagnostics::Graph>
    convertFvfCurve(std::vector<Opm::FlowDiagnostics::Graph>&& curve,
                    const Opm::ECLPhaseIndex                   phase,
                    const Opm::ECLUnits::UnitSystem&           usysFrom,
                    const Opm::ECLUnits::UnitSystem&           usysTo)
    {
        assert ((phase == Opm::ECLPhaseIndex::Liquid) ||
                (phase == Opm::ECLPhaseIndex::Vapour));

        if (phase == Opm::ECLPhaseIndex::Liquid) {
             // Oil FVF.  First column is pressure, second column is Bo.
            const auto& cvrt_x = Opm::ECLUnits::Convert::Pressure()
                .from(usysFrom).to(usysTo);

            const auto& cvrt_y = Opm::ECLUnits::Convert::OilFVF()
                .from(usysFrom).to(usysTo);

            return convertCurve(std::move(curve), cvrt_x, cvrt_y);
        }

        // Gas FVF.  Need to distinguish miscible from immiscible cases.  In
        // the former, the first column is Rv (vapourised oil/gas ratio) and
        // in the second case the first column is the gas pressure.
        //
        // Immiscible case identified by curve.size() <= 1.

        const auto& cvrt_y = Opm::ECLUnits::Convert::GasFVF()
            .from(usysFrom).to(usysTo);

        if (curve.size() <= 1) {
            // Immiscible Gas FVF.  First column is Pg.
            const auto& cvrt_x = Opm::ECLUnits::Convert::Pressure()
                .from(usysFrom).to(usysTo);

            return convertCurve(std::move(curve), cvrt_x, cvrt_y);
        }

        // Miscible Gas FVF.  First column is Rv.
        const auto& cvrt_x = Opm::ECLUnits::Convert::VaporisedOilGasRatio()
            .from(usysFrom).to(usysTo);

        return convertCurve(std::move(curve), cvrt_x, cvrt_y);
    }

    std::vector<Opm::FlowDiagnostics::Graph>
    convertViscosityCurve(std::vector<Opm::FlowDiagnostics::Graph>&& curve,
                          const Opm::ECLPhaseIndex                   phase,
                          const Opm::ECLUnits::UnitSystem&           usysFrom,
                          const Opm::ECLUnits::UnitSystem&           usysTo)
    {
        assert ((phase == Opm::ECLPhaseIndex::Liquid) ||
                (phase == Opm::ECLPhaseIndex::Vapour));

        // This is the viscosity curve.  Second column is always viscosity
        // irrespective of phase or miscible/immiscible fluids.
        const auto& cvrt_y = Opm::ECLUnits::Convert::Viscosity()
            .from(usysFrom).to(usysTo);

        if ((phase == Opm::ECLPhaseIndex::Liquid) || (curve.size() <= 1)) {
             // Graph is oil viscosity or immiscible gas viscosity.  First
             // column is pressure.
            const auto& cvrt_x = Opm::ECLUnits::Convert::Pressure()
                .from(usysFrom).to(usysTo);

            return convertCurve(std::move(curve), cvrt_x, cvrt_y);
        }

        // Miscible Gas viscosity.  First column is Rv (vapourised oil/gas
        // ratio).
        const auto& cvrt_x = Opm::ECLUnits::Convert::VaporisedOilGasRatio()
            .from(usysFrom).to(usysTo);

        return convertCurve(std::move(curve), cvrt_x, cvrt_y);
    }

    std::vector<Opm::FlowDiagnostics::Graph>
    convertSatStateCurve(std::vector<Opm::FlowDiagnostics::Graph>&& curve,
                         const Opm::ECLPhaseIndex                   phase,
                         const Opm::ECLUnits::UnitSystem&           usysFrom,
                         const Opm::ECLUnits::UnitSystem&           usysTo)
    {
        assert ((phase == Opm::ECLPhaseIndex::Liquid) ||
                (phase == Opm::ECLPhaseIndex::Vapour));

        // First column is pressure (Po or Pg).
        const auto& cvrt_x = Opm::ECLUnits::Convert::Pressure()
            .from(usysFrom).to(usysTo);

        // Second column is Rs or Rv depending on 'phase'.
        if (phase == Opm::ECLPhaseIndex::Liquid) {
            // Saturated state curve for miscible oil.  Second column is Rs
            // (dissolved gas/oil ratio).
            const auto& cvrt_y = Opm::ECLUnits::Convert::
                DissolvedGasOilRatio().from(usysFrom).to(usysTo);

            return convertCurve(std::move(curve), cvrt_x, cvrt_y);
        }

        // Saturated state curve for miscible gas.  Second column is Rv
        // (vapourised oil/gas ratio).
        const auto& cvrt_y = Opm::ECLUnits::Convert::
            VaporisedOilGasRatio().from(usysFrom).to(usysTo);

        return convertCurve(std::move(curve), cvrt_x, cvrt_y);
    }
}

Opm::ECLPVT::ECLPvtCurveCollection::
ECLPvtCurveCollection(const ECLGraph&        G,
                      const ECLInitFileData& init)
    : pvtnum_       (pvtnumVector(G, init))
    , gas_          (CreateGasPVTInterpolant::fromECLOutput(init))
    , oil_          (CreateOilPVTInterpolant::fromECLOutput(init))
    , usys_native_  (ECLUnits::serialisedUnitConventions(init))
    , usys_internal_(ECLUnits::internalUnitConventions())
{}

void
Opm::ECLPVT::ECLPvtCurveCollection::
setOutputUnits(std::unique_ptr<const ECLUnits::UnitSystem> usys)
{
    this->usys_output_ = std::move(usys);
}

std::vector<Opm::FlowDiagnostics::Graph>
Opm::ECLPVT::ECLPvtCurveCollection::
getPvtCurve(const RawCurve      curve,
            const ECLPhaseIndex phase,
            const int           activeCell) const
{
    if (! this->isValidRequest(phase, activeCell)) {
        // Not a supported phase or cell index out of bounds.  Not a valid
        // request so return empty.
        return emptyFDGraph();
    }

    // PVTNUM is traditional one-based region identifier.  Subtract one to
    // form valid index into std::vector<>s.
    const auto regID = this->pvtnum_[activeCell] - 1;

    if (phase == ECLPhaseIndex::Liquid) {
        // Caller requests oil properties.
        return this->convertToOutputUnits(
            rawPvtCurve(this->oil_.get(), curve, regID), curve, phase);
    }

    // Caller requests gas properties.
    assert ((phase == ECLPhaseIndex::Vapour) &&
            "Internal Logic Error Identifying Supported Phases");

    return this->convertToOutputUnits(
        rawPvtCurve(this->gas_.get(), curve, regID), curve, phase);
}

std::vector<double>
Opm::ECLPVT::ECLPvtCurveCollection::
getDynamicPropertySI(const RawCurve             property,
                     const ECLPhaseIndex        phase,
                     const int                  activeCell,
                     const std::vector<double>& phasePress,
                     const std::vector<double>& mixRatio) const
{
    if (! this->isValidRequest(phase, activeCell) ||
        (property == RawCurve::SaturatedState))
    {
        // Not a supported phase, cell index out of bounds, or caller
        // requests dynamically evaluating the saturated state curve.  Not a
        // valid request so return empty.
        return {};
    }

    // PVTNUM is traditional one-based region identifier.  Subtract one to
    // form valid index into std::vector<>s.
    const auto regID = this->pvtnum_[activeCell] - 1;

    if (phase == ECLPhaseIndex::Liquid) {
        // Caller requests oil properties.
        return oilProperty(this->oil_.get(), property,
                           regID, phasePress, mixRatio);
    }

    // Caller requests gas properties.
    assert ((phase == ECLPhaseIndex::Vapour) &&
            "Internal Logic Error Identifying Supported Phases");

    return gasProperty(this->gas_.get(), property,
                       regID, phasePress, mixRatio);
}

std::vector<double>
Opm::ECLPVT::ECLPvtCurveCollection::
getDynamicPropertyNative(const RawCurve      property,
                         const ECLPhaseIndex phase,
                         const int           activeCell,
                         std::vector<double> phasePress,
                         std::vector<double> mixRatio) const
{
    if (! this->isValidRequest(phase, activeCell) ||
        (property == RawCurve::SaturatedState))
    {
        // Not a supported phase, cell index out of bounds, or caller
        // requests dynamically evaluating the saturated state curve.  Not a
        // valid request so return empty.
        return {};
    }

    assert (this->usys_native_   != nullptr);
    assert (this->usys_internal_ != nullptr);

    // 1) Convert inputs from native to internal (SI) units of measurement.
    ::Opm::ECLUnits::Convert::Pressure()
          .from(*this->usys_native_)
          .to  (*this->usys_internal_).appliedTo(phasePress);

    if (phase == ECLPhaseIndex::Liquid) {
        ::Opm::ECLUnits::Convert::DissolvedGasOilRatio()
              .from(*this->usys_native_)
              .to  (*this->usys_internal_).appliedTo(mixRatio);
    }
    else {
        assert (phase == ECLPhaseIndex::Vapour);

        ::Opm::ECLUnits::Convert::VaporisedOilGasRatio()
              .from(*this->usys_native_)
              .to  (*this->usys_internal_).appliedTo(mixRatio);
    }

    // 2) Evaluate requested property in strict SI units.
    auto prop = this->getDynamicPropertySI(property, phase, activeCell,
                                           phasePress, mixRatio);

    // 3) Convert property values to user's requested system of units.

    if (this->usys_output_ == nullptr) {
        // No user-defined system of units for outputs.  Use PropertySI()
        // directly.
        return prop;
    }

    // Caller has defined a particular system of units for outputs.  Convert
    // 'prop' accordingly.
    if (property == RawCurve::Viscosity) {
        // The 'prop' values represent viscosities.
        ::Opm::ECLUnits::Convert::Viscosity()
            .from(*this->usys_internal_)
            .to  (*this->usys_output_).appliedTo(prop);
    }
    else {
        assert (property == RawCurve::FVF);

        if (phase == ECLPhaseIndex::Vapour) {
            // The 'prop' values represent Bg.
            ::Opm::ECLUnits::Convert::GasFVF()
                .from(*this->usys_internal_)
                .to  (*this->usys_output_).appliedTo(prop);
        }
        else {
            assert (phase == ECLPhaseIndex::Liquid);

            ::Opm::ECLUnits::Convert::OilFVF()
                .from(*this->usys_internal_)
                .to  (*this->usys_output_).appliedTo(prop);
        }
    }

    return prop;
}

bool
Opm::ECLPVT::ECLPvtCurveCollection::
isValidRequest(const ECLPhaseIndex phase,
               const int           activeCell) const
{
    if (! ((phase == ECLPhaseIndex::Liquid) ||
           (phase == ECLPhaseIndex::Vapour)))
    {
        // We support "liquid" and "vapour" phase (oil/gas) properties only.
        return false;
    }

    // Check if cell index is within bounds.
    return static_cast<decltype(this->pvtnum_.size())>(activeCell)
        <  this->pvtnum_.size();
}

std::vector<Opm::FlowDiagnostics::Graph>
Opm::ECLPVT::ECLPvtCurveCollection::
convertToOutputUnits(std::vector<FlowDiagnostics::Graph>&& graph,
                     const RawCurve                        curve,
                     const ECLPhaseIndex                   phase) const
{
    if (this->usys_output_ == nullptr) {
        // No defined system of units for outputs.  Return unconverted (SI).
        return graph;
    }

    assert ((phase == ECLPhaseIndex::Liquid) ||
            (phase == ECLPhaseIndex::Vapour));

    if (curve == RawCurve::FVF) {
        return convertFvfCurve(std::move(graph), phase,
                               *this->usys_internal_, // from
                               *this->usys_output_);  // to
    }

    if (curve == RawCurve::Viscosity) {
        return convertViscosityCurve(std::move(graph), phase,
                                     *this->usys_internal_, // from
                                     *this->usys_output_);  // to
    }

    if (curve == RawCurve::SaturatedState) {
        return convertSatStateCurve(std::move(graph), phase,
                                    *this->usys_internal_, // from
                                    *this->usys_output_);  // to
    }

    throw std::invalid_argument { "Internal Logic Error" };
}
