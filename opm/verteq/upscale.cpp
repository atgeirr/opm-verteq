#include <opm/verteq/upscale.hpp>
#include <opm/verteq/utility/runlen.hpp>

using namespace Opm;

void
VertEqUpscaler::gather (
		int col,
		double* buf,
		const double* data,
		int stride,
		int offset) const {

	// index into the fine grid for all columns
	const rlw_int col_cells (ts.number_of_cells, ts.col_cellpos, ts.col_cells);

	// get the indices for this particular column
	const int* fine_ndx = col_cells[col];

	// loop through each block in the column and fetch the property
	for (int row = 0; row < col_cells.size (col); ++row) {
		// index in the fine grid for this block
		const int block_ndx = fine_ndx[row];

		// calculate position in the data array
		const int pos = block_ndx * stride + offset;

		// move the data
		buf[row] = data[pos];
	}
}

void
VertEqUpscaler::wgt_dpt (
		int col,
		const double* val,
		double* res) const {

	// get the weights for this particular column
	const rlw_double dz = rlw_double (ts.number_of_cells, ts.col_cellpos, ts.dz);
	const double* dz_col = dz[col];

	// running total
	double accum = 0.;

	// divisor for this particular column. this should be larger than zero
	// since the column otherwise wouldn't be active
	double H = ts.h_tot[col];

	// loop through each of the blocks from the top
	for (int row = 0; row < dz.size(col); ++row) {
		// this is \int_{h}^{\zeta_T} f
		accum += val[row] * dz_col[row];

		// write the average to the resulting array
		res[row] = accum / H;
	}
}

int
VertEqUpscaler::num_rows (
    int col) const {

	// use this helper object to query about the size of the column
	// (the compiler should be able to optimize most of it away)
	const rlw_int pos (ts.number_of_cells, ts.col_cellpos, ts.col_cells);
	return pos.size (col);
}

Elevation
VertEqUpscaler::bottom (
    int col) const {

	// simply initialize to skip *all* blocks in that column
	return Elevation (num_rows (col), 0.);
}
