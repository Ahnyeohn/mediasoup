#include "RTC/SlackPredictor.hpp"

#include <algorithm>
#include <cmath>

namespace RTC
{
	SlackPredictor::SlackPredictor() : config(Config{})
	{
	}

	SlackPredictor::SlackPredictor(const Config& config) : config(config)
	{
	}

	bool SlackPredictor::AddSample(const SlackSample& sample)
	{
		if (!IsValidFeature(sample.feature) || !std::isfinite(sample.decodeSlackMs))
		{
			return false;
		}

		const size_t spatialLayer = static_cast<size_t>(sample.action.spatialLayer);

		if (spatialLayer >= this->spatialLayerSampleCounts.size())
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(this->mutex);

		if (sample.origin == KnnSampleOrigin::COLD_START)
		{
			// Cold Start samples are retained permanently.
			this->coldStartSamples.push_back(sample);
			++this->spatialLayerSampleCounts[spatialLayer];
		}
		else
		{
			// Online samples use FIFO aging only within the online pool.
			this->onlineSamples.push_back(sample);
			++this->spatialLayerSampleCounts[spatialLayer];

			while (this->onlineSamples.size() > this->config.maxOnlineSamples)
			{
				const auto& oldSample = this->onlineSamples.front();
				const size_t oldSpatialLayer = static_cast<size_t>(oldSample.action.spatialLayer);

				if (
				  oldSpatialLayer < this->spatialLayerSampleCounts.size() &&
				  this->spatialLayerSampleCounts[oldSpatialLayer] > 0u)
				{
					--this->spatialLayerSampleCounts[oldSpatialLayer];
				}

				this->onlineSamples.pop_front();
			}
		}

		if (!this->initialSpatialCoverageSatisfied)
		{
			const bool l0Ready =
			  this->spatialLayerSampleCounts[0] >= this->config.minSamplesPerSpatialLayer;
			const bool l1Ready =
			  this->spatialLayerSampleCounts[1] >= this->config.minSamplesPerSpatialLayer;
			const bool l2Ready =
			  this->spatialLayerSampleCounts[2] >= this->config.minSamplesPerSpatialLayer;

			if (l0Ready && l1Ready && l2Ready)
			{
				this->initialSpatialCoverageSatisfied = true;
			}
		}

		return true;
	}

	size_t SlackPredictor::GetSampleCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		return this->coldStartSamples.size() + this->onlineSamples.size();
	}

	size_t SlackPredictor::GetColdStartSampleCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		return this->coldStartSamples.size();
	}

	size_t SlackPredictor::GetOnlineSampleCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		return this->onlineSamples.size();
	}

	bool SlackPredictor::CanPredictLocked() const
	{
		const size_t totalSamples = this->coldStartSamples.size() + this->onlineSamples.size();

		return totalSamples >= this->config.minSamplesToPredict &&
		       this->initialSpatialCoverageSatisfied;
	}

	bool SlackPredictor::CanPredict() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return CanPredictLocked();
	}

	bool SlackPredictor::IsSameAction(
	  const SlackActionIdentity& a, const SlackActionIdentity& b) const
	{
		return a.spatialLayer == b.spatialLayer &&
		       a.fecProtectionFactor == b.fecProtectionFactor &&
		       a.pacingEnabled == b.pacingEnabled;
	}

	std::optional<RTC::SlackPrediction> SlackPredictor::Predict(
	  const SlackFeature& feature, const SlackActionIdentity& action) const
	{
		if (!IsValidFeature(feature))
		{
			return std::nullopt;
		}

		std::lock_guard<std::mutex> lock(this->mutex);

		if (!CanPredictLocked())
		{
			return std::nullopt;
		}

		const size_t totalSampleCount = this->coldStartSamples.size() + this->onlineSamples.size();

		std::vector<Neighbor> neighbors;
		neighbors.reserve(totalSampleCount);

		const auto appendNeighbors = [&](const std::deque<SlackSample>& samples)
		{
			for (const auto& sample : samples)
			{
				if (
				  RTC::KnnExperiment::ActionConditionedPredictionEnabled &&
				  !IsSameAction(action, sample.action))
				{
					continue;
				}

				Neighbor neighbor;
				neighbor.distance = ComputeDistance(feature, action, sample.feature, sample.action);
				neighbor.decodeSlackMs = sample.decodeSlackMs;
				neighbor.deadlineMiss = sample.deadlineMiss;
				neighbor.sample = &sample;

				neighbors.push_back(neighbor);
			}
		};

		appendNeighbors(this->coldStartSamples);
		appendNeighbors(this->onlineSamples);

		// In action-conditioned mode, do not fabricate a prediction from fewer
		// than k exact-action historical samples.
		if (RTC::KnnExperiment::ActionConditionedPredictionEnabled && neighbors.size() < this->config.k)
		{
			return std::nullopt;
		}

		const size_t k = std::min(this->config.k, neighbors.size());

		if (k == 0u)
		{
			return std::nullopt;
		}

		std::partial_sort(
		  neighbors.begin(),
		  neighbors.begin() + static_cast<std::ptrdiff_t>(k),
		  neighbors.end(),
		  [](const Neighbor& a, const Neighbor& b) { return a.distance < b.distance; });

		SlackPrediction prediction;
		prediction.sampleCount   = totalSampleCount;
		prediction.neighborCount = k;

		prediction.debugNeighborCount = std::min(k, KnnDebugNeighborCount);

		for (size_t i{ 0u }; i < prediction.debugNeighborCount; ++i)
		{
			const auto& neighbor = neighbors[i];

			if (!neighbor.sample)
			{
				continue;
			}

			auto& debug         = prediction.debugNeighbors[i];
			debug.frameId       = neighbor.sample->frameId;
			debug.distance      = neighbor.distance;
			debug.feature       = neighbor.sample->feature;
			debug.action        = neighbor.sample->action;
			debug.decodeSlackMs = neighbor.sample->decodeSlackMs;
			debug.deadlineMiss  = neighbor.sample->deadlineMiss;
		}

		double decodeSlackSumMs{ 0.0 };
		size_t deadlineMissCount{ 0u };

		for (size_t i{ 0u }; i < k; ++i)
		{
			const auto& neighbor = neighbors[i];
			decodeSlackSumMs += neighbor.decodeSlackMs;

			if (neighbor.deadlineMiss)
			{
				++deadlineMissCount;
			}
		}

		prediction.predictedDecodeSlackMs = decodeSlackSumMs / static_cast<double>(k);
		prediction.deadlineMissProbability =
		  static_cast<double>(deadlineMissCount) / static_cast<double>(k);

		return prediction;
	}

	double SlackPredictor::ComputeDistance(
	  const SlackFeature& aFeature,
	  const SlackActionIdentity& aAction,
	  const SlackFeature& bFeature,
	  const SlackActionIdentity& bAction) const
	{
		const double dRtt =
		  NormalizeDifference(aFeature.rttMs, bFeature.rttMs, this->config.rttScaleMs);
		const double dLoss =
		  NormalizeDifference(aFeature.lossRate, bFeature.lossRate, this->config.lossScale);
		const double dBw = NormalizeDifference(
		  aFeature.availableBitrateBps,
		  bFeature.availableBitrateBps,
		  this->config.bandwidthScaleBps);
		const double dCongestion = NormalizeDifference(
		  aFeature.congestion, bFeature.congestion, this->config.congestionScale);
		const double dFrameSize = NormalizeDifference(
		  aFeature.frameSizeBytes, bFeature.frameSizeBytes, this->config.frameSizeScaleBytes);
		const double dPacingDelay = NormalizeDifference(
		  aFeature.pacingDelayMs, bFeature.pacingDelayMs, this->config.pacingDelayScaleMs);

		// Legacy action-distance terms. In action-conditioned mode all three are
		// zero because non-identical actions were filtered before this call.
		const double dSpatialLayer =
		  aAction.spatialLayer == bAction.spatialLayer ? 0.0 : this->config.spatialLayerMismatchDistance;

		const double dFec = NormalizeDifference(
		  static_cast<double>(aAction.fecProtectionFactor),
		  static_cast<double>(bAction.fecProtectionFactor),
		  this->config.fecProtectionScale);

		const double dPacing =
		  aAction.pacingEnabled == bAction.pacingEnabled ? 0.0 : this->config.pacingMismatchDistance;

		const double squaredDistance =
		  dRtt * dRtt + dLoss * dLoss + dBw * dBw + dCongestion * dCongestion +
		  dFrameSize * dFrameSize + dPacingDelay * dPacingDelay +
		  dSpatialLayer * dSpatialLayer + dFec * dFec + dPacing * dPacing;

		return std::sqrt(squaredDistance);
	}

	double SlackPredictor::NormalizeDifference(double a, double b, double scale) const
	{
		if (scale <= 0.0)
		{
			return a - b;
		}

		return (a - b) / scale;
	}

	bool SlackPredictor::IsValidFeature(const SlackFeature& feature) const
	{
		return std::isfinite(feature.rttMs) &&
		       std::isfinite(feature.lossRate) &&
		       std::isfinite(feature.availableBitrateBps) &&
		       std::isfinite(feature.congestion) &&
		       std::isfinite(feature.frameSizeBytes) &&
		       std::isfinite(feature.pacingDelayMs);
	}

	void SlackPredictor::Clear()
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		this->coldStartSamples.clear();
		this->onlineSamples.clear();
		this->spatialLayerSampleCounts.fill(0u);
		this->initialSpatialCoverageSatisfied = false;
	}

} // namespace RTC
