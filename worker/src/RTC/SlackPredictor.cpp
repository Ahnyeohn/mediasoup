#include "RTC/SlackPredictor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

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
				const auto& oldSample        = this->onlineSamples.front();
				const size_t oldSpatialLayer = static_cast<size_t>(oldSample.action.spatialLayer);

				if (oldSpatialLayer < this->spatialLayerSampleCounts.size() && this->spatialLayerSampleCounts[oldSpatialLayer] > 0u)
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

		// ============================================
		// Online boundary update
		// ============================================

		if (sample.origin == KnnSampleOrigin::ONLINE && this->calibration.coldCalibrationInitialized)
		{
			++this->calibration.onlineSamplesSinceUpdate;

			if (this->calibration.onlineSamplesSinceUpdate >= this->config.boundaryUpdateInterval)
			{
				this->calibration.onlineSamplesSinceUpdate = 0u;

				UpdateSafetyBoundariesLocked();
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

	std::optional<double> SlackPredictor::EstimateEmpiricalBoundary(
	  const std::vector<const SlackSample*>& samples, size_t layer) const
	{
		struct RiskBin
		{
			size_t total{ 0u };
			size_t failures{ 0u };
		};

		std::map<int, RiskBin> bins;

		const double binWidth = this->config.safetyBinWidthMs;

		if (binWidth <= 0.0)
		{
			return std::nullopt;
		}

		// ============================================
		// 1. Build layer-specific Slack histogram
		// ============================================

		for (const auto* sample : samples)
		{
			if (!sample)
			{
				continue;
			}

			if (sample->action.spatialLayer != layer)
			{
				continue;
			}

			const double slack = sample->decodeSlackMs;

			if (!std::isfinite(slack))
			{
				continue;
			}

			const int binIndex = static_cast<int>(std::floor(slack / binWidth));

			auto& bin = bins[binIndex];

			++bin.total;

			if (sample->outcome == FrameOutcome::LATE || sample->outcome == FrameOutcome::PRE_DECODE_DROP)
			{
				++bin.failures;
			}
		}

		if (bins.empty())
		{
			return std::nullopt;
		}

		// ============================================
		// 2. Find lowest Slack bin in a contiguous
		//    observed SAFE region
		// ============================================

		bool foundSafe{ false };
		double boundary{ 0.0 };

		for (auto it = bins.rbegin(); it != bins.rend(); ++it)
		{
			const auto& bin = it->second;

			// Insufficient statistical support
			if (bin.total < this->config.minCalibrationBinSamples)
			{
				break;
			}

			const double failureRisk = static_cast<double>(bin.failures) / static_cast<double>(bin.total);

			// Empirical risk constraint
			if (failureRisk > this->config.maxCalibrationFailureRisk)
			{
				break;
			}

			boundary = static_cast<double>(it->first) * binWidth;

			foundSafe = true;
		}

		if (!foundSafe)
		{
			return std::nullopt;
		}

		return boundary;
	}

	void SlackPredictor::InitializeSafetyBoundaries()
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->calibration.coldCalibrationInitialized)
		{
			return;
		}

		// Explicit Cold completion 이후 한 번만 초기화.
		this->calibration.coldCalibrationInitialized = true;

		UpdateSafetyBoundariesLocked();
	}

	void SlackPredictor::UpdateSafetyBoundariesLocked()
	{
		std::vector<const SlackSample*> coldSamples;
		std::vector<const SlackSample*> onlineSamples;

		coldSamples.reserve(this->coldStartSamples.size());

		for (const auto& sample : this->coldStartSamples)
		{
			coldSamples.push_back(&sample);
		}

		// ============================================
		// Recent Online window
		// ============================================

		const size_t onlineCount =
		  std::min(this->config.maxRecentCalibrationSamples, this->onlineSamples.size());

		const size_t onlineBegin = this->onlineSamples.size() - onlineCount;

		for (size_t i = onlineBegin; i < this->onlineSamples.size(); ++i)
		{
			onlineSamples.push_back(&this->onlineSamples[i]);
		}

		// ============================================
		// Layer-specific boundary update
		// ============================================

		for (size_t layer{ 0u }; layer < 3u; ++layer)
		{
			const auto coldBoundary = EstimateEmpiricalBoundary(coldSamples, layer);

			const auto onlineBoundary = EstimateEmpiricalBoundary(onlineSamples, layer);

			if (!coldBoundary && !onlineBoundary)
			{
				continue;
			}

			double targetBoundary{ 0.0 };

			if (coldBoundary && onlineBoundary)
			{
				const double coldWeight = this->config.coldBoundaryWeight;

				targetBoundary = coldWeight * (*coldBoundary) + (1.0 - coldWeight) * (*onlineBoundary);
			}
			else if (coldBoundary)
			{
				targetBoundary = *coldBoundary;
			}
			else
			{
				targetBoundary = *onlineBoundary;
			}

			// Initial Cold calibration
			if (!this->calibration.initialized[layer])
			{
				this->calibration.boundary[layer] = targetBoundary;

				this->calibration.initialized[layer] = true;

				continue;
			}

			// ========================================
			// Online slow update: EMA + step clamp
			// ========================================

			const double oldBoundary = this->calibration.boundary[layer];

			const double smoothedBoundary =
			  oldBoundary + this->config.boundaryEmaAlpha * (targetBoundary - oldBoundary);

			const double delta = std::clamp(
			  smoothedBoundary - oldBoundary,
			  -this->config.maxBoundaryStepMs,
			  this->config.maxBoundaryStepMs);

			this->calibration.boundary[layer] = oldBoundary + delta;
		}
	}

	std::array<double, 3> SlackPredictor::GetSafetyBoundaries() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		std::array<double, 3> boundaries{};

		for (size_t layer{ 0u }; layer < 3u; ++layer)
		{
			if (this->calibration.initialized[layer])
			{
				boundaries[layer] = this->calibration.boundary[layer];
			}
			else
			{
				// No calibration -> never mark SAFE
				boundaries[layer] = std::numeric_limits<double>::infinity();
			}
		}

		return boundaries;
	}

	bool SlackPredictor::CanPredictLocked() const
	{
		const size_t totalSamples = this->coldStartSamples.size() + this->onlineSamples.size();

		return totalSamples >= this->config.minSamplesToPredict && this->initialSpatialCoverageSatisfied;
	}

	bool SlackPredictor::CanPredict() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return CanPredictLocked();
	}

	bool SlackPredictor::IsSameAction(const SlackActionIdentity& a, const SlackActionIdentity& b) const
	{
		return a.spatialLayer == b.spatialLayer && a.fecProtectionFactor == b.fecProtectionFactor &&
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
				if (RTC::KnnExperiment::ActionConditionedPredictionEnabled && !IsSameAction(action, sample.action))
				{
					continue;
				}

				Neighbor neighbor;
				neighbor.distance      = ComputeDistance(feature, action, sample.feature, sample.action);
				neighbor.decodeSlackMs = sample.decodeSlackMs;
				neighbor.deadlineMiss  = sample.deadlineMiss;
				neighbor.sample        = &sample;

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
		size_t lateCount{ 0u };
		size_t dropCount{ 0u };

		for (size_t i{ 0u }; i < k; ++i)
		{
			const auto& neighbor = neighbors[i];

			decodeSlackSumMs += neighbor.decodeSlackMs;

			if (neighbor.deadlineMiss)
			{
				++deadlineMissCount;
			}

			if (neighbor.sample)
			{
				switch (neighbor.sample->outcome)
				{
					case FrameOutcome::NORMAL:
						break;

					case FrameOutcome::LATE:
						++lateCount;
						break;

					case FrameOutcome::PRE_DECODE_DROP:
						++dropCount;
						break;
				}
			}
		}

		const double denominator = static_cast<double>(k);

		prediction.predictedDecodeSlackMs = decodeSlackSumMs / denominator;

		prediction.deadlineMissProbability = static_cast<double>(deadlineMissCount) / denominator;

		// NEW: outcome-specific risk
		prediction.lateProbability = static_cast<double>(lateCount) / denominator;

		prediction.dropProbability = static_cast<double>(dropCount) / denominator;

		return prediction;
	}

	double SlackPredictor::ComputeDistance(
	  const SlackFeature& aFeature,
	  const SlackActionIdentity& aAction,
	  const SlackFeature& bFeature,
	  const SlackActionIdentity& bAction) const
	{
		const double dRtt = NormalizeDifference(aFeature.rttMs, bFeature.rttMs, this->config.rttScaleMs);
		const double dLoss =
		  NormalizeDifference(aFeature.lossRate, bFeature.lossRate, this->config.lossScale);
		const double dBw = NormalizeDifference(
		  aFeature.availableBitrateBps, bFeature.availableBitrateBps, this->config.bandwidthScaleBps);
		const double dCongestion =
		  NormalizeDifference(aFeature.congestion, bFeature.congestion, this->config.congestionScale);
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
		  dRtt * dRtt + dLoss * dLoss + dBw * dBw + dCongestion * dCongestion + dFrameSize * dFrameSize +
		  dPacingDelay * dPacingDelay + dSpatialLayer * dSpatialLayer + dFec * dFec + dPacing * dPacing;

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
		return std::isfinite(feature.rttMs) && std::isfinite(feature.lossRate) &&
		       std::isfinite(feature.availableBitrateBps) && std::isfinite(feature.congestion) &&
		       std::isfinite(feature.frameSizeBytes) && std::isfinite(feature.pacingDelayMs);
	}

	void SlackPredictor::Clear()
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		this->coldStartSamples.clear();
		this->onlineSamples.clear();
		this->spatialLayerSampleCounts.fill(0u);
		this->initialSpatialCoverageSatisfied = false;

		this->calibration = CalibrationState{};
	}

} // namespace RTC
