#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace RTC
{
	struct SlackFeature
	{
		double rttMs{ 0.0 };
		double lossRate{ 0.0 };
		double aceQueueBytes{ 0.0 };
		double pacingBacklogBytes{ 0.0 };
		double frameSizeBytes{ 0.0 };

		// optional
		double packetCount{ 0.0 };
		double temporalLayer{ 0.0 };
		double isKeyFrame{ 0.0 }; // 0 or 1
	};

	struct SlackSample
	{
		SlackFeature feature;
		double slackMs{ 0.0 };
		uint32_t frameId{ 0 };
	};

	class SlackPredictor
	{
	public:
		struct Config
		{
			size_t maxSamples{ 2000 };
			size_t k{ 5 };
			size_t minSamplesToPredict{ 50 };

			// normalization scale
			double rttScale{ 100.0 };
			double lossScale{ 0.05 };
			double aceQueueScale{ 50000.0 };
			double pacingBacklogScale{ 50000.0 };
			double frameSizeScale{ 30000.0 };
			double packetCountScale{ 30.0 };
			double temporalLayerScale{ 3.0 };
			double isKeyFrameScale{ 1.0 };

			// feature weights
			double wRtt{ 1.0 };
			double wLoss{ 1.0 };
			double wAceQueue{ 1.5 };
			double wPacingBacklog{ 1.5 };
			double wFrameSize{ 0.7 };
			double wPacketCount{ 0.3 };
			double wTemporalLayer{ 0.2 };
			double wIsKeyFrame{ 0.2 };

			// numerical stability for inverse-distance weighting
			double epsilon{ 1e-6 };
		};

	public:
		SlackPredictor();
        explicit SlackPredictor(const Config& config);

	public:
		void AddSample(const SlackSample& sample);
		size_t GetSampleCount() const;
		bool CanPredict() const;

		// return nullopt if not enough samples
		std::optional<double> PredictSlackMs(const SlackFeature& feature) const;

		// simple heuristic fallback if needed
		double PredictHeuristicSlackScore(const SlackFeature& feature) const;

		void Clear();

	private:
		struct Neighbor
		{
			double distance{ 0.0 };
			double slackMs{ 0.0 };
		};

	private:
		double ComputeDistance(const SlackFeature& a, const SlackFeature& b) const;
		double Normalize(double value, double scale) const;
		double ClampNonNegative(double value) const;

	private:
		Config config;
		mutable std::mutex mutex;
		std::deque<SlackSample> samples;
	};
} // namespace RTC