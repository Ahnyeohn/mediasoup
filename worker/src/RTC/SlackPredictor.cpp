#include "RTC/SlackPredictor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace RTC
{
	SlackPredictor::SlackPredictor() : config(Config{})
	{
	}

	SlackPredictor::SlackPredictor(const Config& config) : config(config)
	{
	}

	void SlackPredictor::AddSample(const SlackSample& sample)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		this->samples.push_back(sample);

		while (this->samples.size() > this->config.maxSamples)
		{
			this->samples.pop_front();
		}
	}

	size_t SlackPredictor::GetSampleCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->samples.size();
	}

	bool SlackPredictor::CanPredict() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->samples.size() >= this->config.minSamplesToPredict;
	}

	std::optional<double> SlackPredictor::PredictSlackMs(const SlackFeature& feature) const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->samples.size() < this->config.minSamplesToPredict)
		{
			return std::nullopt;
		}

		std::vector<Neighbor> neighbors;
		neighbors.reserve(this->samples.size());

		for (const auto& sample : this->samples)
		{
			double distance = ComputeDistance(feature, sample.feature);

			Neighbor n;
			n.distance = distance;
			n.slackMs  = sample.slackMs;

			neighbors.push_back(n);
		}

		const size_t k = std::min(this->config.k, neighbors.size());

		std::partial_sort(
		  neighbors.begin(),
		  neighbors.begin() + static_cast<std::ptrdiff_t>(k),
		  neighbors.end(),
		  [](const Neighbor& a, const Neighbor& b) { return a.distance < b.distance; });

		double weightedSum = 0.0;
		double weightSum   = 0.0;

		for (size_t i = 0; i < k; ++i)
		{
			const double d = neighbors[i].distance;
			const double w = 1.0 / (d + this->config.epsilon);

			weightedSum += w * neighbors[i].slackMs;
			weightSum += w;
		}

		if (weightSum <= 0.0)
		{
			return std::nullopt;
		}

		return weightedSum / weightSum;
	}

	double SlackPredictor::PredictHeuristicSlackScore(const SlackFeature& feature) const
	{
		// This is not "true slack".
		// It is just a simple risk/stress-like score for fallback.
		const double rttN     = Normalize(feature.rttMs, this->config.rttScale);
		const double lossN    = Normalize(feature.lossRate, this->config.lossScale);
		const double queueN   = Normalize(feature.aceQueueBytes, this->config.aceQueueScale);
		const double backlogN = Normalize(feature.pacingBacklogBytes, this->config.pacingBacklogScale);
		// const double frameN   = Normalize(feature.frameSizeBytes, this->config.frameSizeScale);

		// Higher value means more "stress"/urgency
		const double score = 0.20 * rttN + 0.20 * lossN + 0.30 * queueN + 0.20 * backlogN + 0.10;

		return score;
	}

	void SlackPredictor::Clear()
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->samples.clear();
	}

	double SlackPredictor::ComputeDistance(const SlackFeature& a, const SlackFeature& b) const
	{
		const double artt  = Normalize(a.rttMs, this->config.rttScale);
		const double aloss = Normalize(a.lossRate, this->config.lossScale);
		const double aq    = Normalize(a.aceQueueBytes, this->config.aceQueueScale);
		const double ab    = Normalize(a.pacingBacklogBytes, this->config.pacingBacklogScale);
		// const double af    = Normalize(a.frameSizeBytes, this->config.frameSizeScale);
		// const double apc   = Normalize(a.packetCount, this->config.packetCountScale);
		// const double atl   = Normalize(a.temporalLayer, this->config.temporalLayerScale);
		// const double akf   = Normalize(a.isKeyFrame, this->config.isKeyFrameScale);

		const double brtt  = Normalize(b.rttMs, this->config.rttScale);
		const double bloss = Normalize(b.lossRate, this->config.lossScale);
		const double bq    = Normalize(b.aceQueueBytes, this->config.aceQueueScale);
		const double bb    = Normalize(b.pacingBacklogBytes, this->config.pacingBacklogScale);
		// const double bf    = Normalize(b.frameSizeBytes, this->config.frameSizeScale);
		// const double bpc   = Normalize(b.packetCount, this->config.packetCountScale);
		// const double btl   = Normalize(b.temporalLayer, this->config.temporalLayerScale);
		// const double bkf   = Normalize(b.isKeyFrame, this->config.isKeyFrameScale);

		const double drtt  = artt - brtt;
		const double dloss = aloss - bloss;
		const double dq    = aq - bq;
		const double db    = ab - bb;
		// const double df    = af - bf;
		// const double dpc   = apc - bpc;
		// const double dtl   = atl - btl;
		// const double dkf   = akf - bkf;

		const double sum = this->config.wRtt * drtt * drtt + this->config.wLoss * dloss * dloss +
		                   this->config.wAceQueue * dq * dq + this->config.wPacingBacklog * db * db;
						//    + this->config.wFrameSize * df * df + this->config.wPacketCount * dpc * dpc 
						//    + this->config.wTemporalLayer * dtl * dtl + this->config.wIsKeyFrame * dkf * dkf;

		return std::sqrt(sum);
	}

	double SlackPredictor::Normalize(double value, double scale) const
	{
		value = ClampNonNegative(value);

		if (scale <= 0.0)
		{
			return value;
		}

		return value / scale;
	}

	double SlackPredictor::ClampNonNegative(double value) const
	{
		if (value < 0.0)
		{
			return 0.0;
		}
		return value;
	}
} // namespace RTC