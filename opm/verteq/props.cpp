#include <opm/verteq/props.hpp>
#include <opm/verteq/topsurf.hpp>
#include <opm/verteq/upscale.hpp>
#include <opm/verteq/utility/exc.hpp>
#include <opm/verteq/utility/runlen.hpp>
#include <algorithm> // fill
#include <memory> // auto_ptr
using namespace Opm;
using namespace std;

struct VertEqPropsImpl : public VertEqProps {
	/// Get the underlaying fluid information from here
	const IncompPropertiesInterface& fp;

	/// Get the grid information from here
	const TopSurf& ts;

	// constants to avoid a bunch of "magic" values in the code
	static const int TWO_DIMS   = 2;
	static const int THREE_DIMS = 3;

	// size of the permeability matrices (in numbers)
	static const int PERM_MATRIX_2D = TWO_DIMS * TWO_DIMS;
	static const int PERM_MATRIX_3D = THREE_DIMS * THREE_DIMS;

	// offsets when indexing into the permeability matrix
	static const int KXX_OFS_3D = 0 * THREE_DIMS + 0; // (x, x), x = 0
	static const int KXY_OFS_3D = 0 * THREE_DIMS + 1; // (x, y), x = 0, y = 1
	static const int KYY_OFS_3D = 1 * THREE_DIMS + 1; // (y, y), y = 1

	static const int KXX_OFS_2D = 0 * TWO_DIMS + 0; // (x, x), x = 0
	static const int KXY_OFS_2D = 0 * TWO_DIMS + 1; // (x, y), x = 0, y = 1
	static const int KYX_OFS_2D = 1 * TWO_DIMS + 0; // (y, x), x = 0, y = 1
	static const int KYY_OFS_2D = 1 * TWO_DIMS + 1; // (y, y), y = 1

	// we assume this ordering of the phases in arrays
	static const int GAS = 0;
	static const int WAT = 1;

	/// Helper object to do averaging
	const VertEqUpscaler up;

	/// Upscaled porosity; this is \Phi in the papers
	vector <double> upscaled_poro;

	/// Upscaled permeability; this is K in the papers
	vector <double> upscaled_absperm;

	/// Volume fractions of gas phase, used in averaging
	RunLenData <double> res_gas_vol; // \phi S_{n,r}
	RunLenData <double> mob_mix_vol; // \phi (1 - S_{w,r} - S_{n,r})
	RunLenData <double> res_wat_vol; // \phi (1 - S_{w,r})

	/// Volume-of-gas-phase-fraction-weighted depths-fractions
	RunLenData <double> res_gas_dpt; // int_{h}^{\zeta_T} \phi S_{n,r} dz
	RunLenData <double> mob_mix_dpt; // int_{h}^{\zeta_T} \phi (1 - S_{w,r} - S_{n,r} dz
	RunLenData <double> res_wat_dpt; // int_{h}^{\zeta_T} \phi (1 - S_{w,r}) dz

	// we need to keep track of where the plume has been and deposited
	// residual CO2. however, finding the interface is non-trivial and
	// should only be done if we actually see a new maximum of the
	// saturation. this array contains the trigger point for recalc.
	vector <double> max_gas_sat;      // S_{g,max}
	vector <Elevation> max_gas_elev;  // \zeta_R

	virtual void upd_res_sat (const double* snap) {
		// cache this here outside of the loop
		const int num_phases = numPhases ();

		// update saturation for each column
		for (int col = 0; col < ts.number_of_cells; ++col) {
			// current CO2 saturation
			const double cur_sat = snap[col * num_phases + GAS];

			// has it increased? is there more of the plume in this column?
			check_res_sat (col, cur_sat);
		}
	}

	void check_res_sat (int col, double cur_sat) {
		if (cur_sat > max_gas_sat[col]) {
			// recalculate discretized elevation
			max_gas_elev[col] = res_elev (col, cur_sat);

			// update stored saturation so we test correctly next time
			max_gas_sat[col] = cur_sat;
		}
	}

	/**
	 * Find the elevation of the residual CO2 in this column based on the
	 * maximum upscaled CO2 saturation.
	 *
	 * This is done by solving this equation for \zeta_R:
	 *
	 * H \Phi S_{g,max} = \int_{\zeta_R}^{\zeta_T} \phi (1 - s_{w,r}) dz
	 *
	 * using precalculated values for the integral.
	 */
	Elevation res_elev(const int col, const double max_sat) {
		// right-hand side of the equation (apart from H, which is divided
		// in the averaging operator stored)
		const double max_vol = upscaled_poro[col] * max_sat;

		// find the elevation which makes the integral have this value
		const Elevation zeta_r = up.find (col, res_wat_dpt[col], max_vol);
		return zeta_r;
	}

	VertEqPropsImpl (const IncompPropertiesInterface& fineProps,
	                 const TopSurf& topSurf)
		: fp (fineProps)
		, ts (topSurf)
		, up (ts)
		, res_gas_vol (ts.number_of_cells, ts.col_cellpos)
		, mob_mix_vol (ts.number_of_cells, ts.col_cellpos)
		, res_wat_vol (ts.number_of_cells, ts.col_cellpos)
		, res_gas_dpt (ts.number_of_cells, ts.col_cellpos)
		, mob_mix_dpt (ts.number_of_cells, ts.col_cellpos)
		, res_wat_dpt (ts.number_of_cells, ts.col_cellpos)

		// assume that there is no initial plume; first notification will
		// trigger an update of all columns where there actually is CO2
		, max_gas_sat (ts.number_of_cells, 0.)

		// this is the elevation that corresponds to no CO2 sat.
		, max_gas_elev (ts.number_of_cells, Elevation (0, 0.)) {

		// allocate memory to store results for faster lookup later
		upscaled_poro.resize (ts.number_of_cells);

		// number of phases (should be 2)
		const int num_phases = fp.numPhases ();

		// buffers that holds intermediate values for each column;
		// pre-allocate to avoid doing that inside the loop
		vector <double> poro (ts.max_vert_res, 0.); // porosity
		vector <double> kxx (ts.max_vert_res, 0.);  // abs.perm.
		vector <double> kxy (ts.max_vert_res, 0.);
		vector <double> kyy (ts.max_vert_res, 0.);
		vector <double> sgr   (ts.max_vert_res * num_phases, 0.); // residual CO2
		vector <double> l_swr (ts.max_vert_res * num_phases, 0.); // 1 - residual brine

		// pointer to all porosities in the fine grid
		const double* fine_poro = fp.porosity ();
		const double* fine_perm = fp.permeability ();

		// upscale each column separately
		for (int col = 0; col < ts.number_of_cells; ++col) {
			// retrieve the fine porosities for this column only
			up.gather (col, &poro[0], fine_poro, 1, 0);

			// compute the depth-averaged value and store
			upscaled_poro[col] = up.dpt_avg (col, &poro[0]);

			// retrieve the fine abs. perm. for this column only
			up.gather (col, &kxx[0], fine_perm, PERM_MATRIX_3D, KXX_OFS_3D);
			up.gather (col, &kxy[0], fine_perm, PERM_MATRIX_3D, KXY_OFS_3D);
			up.gather (col, &kyy[0], fine_perm, PERM_MATRIX_3D, KYY_OFS_3D);

			// compute upscaled values for each dimension separately
			const double up_kxx = up.dpt_avg (col, &kxx[0]);
			const double up_kxy = up.dpt_avg (col, &kxy[0]);
			const double up_kyy = up.dpt_avg (col, &kyy[0]);

			// store back into the interleaved format required by the 2D
			// simulator code (fetching a tensor at the time, probably)
			// notice that we take advantage of the tensor being symmetric
			// at the third line below
			upscaled_absperm[PERM_MATRIX_2D * col + KXX_OFS_2D] = up_kxx;
			upscaled_absperm[PERM_MATRIX_2D * col + KXY_OFS_2D] = up_kxy;
			upscaled_absperm[PERM_MATRIX_2D * col + KYX_OFS_2D] = up_kxy;
			upscaled_absperm[PERM_MATRIX_2D * col + KYY_OFS_2D] = up_kyy;

			// query the fine properties for the residual saturations;
			// notice that we implicitly get the brine saturation as the maximum
			// allowable co2 saturation; now we've got the values we need, but
			// only every other item (due to that both phases are stored)
			const rlw_int col_cells (ts.number_of_cells, ts.col_cellpos, ts.col_cells);
			fp.satRange (col_cells.size (col), col_cells[col], &sgr[0], &l_swr[0]);

			// cache pointers to this particular column to avoid recomputing
			// the starting point for each and every item
			double* res_gas_col = res_gas_vol[col];
			double* mob_mix_col = mob_mix_vol[col];
			double* res_wat_col = res_wat_vol[col];

			for (int row = 0; row < col_cells.size (col); ++row) {
				// multiply with num_phases because the saturations for *both*
				// phases are store consequtively (as a record); we only need
				// the residuals framed as co2 saturations
				const double sgr_ = sgr[row * num_phases + GAS];
				const double l_swr_ = l_swr[row * num_phases + GAS];

				// portions of the block that are filled with: residual co2,
				// mobile fluid and residual brine, respectively
				res_gas_col[row] = poro[row] * sgr_;            // \phi*S_{n,r}
				mob_mix_col[row] = poro[row] * (l_swr_ - sgr_); // \phi*(1-S_{w,r}-S_{n_r})
				res_wat_col[row] = poro[row] * l_swr_;          // \phi*(1-S_{w,r}
			}

			// weight the relative depth factor (how close are we towards a
			// completely filled column) with the volume portions
			up.wgt_dpt (col, &res_gas_col[0], &res_gas_dpt[col][0]);
			up.wgt_dpt (col, &mob_mix_col[0], &mob_mix_dpt[col][0]);
			up.wgt_dpt (col, &res_gas_col[0], &res_wat_dpt[col][0]);
		}
	}

	/* rock properties; use volume-weighted averages */
	virtual int numDimensions () const {
		// the upscaled grid is always dimensionally reduced
		return TWO_DIMS;
	}

	virtual int numCells () const {
		// we'll provide on value for each column in the upscaled grid
		return ts.number_of_cells;
	}

	virtual const double* porosity () const {
		// calculated in the constructor; since we must return a full
		// array there isn't anything to save by calculating on the fly
		// (accessing the data like this is supposed to be safe as long
		// as the container "lives")
		return &upscaled_poro[0];
	}

	virtual const double* permeability () const {
		return &upscaled_absperm[0];
	}

	/* fluid properties; these don't change when upscaling */
	virtual int numPhases () const {
		return fp.numPhases ();
	}

	virtual const double* viscosity () const {
		return fp.viscosity ();
	}

	virtual const double* density () const {
		return fp.density ();
	}

	virtual const double* surfaceDensity () const {
		return fp.surfaceDensity ();
	}

	/* hydrological (unsaturated zone) properties */
	virtual void relperm (const int n,
	                      const double *s,
	                      const int *cells,
	                      double *kr,
	                      double *dkrds) const {
		throw OPM_EXC ("Not implemented yet");
	}

	virtual void capPress (const int n,
	                       const double *s,
	                       const int *cells,
	                       double *pc,
	                       double *dpcds) const {
		throw OPM_EXC ("Not implemented yet");
	}

	virtual void satRange (const int n,
	                       const int *cells,
	                       double *smin,
	                       double *smax) const {
		// saturation is just another name for "how much of the column
		// is filled", so every range from nothing to completely filled
		// are valid. even though there is residual water/gas in each
		// block, this is not seen from the 2D code
		const int np = n * numPhases ();
		fill (smin, smin + np, 0.);
		fill (smax, smax + np, 1.);
	}
};

VertEqProps*
VertEqProps::create (const IncompPropertiesInterface& fineProps,
                     const TopSurf& topSurf) {
	// construct real object which contains all the implementation details
	auto_ptr <VertEqProps> props (new VertEqPropsImpl (fineProps, topSurf));

	// client owns pointer to constructed fluid object from this point
	return props.release ();
}
