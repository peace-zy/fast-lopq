#pragma once

#include "model.h"

#include <string>
#include <vector>
#include <cstdint>
#include <blaze/Math.h>
#include <unordered_map>


namespace lopq {

struct Searcher {
	struct Cluster final {
		std::vector<std::string> ids;
		std::vector<Model::FineCode> vectors;
	};

	struct Options final {
		Options& limit(uint32_t q) {
			quota = q;
			return *this;
		}

		Options& deduplication() {
			dedup = true;
			return *this;
		}

		Options& no_deduplication() {
			dedup = false;
			return *this;
		}

		Options& deduplication(float threshold) {
			this->dedup = true;
			this->dedup_threshold = threshold;
			return *this;
		}

		size_t quota = 12;
		bool dedup = false;
		float dedup_threshold = 0.0001;
	};

	Options& configure() {
		return options;
	}

	struct Response final {
		explicit Response(std::string_view id)
			: id(id), distance(0) { }

		explicit Response(std::string_view id, float distance)
			: id(id), distance(distance) { }

		std::string id;
		float distance;
	};

	void load_model(const std::string& proto_path);

	std::vector<Response> search(const Model::FeatureVector& x);
	std::vector<Response> search_in(const Model::CoarseCode& coarse_code, const Model::FeatureVector& x);

protected:
	virtual Cluster& get_cell(const Model::CoarseCode& coarse_code) = 0;

private:
	Model model;
	std::unordered_map<int, Cluster> clusters;
	Options options;

	using DistanceCache = std::unordered_map<int, blaze::DynamicVector<Model::FloatVector>>;

	float distance(const Model::FeatureVector& x, blaze::StaticVector<uint8_t, 2UL> coarse_code, blaze::StaticVector<uint8_t, 16UL>& fine_code, DistanceCache& cache) const;
};

} // lopq
