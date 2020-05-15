
#ifndef TSHIRT_H
#define TSHIRT_H

#include "schaake_partitioning.hpp"
#include "Constants.h"
#include "Nonlinear_Reservoir.hpp"
#include "GIUH.hpp"
#include "Pdm03.h"
#include <cmath>
#include <utility>
#include <vector>

namespace tshirt {

    //! Tshirt parameters struct
    /*!
        This structure provides storage for the parameters of the Tshirt hydrological model
    */

    struct tshirt_params {
        double maxsmc;              //!< saturated soil moisture content (sometimes theta_e or smcmax)
        double wltsmc;              //!< wilting point soil moisture content
        double satdk;               //!< vertical saturated hydraulic conductivity [m s^-1] (sometimes Kperc or Ks)
        double satpsi;              //!< saturated capillary head [m]
        // TODO: explain more what this is
        double slope;               //!< SLOPE parameter
        double b;                   //!< 'b' exponent on Clapp-Hornberger soil water relations (sometime bexp)
        double multiplier;          //!< the multiplier applied to 'satdk' to route water rapidly downslope in subsurface (sometimes 'mult' or 'LKSATFAC')
        double alpha_fc;            //!< alpha constant for given soil type for relative suction head value, with respect to Hatm
        double Klf;                 //!< lateral flow independent calibration parameter
        double Kn;                  //!< Nash cascade linear reservoir coefficient lateral flow parameter
        int nash_n;                 //!< number of nash cascades
        double Cgw;                 //!< Ground water flow param
        double Cschaake;            //!< The Schaake adjusted magic constant by soil type
        double expon;               //!< Ground water flow exponent param (analogous to NWM 2.0 expon param)
        double max_soil_storage_meters;  //!< Subsurface soil water flow max storage param ("Ssmax"), calculated from maxsmc and depth
        double max_groundwater_storage_meters;    //!< Ground water flow max storage param ("Sgwmax"; analogous to NWM 2.0 zmax param)
        double max_lateral_flow;    //!< Max rate for subsurface lateral flow (i.e., max transmissivity)
        const double depth = 2.0;         //!< Total soil column depth ('D') [m]

        //! Constructor for tshirt parameters
        /*!
            Constructor for tshirt param objects.
        */
        tshirt_params(double maxsmc, double wltsmc, double satdk, double satpsi, double slope, double b,
                      double multiplier, double alpha_fc, double Klf, double Kn, int nash_n, double Cgw, double expon,
                      double max_gw_storage) :
                maxsmc(maxsmc),
                wltsmc(wltsmc),
                satdk(satdk),
                satpsi(satpsi),
                slope(slope),
                b(b),
                multiplier(multiplier),
                alpha_fc(alpha_fc),
                Klf(Klf),
                Kn(Kn),
                nash_n(nash_n),
                Cgw(Cgw),
                expon(expon),
                max_groundwater_storage_meters(max_gw_storage) {
            this->max_soil_storage_meters = this->depth * maxsmc;
            this->Cschaake = 3.0 * satdk / (2.0e-6);
            this->max_lateral_flow = satdk * multiplier * this->max_soil_storage_meters;
        }

    };

    /*!
     * Tshirt state structure
     *
     * This structure provides storage for the state used by Tshirt hydrological model at a particular time step.
     */
    struct tshirt_state {
        // TODO: confirm this is correct
        double soil_storage_meters;              //!< current water storage in soil column nonlinear reservoir ("Ss")
        double groundwater_storage_meters;       //!< current water storage in ground water nonlinear reservoir ("Sgw")
        double* nash_cascade_storeage_meters;    //!< water storage in nonlinear reservoirs of Nash Cascade for lateral subsurface flow

        // I think this doesn't belong in state, and so is just in run() below
        //double column_total_soil_moisture_deficit;    //!< soil column total moisture deficit

        tshirt_state(double soil_storage_meters, double groundwater_storage_meters,
                     double* nash_cascade_storeage_meters = nullptr)
                : soil_storage_meters(soil_storage_meters),
                  groundwater_storage_meters(groundwater_storage_meters),
                  nash_cascade_storeage_meters(nash_cascade_storeage_meters) {}
    };

    /*!
     * Tshirt flux structure
     *
     * This structure provides storage for the fluxes generated by Tshirt at any time step
     */
    struct tshirt_fluxes {
        double surface_runoff_meters_per_second;  //!< Direct surface runoff, in meters per second
        double groundwater_flow_meters_per_second;         //!< Deep groundwater flow from groundwater reservoir to channel flow
        double soil_percolation_flow_meters_per_second;    //!< Percolation flow from subsurface to groundwater reservoir ("Qperc")
        double soil_lateral_flow_meters_per_second;        //!< Lateral subsurface flow ("Qlf")
        double et_loss_meters;         //!< Loss from ET, in meters

        tshirt_fluxes(double q_gw, double q_perc, double q_lf, double runoff, double et_loss)
                : groundwater_flow_meters_per_second(q_gw),
                  soil_percolation_flow_meters_per_second(q_perc),
                  soil_lateral_flow_meters_per_second(q_lf),
                  surface_runoff_meters_per_second(runoff),
                  et_loss_meters(et_loss) {

        }
    };

    // TODO: consider combining with or differentiating from similar hymod enum
    enum TshirtErrorCodes {
        TSHIRT_NO_ERROR = 0,
        TSHIRT_MASS_BALANCE_ERROR = 100
    };

    /**
     * Tshirt model class.
     *
     * A less static, more OO implementation of the Tshirt hydrological model.
     */
    class tshirt_model {

    public:

        tshirt_model(tshirt_params model_params,
                     const shared_ptr<tshirt_state>& initial_state) : model_params(model_params),
                                                                      previous_state(initial_state),
                                                                      current_state(initial_state) {
            // This will get used a few times ...
            double Sfc = calc_soil_field_capacity_storage();

            // Create the vector of Nash Cascade reservoirs used at the end of the soil lateral flow outlet
            soil_lf_nash_res.resize(model_params.nash_n);
            // TODO: verify correctness of activation_threshold (Sfc) and max_velocity (max_lateral_flow) arg values
            for (unsigned long i = 0; i < soil_lf_nash_res.size(); ++i) {
                //construct a single outlet nonlinear reservoir
                soil_lf_nash_res[i] = make_unique<Nonlinear_Reservoir>(
                        Nonlinear_Reservoir(0.0, model_params.max_soil_storage_meters,
                                            previous_state->nash_cascade_storeage_meters[i], model_params.Kn, 1.0,
                                            Sfc, model_params.max_lateral_flow));
            }

            // Create the soil reservoir
            // TODO: probably will need to change this to be vector of pointers once Reservoir class changes are done
            vector<Reservoir_Outlet> soil_res_outlets(2);

            // init subsurface later flow outlet
            soil_res_outlets[lf_outlet_index] = Reservoir_Outlet(model_params.Klf, 1.0, Sfc,
                                                                 model_params.max_lateral_flow);
            // init subsurface percolation flow outlet
            // The max perc flow should be equal to the params.satdk value
            soil_res_outlets[perc_outlet_index] = Reservoir_Outlet(model_params.satdk * model_params.slope, 1.0, Sfc,
                                                                   model_params.satdk);

            soil_reservoir = Nonlinear_Reservoir(0.0, model_params.depth, previous_state->soil_storage_meters, soil_res_outlets);

            // Create the groundwater reservoir
            // Given the equation:
            //      double groundwater_flow_meters_per_second = params.Cgw * ( exp(params.expon * state.groundwater_storage_meters / params.max_groundwater_storage_meters) - 1 );
            // The max value should be when groundwater_storage_meters == max_groundwater_storage_meters, or ...
            double max_gw_velocity = model_params.Cgw * (exp(model_params.expon) - 1);
            // TODO: verify activation threshold
            groundwater_reservoir = Nonlinear_Reservoir(0, model_params.max_groundwater_storage_meters,
                                                        previous_state->groundwater_storage_meters, model_params.Cgw, 1,
                                                        0, max_gw_velocity);
            fluxes = nullptr;

        }

        tshirt_model(tshirt_params model_params) :
            tshirt_model(model_params, make_shared<tshirt_state>(tshirt_state(0.0, 0.0))) {}

        /**
         * Calculate losses due to evapotranspiration.
         *
         * @param soil_m
         * @param et_params
         * @return
         */
        double calc_evapotranspiration(double soil_m, pdm03_struct *et_params);

        /**
         * Calculate soil field capacity storage, the level at which free drainage stops (i.e., "Sfc").
         *
         * @return
         */
        double calc_soil_field_capacity_storage();

        /**
         * Run the model to one time step, moving the initial `current_state` value to `previous_state` and resetting
         * other members applicable only to in the context of the current time step so that they are recalculated.
         *
         * @param dt the time step
         * @param input_flux_meters the amount water entering the system this time step, in meters
         * @param et_params ET parameters struct
         * @return
         */
        int run(double dt, double input_flux_meters, pdm03_struct *et_params);

    private:
        /** Model state for the "current" time step, which may not be calculated yet. */
        shared_ptr<tshirt_state> current_state;
        /** Model execution parameters. */
        tshirt_params model_params;
        /** Model state from that previous time step before the current. */
        shared_ptr<tshirt_state> previous_state;
        /**
         * A collection of reservoirs for a Nash Cascade at the end of the lateral flow output from the subsurface soil
         * reservoir.
         */
        vector<unique_ptr<Nonlinear_Reservoir>> soil_lf_nash_res;
        /** The index of the subsurface lateral flow outlet in the soil reservoir. */
        int lf_outlet_index = 0;
        /** The index of the percolation flow outlet in the soil reservoir. */
        int perc_outlet_index = 1;
        Nonlinear_Reservoir soil_reservoir;
        Nonlinear_Reservoir groundwater_reservoir;
        shared_ptr<tshirt_fluxes> fluxes;

    };

    /*!
     * Tshirt kernel class
     *
     * This class implements the Tshirt hydrological model.
     */
    class tshirt_kernel {
    public:

        /**
         * Calculate losses due to evapotranspiration.
         *
         * @param soil_m
         * @param et_params
         * @return
         */
        static double calc_evapotranspiration(double soil_m, void *et_params) {
            pdm03_struct *pdm = (pdm03_struct *) et_params;
            pdm->XHuz = soil_m;
            pdm03_wrapper(pdm);

            return pdm->XHuz - soil_m;
        }

        /**
         * Calculate soil field capacity storage, the level at which free drainage stops (i.e., "Sfc").
         *
         * @param params
         * @param state
         * @return
         */
        static double calc_soil_field_capacity_storage(const tshirt_params &params, const tshirt_state &state) {
            // Calculate the suction head above water table (Hwt)
            double head_above_water_table = params.alpha_fc * (ATMOSPHERIC_PRESSURE_PASCALS / WATER_SPECIFIC_WEIGHT);
            // TODO: account for possibility of Hwt being less than 0.5 (though initially, it looks like this will never be the case)

            double z1 = head_above_water_table - 0.5;
            double z2 = z1 + 2;

            // Note that z^( 1 - (1/b) ) / (1 - (1/b)) == b * (z^( (b - 1) / b ) / (b - 1)
            return params.maxsmc * pow((1.0 / params.satpsi), (-1.0 / params.b)) *
                   ((params.b * pow(z2, ((params.b - 1) / params.b)) / (params.b - 1)) -
                    (params.b * pow(z1, ((params.b - 1) / params.b)) / (params.b - 1)));
        }

        static void init_nash_cascade_vector(vector<Nonlinear_Reservoir> &reservoirs, const tshirt_params &params,
                                             const tshirt_state &state, const double activation,
                                             const double max_flow_velocity) {
            reservoirs.resize(params.nash_n);
            for (unsigned long i = 0; i < reservoirs.size(); ++i) {
                //construct a single outlet nonlinear reservoir
                reservoirs[i] = Nonlinear_Reservoir(0, params.max_soil_storage_meters,
                                                    state.nash_cascade_storeage_meters[i], params.Kn, 1, activation,
                                                    max_flow_velocity);
            }
        }

        //! run one time step of tshirt
        static int run(
                double dt,
                tshirt_params params,        //!< static parameters for tshirt
                tshirt_state state,          //!< model state
                tshirt_state &new_state,     //!< model state struct to hold new model state
                tshirt_fluxes &fluxes,       //!< model flux object to hold calculated fluxes
                double input_flux_meters,          //!< the amount water entering the system this time step
                // TODO: should/can this be a smart pointer?
                giuh::giuh_kernel *giuh_obj,       //!< kernel object for calculating GIUH runoff from subsurface lateral flow
                void *et_params)            //!< parameters for the et function
        {
            double column_total_soil_moisture_deficit = params.max_soil_storage_meters - state.soil_storage_meters;

            // Note this surface runoff value has not yet performed GIUH calculations
            double surface_runoff, subsurface_infiltration_flux;

            Schaake_partitioning_scheme(dt, params.Cschaake, column_total_soil_moisture_deficit, input_flux_meters,
                                        &surface_runoff, &subsurface_infiltration_flux);

            double Sfc = calc_soil_field_capacity_storage(params, state);

            vector<Reservoir_Outlet> subsurface_outlets;

            // Keep track of the indexes of the specific outlets for later access
            int lf_outlet_index = 0;
            int perc_outlet_index = 1;

            // init subsurface later flow outlet
            subsurface_outlets[lf_outlet_index] = Reservoir_Outlet(params.Klf, 1.0, Sfc, params.max_lateral_flow);
            // init subsurface percolation flow outlet
            // The max perc flow should be equal to the params.satdk value
            subsurface_outlets[perc_outlet_index] = Reservoir_Outlet(params.satdk * params.slope, 1.0, Sfc,
                                                                     params.satdk);

            Nonlinear_Reservoir subsurface_reservoir(0.0, params.depth, state.soil_storage_meters, subsurface_outlets);

            double subsurface_excess;
            subsurface_reservoir.response_meters_per_second(subsurface_infiltration_flux, dt, subsurface_excess);

            // lateral subsurface flow
            double Qlf = subsurface_reservoir.velocity_meters_per_second_for_outlet(lf_outlet_index);

            // percolation flow
            double Qperc = subsurface_reservoir.velocity_meters_per_second_for_outlet(perc_outlet_index);

            // TODO: make sure ET doesn't need to be taken out sooner
            double new_soil_storage = subsurface_reservoir.get_storage_height_meters();
            fluxes.et_loss_meters = calc_evapotranspiration(new_soil_storage, et_params);
            new_state.soil_storage_meters = new_soil_storage - fluxes.et_loss_meters;

            // initialize the Nash cascade of nonlinear reservoirs
            std::vector<Nonlinear_Reservoir> nash_cascade;
            // TODO: verify correctness of activation_threshold (Sfc) and max_velocity (max_lateral_flow) arg values
            init_nash_cascade_vector(nash_cascade, params, state, Sfc, params.max_lateral_flow);

            // cycle through lateral flow Nash cascade of nonlinear reservoirs
            // loop essentially copied from Hymod logic, but with different variable names
            for (unsigned long int i = 0; i < nash_cascade.size(); ++i) {
                // get response water velocity of nonlinear reservoir
                Qlf = nash_cascade[i].response_meters_per_second(Qlf, dt, subsurface_excess);
                // TODO: confirm this is correct
                Qlf += subsurface_excess / dt;
            }

            // "raw" GW calculations
            //state.groundwater_storage_meters += soil_percolation_flow_meters_per_second * dt;
            //double groundwater_flow_meters_per_second = params.Cgw * ( exp(params.expon * state.groundwater_storage_meters / params.max_groundwater_storage_meters) - 1 );

            // Given the equation:
            //      double groundwater_flow_meters_per_second = params.Cgw * ( exp(params.expon * state.groundwater_storage_meters / params.max_groundwater_storage_meters) - 1 );
            // The max value should be when groundwater_storage_meters == max_groundwater_storage_meters, or ...
            double max_gw_velocity = params.Cgw * (exp(params.expon) - 1);
            // TODO: verify activation threshold
            Nonlinear_Reservoir groundwater_res(0, params.max_groundwater_storage_meters,
                                                state.groundwater_storage_meters, params.Cgw, 1, 0, max_gw_velocity);
            // TODO: what needs to be done with this value?
            double excess_gw_water;
            fluxes.groundwater_flow_meters_per_second = groundwater_res.response_meters_per_second(Qperc, dt,
                                                                                                   excess_gw_water);
            // update state
            new_state.groundwater_storage_meters = groundwater_res.get_storage_height_meters();

            // record other fluxes
            fluxes.soil_lateral_flow_meters_per_second = Qlf;
            fluxes.soil_percolation_flow_meters_per_second = Qperc;
            // Calculate GIUH surface runoff
            fluxes.surface_runoff_meters_per_second = giuh_obj->calc_giuh_output(dt, surface_runoff);

            return 0;
        }

        static int mass_check(
                const tshirt_params &params,
                const tshirt_state &current_state,
                double input_flux_meters,
                const tshirt_state &next_state,
                const tshirt_fluxes &calculated_fluxes,
                double timestep_seconds) {
            // TODO: implement
            return 0;
        }
    };
}

//!

//!



#endif //TSHIRT_H
