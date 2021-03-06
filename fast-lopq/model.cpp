#include "include/fast-lopq/model.h"

#include <fstream>

#include <blaze/Math.h>
#include <blaze/math/Row.h>
#include <blaze/math/Vector.h>
#include <blaze/math/Matrix.h>

#include "lopq_model.pb.h"


namespace {

uint8_t predict_cluster(const blaze::DynamicVector<double>& x, const blaze::DynamicMatrix<float>& centroids) {
	blaze::DynamicVector<double, blaze::rowVector> tx = blaze::trans(x);
	blaze::DynamicMatrix<float> cx(centroids);
	for (uint32_t r = 0; r < cx.rows(); ++r)
		blaze::row(cx, r) = tx - blaze::row(cx, r);

	blaze::DynamicMatrix<float> cx_sq = map(cx, [](float d) { return d * d; } );

	blaze::DynamicVector<float, blaze::columnVector> multiplier(cx_sq.columns(), 1.0);
	blaze::DynamicVector<float> cx_sum = cx_sq * multiplier;

	return std::distance(cx_sum.begin(), std::min_element(cx_sum.begin(), cx_sum.end()));
}

} // namespace


namespace lopq {

void Model::load(const std::string& proto_path) {
	com::flickr::vision::lopq::LOPQModelParams lopq_params;

	std::ifstream proto_stream(proto_path);
	lopq_params.ParseFromIstream(&proto_stream);

	num_coarse_splits = lopq_params.cs_size();
	num_fine_splits = lopq_params.subs_size() / 2;

	for (uint32_t ci = 0; ci < num_coarse_splits; ++ci) {
		const auto& cs = lopq_params.cs(ci);

		auto C = FloatMatrix(cs.shape(0), cs.shape(1));
		for (uint32_t i = 0; i < C.rows(); ++i) {
			auto crow = blaze::row(C, i);
			for (uint32_t j = 0; j < C.columns(); ++j)
				crow[j] = cs.values(i * C.columns() + j);
		}
		Cs[ci] = C;
	}

	uint32_t rs_size = lopq_params.rs_size();
	uint32_t rs_half = rs_size / 2;
	for (uint32_t ri = 0; ri < 2; ++ri)
		Rs[ri] = blaze::DynamicVector<FloatMatrix>(rs_half);
	for (uint32_t c = 0; c < rs_size; ++c) {
		const auto& rs = lopq_params.rs(c);

		auto R = FloatMatrix(rs.shape(0), rs.shape(1));
		for (uint32_t i = 0; i < R.rows(); ++i) {
			auto crow = blaze::row(R, i);
			for (uint32_t j = 0; j < R.columns(); ++j)
				crow[j] = rs.values(i * R.columns() + j);
		}
		Rs[c / rs_half][c % rs_half] = R;
	}

	uint32_t mus_size = lopq_params.mus_size();
	uint32_t mus_half = mus_size / 2;
	for (uint32_t mui = 0; mui < 2; ++mui)
		mus[mui] = blaze::DynamicVector<FloatVector>(mus_half);
	for (uint32_t c = 0; c < mus_size; ++c) {
		const auto& mu = lopq_params.mus(c);
		auto mui = c / mus_half;
		auto musi = c % mus_half;

		mus[mui][musi] = FloatVector(mu.values_size());
		for (uint32_t i = 0; i < mus[mui][musi].size(); ++i)
			mus[mui][musi][i] = mu.values(i);
	}

	uint32_t subs_size = lopq_params.subs_size();
	uint32_t subs_half = subs_size / 2;
	for (uint32_t si = 0; si < 2; ++si)
		subquantizers[si] = blaze::DynamicVector<FloatMatrix>(subs_half);
	for (uint32_t c = 0; c < subs_size; ++c) {
		const auto& subs = lopq_params.subs(c);

		auto S = FloatMatrix(subs.shape(0), subs.shape(1));
		for (uint32_t i = 0; i < S.rows(); ++i) {
			auto crow = blaze::row(S, i);
			for (uint32_t j = 0; j < S.columns(); ++j)
				crow[j] = subs.values(i * S.columns() + j);
		}
		subquantizers[c / subs_half][c % subs_half] = S;
	}
}

const blaze::StaticVector<double, 128UL> Model::project(const FeatureVector& x, const CoarseCode& coarse_codes) const {
	uint32_t split_size = x.size() / num_coarse_splits;

	blaze::StaticVector<double, 128UL> result;

	for (uint32_t split = 0; split < num_coarse_splits; ++split) {
		auto cx = blaze::subvector(x, split * split_size, split_size);

		auto& cluster = coarse_codes[split];

		// Compute residual
		blaze::DynamicVector<float, blaze::rowVector> crow = blaze::row(Cs[split], cluster);
		blaze::CustomVector<float, blaze::aligned, blaze::padded, blaze::columnVector> row_v(&crow[0], crow.size(), crow.size());
		auto r = cx - row_v;

		// Project residual to local frame
		auto pr = Rs[split][cluster] * (r - mus[split][cluster]);

		blaze::subvector(result, split * split_size, split_size) = pr;
	}

	return result;
}

Model::CoarseCode Model::predict_coarse(const FeatureVector& x) const {
	Model::CoarseCode coarse_codes;

	uint32_t split_size = x.size() / num_coarse_splits;
	for (uint32_t split = 0; split < num_coarse_splits; ++split) {
		auto cx = blaze::subvector(x, split * split_size, split_size);

		coarse_codes[split] = predict_cluster(cx, Cs[split]);
	}

	return coarse_codes;
}

Model::FineCode Model::predict_fine(const FeatureVector& x, const CoarseCode& coarse_codes) const {
	Model::FineCode fine_codes;

	auto px = project(x, coarse_codes);

	uint32_t split_size = px.size() / num_coarse_splits;
	for (uint32_t split = 0; split < num_coarse_splits; ++split) {
		auto cx = blaze::subvector(px, split * split_size, split_size);

		// Compute subquantizer codes
		uint32_t subsplit_size = cx.size() / num_fine_splits;
		for (uint32_t subsplit = 0; subsplit < num_fine_splits; ++subsplit) {
			blaze::DynamicVector<double> fx = blaze::subvector(cx, subsplit * subsplit_size, subsplit_size);

			fine_codes[split * num_fine_splits + subsplit] = predict_cluster(fx, subquantizers[split][subsplit]);
		}
	}

	return fine_codes;
}

blaze::DynamicVector<Model::FloatVector> Model::subquantizer_distances(const FeatureVector& x, const CoarseCode& coarse_codes, uint32_t split) const {
	auto px = project(x, coarse_codes);

	uint32_t split_size = px.size() / num_coarse_splits;

	auto sx = blaze::subvector(px, split * split_size, split_size);

	uint32_t subsplit_size = sx.size() / num_fine_splits;

	blaze::DynamicVector<Model::FloatVector> distances(num_fine_splits);

	for (uint32_t subsplit = 0; subsplit < num_fine_splits; ++subsplit) {
		blaze::DynamicVector<double> fx = blaze::subvector(sx, subsplit * subsplit_size, subsplit_size);

		blaze::DynamicVector<double, blaze::rowVector> tx = blaze::trans(fx);
		blaze::DynamicMatrix<float> cx(subquantizers[split][subsplit]);
		for (uint32_t r = 0; r < cx.rows(); ++r)
			blaze::row(cx, r) = tx - blaze::row(cx, r);

		blaze::DynamicMatrix<float> cx_sq = map(cx, [](float d) { return d * d; } );

		blaze::DynamicVector<float, blaze::columnVector> multiplier(cx_sq.columns(), 1.0);

		distances[subsplit] = cx_sq * multiplier;
	}

	return distances;
}

} // lopq
