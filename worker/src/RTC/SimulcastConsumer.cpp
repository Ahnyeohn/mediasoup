#define MS_CLASS "RTC::SimulcastConsumer"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/SimulcastConsumer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "RTC/Codecs/Tools.hpp"
#ifdef MS_RTC_LOGGER_RTP
#include "RTC/RtcLogger.hpp"
#endif
#include <limits> // std::numeric_limits

// simulcast vp8 파싱
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <array>
#include <unordered_map>

#include "RTC/FrameRecord.hpp"
#include "RTC/NetworkState.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>

static std::ofstream g_knnLayerRankCsv(
  "/home/n2sl/yeon/qos/network/log/frame/knn_layer_rank.csv", std::ios::out | std::ios::trunc);
static bool g_knnLayerRankHeaderWritten{ false };

static std::ofstream g_knnNeighborCsv(
  "/home/n2sl/yeon/qos/network/log/frame/knn_neighbor_debug.csv", std::ios::out | std::ios::trunc);
static bool g_knnNeighborHeaderWritten{ false };

namespace
{

	static const char* GetKnnColdStartInitialTcRate(int16_t layer)
	{
		switch (layer)
		{
			case 2:
				return "14mbit";

			case 1:
				return "1mbit";

			case 0:
				return "14mbit";

			default:
				return "unknown";
		}
	}

	static const char* GetKnnColdStartNextTcRate(int16_t layer, size_t layerSampleCount)
	{
		switch (layer)
		{
			case 2:
			{
				// L2 : 14 -> 7 -> 2 -> 1
				if (layerSampleCount == 700u)
				{
					return "7mbit";
				}

				if (layerSampleCount == 1400u)
				{
					return "2mbit";
				}

				if (layerSampleCount == 2100u)
				{
					return "1mbit";
				}

				break;
			}

			case 1:
			{
				// L1 : 1 -> 2 -> 7 -> 14
				if (layerSampleCount == 700u)
				{
					return "2mbit";
				}

				if (layerSampleCount == 1400u)
				{
					return "7mbit";
				}

				if (layerSampleCount == 2100u)
				{
					return "14mbit";
				}

				break;
			}

			case 0:
			{
				// L0 : 14 -> 7 -> 2 -> 1
				if (layerSampleCount == 700u)
				{
					return "7mbit";
				}

				if (layerSampleCount == 1400u)
				{
					return "2mbit";
				}

				if (layerSampleCount == 2100u)
				{
					return "1mbit";
				}

				break;
			}
		}

		return nullptr;
	}
} // namespace

namespace RTC
{

	struct KnnNetworkProfileSequence
	{
		std::array<RTC::KnnColdStartNetworkProfile, 4u> profiles{
		  RTC::KnnColdStartNetworkProfile::NORMAL,
		  RTC::KnnColdStartNetworkProfile::NORMAL,
		  RTC::KnnColdStartNetworkProfile::NORMAL,
		  RTC::KnnColdStartNetworkProfile::NORMAL
		};

		size_t count{ 1u };
	};

	static KnnNetworkProfileSequence GetKnnNetworkProfileSequence(
	  RTC::KnnExperiment::NetworkExperimentOption option)
	{
		KnnNetworkProfileSequence sequence;

		switch (option)
		{
			case RTC::KnnExperiment::NetworkExperimentOption::A:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT,
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 2u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::B:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT,
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 2u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::C:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT,
				  RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 3u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::D:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT,
				  RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT,
				  RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT
				};
				sequence.count = 4u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::E:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LOSS_2PCT,
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 2u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::F:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LOSS_5PCT,
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 2u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::G:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LOSS_15PCT,
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::NORMAL
				};
				sequence.count = 2u;
				break;

			case RTC::KnnExperiment::NetworkExperimentOption::H:
				sequence.profiles = {
				  RTC::KnnColdStartNetworkProfile::NORMAL,
				  RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT,
				  RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT,
				  RTC::KnnColdStartNetworkProfile::LOSS_5PCT
				};
				sequence.count = 4u;
				break;
		}

		return sequence;
	}

	static size_t GetKnnNetworkProfileCount(RTC::KnnExperiment::NetworkExperimentOption option)
	{
		return GetKnnNetworkProfileSequence(option).count;
	}

	static RTC::KnnColdStartNetworkProfile GetKnnNetworkProfileAt(
	  RTC::KnnExperiment::NetworkExperimentOption option, size_t index)
	{
		const auto sequence = GetKnnNetworkProfileSequence(option);

		MS_ASSERT(index < sequence.count, "invalid KNN network profile index");

		return sequence.profiles[index];
	}

	static size_t GetKnnNetworkProfileIndex(
	  RTC::KnnExperiment::NetworkExperimentOption option, RTC::KnnColdStartNetworkProfile profile)
	{
		const auto sequence = GetKnnNetworkProfileSequence(option);

		for (size_t index{ 0u }; index < sequence.count; ++index)
		{
			if (sequence.profiles[index] == profile)
			{
				return index;
			}
		}

		MS_ABORT("KNN network profile is not part of selected option");

		return 0u;
	}

	static char KnnNetworkExperimentOptionToChar(RTC::KnnExperiment::NetworkExperimentOption option)
	{
		return static_cast<char>('A' + static_cast<uint8_t>(option));
	}

	static RTC::KnnExperiment::NetworkExperimentOption ParseKnnNetworkExperimentOption()
	{
		const char* value = std::getenv("KNN_NETWORK_OPTION");

		if (!value)
		{
			return RTC::KnnExperiment::NetworkExperimentOption::A;
		}

		const std::string option{ value };

		if (option == "B")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::B;
		}
		if (option == "C")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::C;
		}
		if (option == "D")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::D;
		}
		if (option == "E")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::E;
		}
		if (option == "F")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::F;
		}
		if (option == "G")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::G;
		}
		if (option == "H")
		{
			return RTC::KnnExperiment::NetworkExperimentOption::H;
		}

		return RTC::KnnExperiment::NetworkExperimentOption::A;
	}

	// yeon: layer fec 별 frame size 추정
	static constexpr size_t CandidateLayerCount{ 3u };
	static constexpr size_t FecCandidateCount{ 5u };

	static constexpr std::array<double, CandidateLayerCount> LayerPayloadScale{ 0.10, 0.25, 1.00 };

	// ============================================================
	// Legacy Cold Start.
	//
	// L0 : frame 1 ~ 710
	// L1 : frame 711 ~ 1420
	// L2 : frame 1421 ~ KNN READY
	// ============================================================
	static constexpr size_t KnnColdStartLegacyL0PhaseFrames{ 710u };
	static constexpr size_t KnnColdStartLegacyL1PhaseFrames{ 710u };

	static int16_t GetKnnLegacyColdStartCollectionLayer(size_t coldFrameCount)
	{
		if (coldFrameCount <= KnnColdStartLegacyL0PhaseFrames)
		{
			return 0;
		}

		if (coldFrameCount <= KnnColdStartLegacyL0PhaseFrames + KnnColdStartLegacyL1PhaseFrames)
		{
			return 1;
		}

		return 2;
	}

	static size_t GetKnnLegacyColdStartLayerPhaseFrameIndex(size_t coldFrameCount)
	{
		if (coldFrameCount <= KnnColdStartLegacyL0PhaseFrames)
		{
			return coldFrameCount;
		}

		if (coldFrameCount <= KnnColdStartLegacyL0PhaseFrames + KnnColdStartLegacyL1PhaseFrames)
		{
			return coldFrameCount - KnnColdStartLegacyL0PhaseFrames;
		}

		return coldFrameCount - KnnColdStartLegacyL0PhaseFrames - KnnColdStartLegacyL1PhaseFrames;
	}

	static constexpr std::array<double, FecCandidateCount> FecRedundancyRates{
		0.00, 0.10, 0.25, 0.40, 0.50
	};

	static constexpr std::array<uint8_t, 5> FecProtectionFactors{
		0u,   // 0%
		26u,  // ~10%
		64u,  // ~25%
		102u, // ~40%
		128u  // ~50%
	};

	static constexpr double KnnSlackTargetMs{ 0.0 };

	// Balanced Cold Start logging / CSV helper.
	// Definition is near the cold-start helper implementations below.
	static const char* KnnColdStartNetworkProfileToString(RTC::KnnColdStartNetworkProfile profile);

	static bool GetFecCandidateIndex(uint8_t protectionFactor, size_t& fecIndex)
	{
		switch (protectionFactor)
		{
			case 0u:
			{
				fecIndex = 0u;
				return true;
			}

			case 26u:
			{
				fecIndex = 1u;
				return true;
			}

			case 64u:
			{
				fecIndex = 2u;
				return true;
			}

			case 102u:
			{
				fecIndex = 3u;
				return true;
			}

			case 128u:
			{
				fecIndex = 4u;
				return true;
			}

			default:
			{
				return false;
			}
		}
	}

	struct CandidateMediaFrameSizes
	{
		std::array<uint32_t, CandidateLayerCount> sizeBytes{ 0u, 0u, 0u };

		// true: prefix에서 얻은 actual
		// false: 다른 layer로부터 estimate
		std::array<bool, CandidateLayerCount> actual{ false, false, false };
	};

	struct CandidateActionFrameSizes
	{
		// [layer][fecIndex]
		std::array<std::array<uint32_t, FecCandidateCount>, CandidateLayerCount> sizeBytes{};
	};

	static CandidateMediaFrameSizes BuildCandidateMediaFrameSizes(
	  const RTC::LogicalFrameSizeSnapshot& snapshot)
	{
		CandidateMediaFrameSizes result;

		// 실제 관측값 먼저 복사.
		for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
		{
			if (snapshot.available[layer])
			{
				result.sizeBytes[layer] = snapshot.sizeBytes[layer];
				result.actual[layer]    = true;
			}
		}

		// 추정을 위한 anchor 선택.
		// 가능한 한 높은 layer actual을 사용.
		int anchorLayer{ -1 };

		if (snapshot.available[2])
		{
			anchorLayer = 2;
		}
		else if (snapshot.available[1])
		{
			anchorLayer = 1;
		}
		else if (snapshot.available[0])
		{
			anchorLayer = 0;
		}

		if (anchorLayer < 0)
		{
			return result;
		}

		const double anchorSize = static_cast<double>(snapshot.sizeBytes[anchorLayer]);

		const double anchorScale = LayerPayloadScale[anchorLayer];

		for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
		{
			// actual은 절대 덮어쓰지 않음.
			if (result.actual[layer])
			{
				continue;
			}

			const double estimatedSize = anchorSize * (LayerPayloadScale[layer] / anchorScale);

			result.sizeBytes[layer] = static_cast<uint32_t>(std::max(1.0, std::round(estimatedSize)));
		}

		return result;
	}

	static uint32_t EstimateFrameSizeWithFec(uint32_t payloadSizeBytes, double fecRedundancyRate)
	{
		return static_cast<uint32_t>(
		  std::ceil(static_cast<double>(payloadSizeBytes) * (1.0 + fecRedundancyRate)));
	}

	static CandidateActionFrameSizes BuildCandidateActionFrameSizes(
	  const CandidateMediaFrameSizes& mediaSizes)
	{
		CandidateActionFrameSizes result;

		for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
		{
			for (size_t fecIdx{ 0u }; fecIdx < FecCandidateCount; ++fecIdx)
			{
				result.sizeBytes[layer][fecIdx] =
				  EstimateFrameSizeWithFec(mediaSizes.sizeBytes[layer], FecRedundancyRates[fecIdx]);
			}
		}

		return result;
	}

	static double PredictPacingDelayMs(uint32_t actionFrameSizeBytes, const RTC::PacerSnapshot& pacer)
	{
		if (pacer.tokenRateBytesPerMs <= 0.0)
		{
			return std::numeric_limits<double>::infinity();
		}

		const double requiredBytes = pacer.queuedBytes + static_cast<double>(actionFrameSizeBytes);

		const double deficitBytes = std::max(0.0, requiredBytes - pacer.effectiveTokensBytes);

		return deficitBytes / pacer.tokenRateBytesPerMs;
	}

	static constexpr size_t PacingCandidateCount{ 2u };

	struct CandidateTimingPrediction
	{
		double pacingDelayMs{ 0.0 };
		double predictedOutTimeMs{ 0.0 };
		double predictedSfuTimeMs{ 0.0 };
	};

	struct CandidateActionTimings
	{
		// [layer][fec][pacing]
		// pacing=0 : OFF
		// pacing=1 : ON
		std::array<
		  std::array<std::array<CandidateTimingPrediction, PacingCandidateCount>, FecCandidateCount>,
		  CandidateLayerCount>
		  values{};
	};

	static CandidateActionTimings BuildCandidateActionTimings(
	  const CandidateActionFrameSizes& actionFrameSizes,
	  const RTC::PacerSnapshot& pacer/*,
	  double convertedRtpTimestampMs,
	  uint64_t nowMs*/)
	{
		CandidateActionTimings result;
		// const double now = static_cast<double>(nowMs);

		for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
		{
			for (size_t fecIdx{ 0u }; fecIdx < FecCandidateCount; ++fecIdx)
			{
				const uint32_t frameSizeBytes = actionFrameSizes.sizeBytes[layer][fecIdx];

				// Pacing OFF
				{
					auto& timing = result.values[layer][fecIdx][0];

					timing.pacingDelayMs = 0.0;

					// V1에서는 별도 processing delay를 0으로 둔다.
					// timing.predictedOutTimeMs = now;

					// timing.predictedSfuTimeMs = convertedRtpTimestampMs - timing.predictedOutTimeMs;
				}

				// Pacing ON
				{
					auto& timing = result.values[layer][fecIdx][1];

					timing.pacingDelayMs = PredictPacingDelayMs(frameSizeBytes, pacer);
					// timing.predictedOutTimeMs = now + timing.pacingDelayMs;
					// timing.predictedSfuTimeMs = convertedRtpTimestampMs - timing.predictedOutTimeMs;
				}
			}
		}
		return result;
	}

	struct ActionCandidate
	{
		size_t layer{ 0u };
		size_t fecIndex{ 0u };
		size_t pacingIndex{ 0u };

		uint32_t frameSizeBytes{ 0u };
		double pacingDelayMs{ 0.0 };

		RTC::SlackFeature feature;

		RTC::SlackActionIdentity action;

		bool hasPrediction{ false };
		double predictedDecodeSlackMs{ 0.0 };
		double predictedDeadlineMissProbability{ 0.0 };

		// ========================================================
		// yeon: 해당 candidate prediction에 실제 사용된 neighbors.
		// ========================================================
		size_t predictorSampleCount{ 0u };

		size_t debugNeighborCount{ 0u };

		std::array<RTC::KnnNeighborDebugInfo, RTC::KnnDebugNeighborCount> debugNeighbors{};
	};

	using ActionCandidateList =
	  std::array<ActionCandidate, CandidateLayerCount * FecCandidateCount * PacingCandidateCount>;

	static bool IsBetterSafeAction(const ActionCandidate& candidate, const ActionCandidate& best)
	{
		// 1. 영상 품질: 높은 spatial layer 우선.
		if (candidate.layer != best.layer)
		{
			return candidate.layer > best.layer;
		}

		// 2. 같은 화질이라면 deadline budget이 허용하는 범위에서
		//    높은 FEC 보호율 우선.
		if (candidate.fecIndex != best.fecIndex)
		{
			return candidate.fecIndex > best.fecIndex;
		}

		// 3. 그것도 같다면 pacing ON 우선.
		return candidate.pacingIndex > best.pacingIndex;
	}

	static bool IsBetterKnnAction(const ActionCandidate& candidate, const ActionCandidate& currentBest)
	{
		constexpr double Epsilon{ 1e-9 };

		if (candidate.predictedDeadlineMissProbability < currentBest.predictedDeadlineMissProbability - Epsilon)
		{
			return true;
		}

		if (candidate.predictedDeadlineMissProbability > currentBest.predictedDeadlineMissProbability + Epsilon)
		{
			return false;
		}

		if (candidate.predictedDecodeSlackMs > currentBest.predictedDecodeSlackMs + Epsilon)
		{
			return true;
		}

		if (candidate.predictedDecodeSlackMs < currentBest.predictedDecodeSlackMs - Epsilon)
		{
			return false;
		}

		return IsBetterSafeAction(candidate, currentBest);
	}
	// 이제 slack 최대값을 선택하는게 아니라, 다음 규칙 적용:
	// 1순위 Deadline miss probability 최소
	// 2순위 Predicted decode Slack
	static const ActionCandidate* SelectBestKnnAction(const ActionCandidateList& candidates)
	{
		const ActionCandidate* best{ nullptr };

		for (const auto& candidate : candidates)
		{
			if (!candidate.hasPrediction)
			{
				continue;
			}

			if (!best || IsBetterKnnAction(candidate, *best))
			{
				best = &candidate;
			}
		}

		return best;
	}

	static const ActionCandidate* SelectBestKnnActionForLayer(
	  const ActionCandidateList& candidates, size_t fixedLayer)
	{
		const ActionCandidate* best{ nullptr };

		for (const auto& candidate : candidates)
		{
			if (candidate.layer != fixedLayer)
			{
				continue;
			}

			if (!candidate.hasPrediction)
			{
				continue;
			}

			if (!best || IsBetterKnnAction(candidate, *best))
			{
				best = &candidate;
			}
		}

		return best;
	}

	/* Static. */

	static constexpr uint64_t StreamMinActiveMs{ 2000u };
	static constexpr uint64_t BweDowngradeConservativeMs{ 10000u };
	static constexpr uint64_t BweDowngradeMinActiveMs{ 8000u };
	static constexpr uint16_t MaxSequenceNumberGap{ 100u };
	static constexpr size_t TargetLayerRetransmissionBufferSize{ 30u };

	struct Vp8DescInfo
	{
		bool hasPictureId{ false };
		uint16_t pictureId{ 0 };

		bool hasTl0PicIdx{ false };
		uint8_t tl0PicIdx{ 0 };

		bool hasTid{ false };
		uint8_t tid{ 0 }; // 0..3
	};

	// Returns true if parsed something meaningful. Safe for short payloads.
	static bool ParseVp8PayloadDescriptor(const uint8_t* payload, size_t len, Vp8DescInfo& out)
	{
		if (!payload || len < 1)
		{
			return false;
		}

		size_t i         = 0;
		const uint8_t b0 = payload[i++];

		const bool X = (b0 & 0x80) != 0; // extension present
		if (!X)
		{
			// No extension => no PictureID/TL0PICIDX/TID in this minimal parser.
			return true;
		}

		if (i >= len)
		{
			return false;
		}
		const uint8_t x1 = payload[i++];

		const bool I = (x1 & 0x80) != 0;
		const bool L = (x1 & 0x40) != 0;
		const bool T = (x1 & 0x20) != 0;
		const bool K = (x1 & 0x10) != 0;

		// PictureID (I)
		if (I)
		{
			if (i >= len)
			{
				return false;
			}
			uint8_t pic = payload[i++];

			if (pic & 0x80)
			{
				// 16-bit PictureID (15 bits used)
				if (i >= len)
				{
					return false;
				}
				uint8_t pic2     = payload[i++];
				out.hasPictureId = true;
				out.pictureId    = static_cast<uint16_t>(((pic & 0x7F) << 8) | pic2);
			}
			else
			{
				// 8-bit PictureID
				out.hasPictureId = true;
				out.pictureId    = pic;
			}
		}

		// TL0PICIDX (L)
		if (L)
		{
			if (i >= len)
			{
				return false;
			}
			out.hasTl0PicIdx = true;
			out.tl0PicIdx    = payload[i++];
		}

		// TID/KEYIDX byte exists if T or K is set.
		if (T || K)
		{
			if (i >= len)
			{
				return false;
			}
			const uint8_t tk = payload[i++];

			if (T)
			{
				out.hasTid = true;
				out.tid    = static_cast<uint8_t>((tk >> 6) & 0x03);
			}
		}

		return true;
	}

	static int FindEncodingIdxBySsrc(const RTC::RtpParameters& rtpParameters, uint32_t ssrc)
	{
		for (size_t i = 0; i < rtpParameters.encodings.size(); ++i)
		{
			const auto& enc = rtpParameters.encodings[i];
			if (enc.ssrc == ssrc || enc.rtx.ssrc == ssrc)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	/* Instance methods. */

	SimulcastConsumer::SimulcastConsumer(
	  RTC::Shared* shared,
	  const std::string& id,
	  const std::string& producerId,
	  RTC::Consumer::Listener* listener,
	  const FBS::Transport::ConsumeRequest* data)
	  : RTC::Consumer::Consumer(
	      shared, id, producerId, listener, data, RTC::RtpParameters::Type::SIMULCAST)
	{
		MS_TRACE();

		this->knnNetworkExperimentOption = ParseKnnNetworkExperimentOption();
		this->knnNetworkProfileCount     = GetKnnNetworkProfileCount(this->knnNetworkExperimentOption);
		this->knnColdStartSamplesPerLayer =
		  KnnColdStartSamplesPerNetworkPhase * this->knnNetworkProfileCount;
		this->knnColdStartRequiredTotalSamples = this->knnColdStartSamplesPerLayer * 3u;

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-NETWORK-EXPERIMENT-CONFIG] consumer:%s option:%c profiles:%zu samplesPerLayer:%zu totalSamples:%zu",
		  this->id.c_str(),
		  KnnNetworkExperimentOptionToChar(this->knnNetworkExperimentOption),
		  this->knnNetworkProfileCount,
		  this->knnColdStartSamplesPerLayer,
		  this->knnColdStartRequiredTotalSamples);

		// We allow a single encoding in simulcast (so we can enable temporal layers
		// with a single simulcast stream).
		// NOTE: No need to check this->consumableRtpEncodings.size() > 0 here since
		// it's already done in Consumer constructor.

		auto& encoding = this->rtpParameters.encodings[0];

		// Ensure there are as many spatial layers as encodings.
		if (encoding.spatialLayers != this->consumableRtpEncodings.size())
		{
			MS_THROW_TYPE_ERROR("encoding.spatialLayers does not match number of consumableRtpEncodings");
		}

		// Fill mapMappedSsrcSpatialLayer.
		for (size_t idx{ 0u }; idx < this->consumableRtpEncodings.size(); ++idx)
		{
			auto& encoding = this->consumableRtpEncodings[idx];

			this->mapMappedSsrcSpatialLayer[encoding.ssrc] = static_cast<int16_t>(idx);
		}

		// Set preferredLayers (if given).
		if (flatbuffers::IsFieldPresent(data, FBS::Transport::ConsumeRequest::VT_PREFERREDLAYERS))
		{
			const auto* preferredLayers = data->preferredLayers();

			this->preferredLayers.spatial = preferredLayers->spatialLayer();

			if (this->preferredLayers.spatial > encoding.spatialLayers - 1)
			{
				this->preferredLayers.spatial = static_cast<int16_t>(encoding.spatialLayers - 1);
			}

			if (auto preferredTemporalLayer = preferredLayers->temporalLayer(); preferredTemporalLayer.has_value())
			{
				this->preferredLayers.temporal = preferredTemporalLayer.value();

				if (this->preferredLayers.temporal > encoding.temporalLayers - 1)
				{
					this->preferredLayers.temporal = static_cast<int16_t>(encoding.temporalLayers - 1);
				}
			}
			else
			{
				this->preferredLayers.temporal = static_cast<int16_t>(encoding.temporalLayers - 1);
			}
		}
		else
		{
			// Initially set preferredSpatialLayer and preferredTemporalLayer to the
			// maximum value.
			this->preferredLayers.spatial  = static_cast<int16_t>(encoding.spatialLayers - 1);
			this->preferredLayers.temporal = static_cast<int16_t>(encoding.temporalLayers - 1);
		}

		// Reserve space for the Producer RTP streams by filling all the possible
		// entries with nullptr.
		this->producerRtpStreams.insert(
		  this->producerRtpStreams.begin(), this->consumableRtpEncodings.size(), nullptr);

		// Create the encoding context.
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		if (!RTC::Codecs::Tools::IsValidTypeForCodec(this->type, mediaCodec->mimeType))
		{
			MS_THROW_TYPE_ERROR(
			  "%s codec not supported for simulcast", mediaCodec->mimeType.ToString().c_str());
		}

		// Let's chosee an initial output seq number between 1000 and 32768 to avoid
		// libsrtp bug:
		// https://github.com/versatica/mediasoup/issues/1437
		const uint16_t initialOutputSeq =
		  Utils::Crypto::GetRandomUInt(1000u, std::numeric_limits<uint16_t>::max() / 2);

		this->rtpSeqManager = RTC::SeqManager<uint16_t>(initialOutputSeq);

		RTC::Codecs::EncodingContext::Params params;

		params.spatialLayers  = encoding.spatialLayers;
		params.temporalLayers = encoding.temporalLayers;

		this->encodingContext.reset(RTC::Codecs::Tools::GetEncodingContext(mediaCodec->mimeType, params));

		MS_ASSERT(this->encodingContext, "no encoding context for this codec");

		// Create RtpStreamSend instance for sending a single stream to the remote.
		CreateRtpStream();

		// NOTE: This may throw.
		this->shared->channelMessageRegistrator->RegisterHandler(
		  this->id,
		  /*channelRequestHandler*/ this,
		  /*channelRequestHandler*/ nullptr);
	}

	SimulcastConsumer::~SimulcastConsumer()
	{
		MS_TRACE();

		this->shared->channelMessageRegistrator->UnregisterHandler(this->id);

		delete this->rtpStream;
		this->targetLayerRetransmissionBuffer.clear();
	}

	flatbuffers::Offset<FBS::Consumer::DumpResponse> SimulcastConsumer::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Call the parent method.
		auto base = RTC::Consumer::FillBuffer(builder);
		// Add rtpStream.
		std::vector<flatbuffers::Offset<FBS::RtpStream::Dump>> rtpStreams;
		rtpStreams.emplace_back(this->rtpStream->FillBuffer(builder));

		auto dump = FBS::Consumer::CreateConsumerDumpDirect(
		  builder,
		  base,
		  &rtpStreams,
		  this->preferredLayers.spatial,
		  this->targetLayers.spatial,
		  this->currentSpatialLayer,
		  this->preferredLayers.temporal,
		  this->targetLayers.temporal,
		  this->encodingContext->GetCurrentTemporalLayer());

		return FBS::Consumer::CreateDumpResponse(builder, dump);
	}

	flatbuffers::Offset<FBS::Consumer::GetStatsResponse> SimulcastConsumer::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		std::vector<flatbuffers::Offset<FBS::RtpStream::Stats>> rtpStreams;

		// Add stats of our send stream.
		rtpStreams.emplace_back(this->rtpStream->FillBufferStats(builder));

		auto* producerCurrentRtpStream = GetProducerCurrentRtpStream();

		// Add stats of our recv stream.
		if (producerCurrentRtpStream)
		{
			rtpStreams.emplace_back(producerCurrentRtpStream->FillBufferStats(builder));
		}

		return FBS::Consumer::CreateGetStatsResponseDirect(builder, &rtpStreams);
	}

	flatbuffers::Offset<FBS::Consumer::ConsumerScore> SimulcastConsumer::FillBufferScore(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		MS_ASSERT(this->producerRtpStreamScores, "producerRtpStreamScores not set");

		auto* producerCurrentRtpStream = GetProducerCurrentRtpStream();

		uint8_t producerScore{ 0 };

		if (producerCurrentRtpStream)
		{
			producerScore = producerCurrentRtpStream->GetScore();
		}
		else
		{
			producerScore = 0;
		}

		return FBS::Consumer::CreateConsumerScoreDirect(
		  builder, this->rtpStream->GetScore(), producerScore, this->producerRtpStreamScores);
	}

	void SimulcastConsumer::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::CONSUMER_DUMP:
			{
				auto dumpOffset = FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Consumer_DumpResponse, dumpOffset);

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_REQUEST_KEY_FRAME:
			{
				if (IsActive())
				{
					RequestKeyFrames();
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_SET_PREFERRED_LAYERS:
			{
				auto previousPreferredLayers = this->preferredLayers;

				const auto* body = request->data->body_as<FBS::Consumer::SetPreferredLayersRequest>();
				const auto* preferredLayers = body->preferredLayers();

				// Spatial layer.
				this->preferredLayers.spatial = preferredLayers->spatialLayer();

				if (this->preferredLayers.spatial > this->rtpStream->GetSpatialLayers() - 1)
				{
					this->preferredLayers.spatial =
					  static_cast<int16_t>(this->rtpStream->GetSpatialLayers() - 1);
				}

				// preferredTemporaLayer is optional.
				auto preferredTemporalLayer = preferredLayers->temporalLayer();

				if (preferredTemporalLayer.has_value())
				{
					this->preferredLayers.temporal = preferredTemporalLayer.value();

					if (this->preferredLayers.temporal > this->rtpStream->GetTemporalLayers() - 1)
					{
						this->preferredLayers.temporal =
						  static_cast<int16_t>(this->rtpStream->GetTemporalLayers() - 1);
					}
				}
				else
				{
					this->preferredLayers.temporal =
					  static_cast<int16_t>(this->rtpStream->GetTemporalLayers() - 1);
				}

				MS_DEBUG_DEV(
				  "preferred layers changed [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
				  this->preferredLayers.spatial,
				  this->preferredLayers.temporal,
				  this->id.c_str());

				preferredTemporalLayer     = this->preferredLayers.temporal;
				auto preferredLayersOffset = FBS::Consumer::CreateConsumerLayers(
				  request->GetBufferBuilder(), this->preferredLayers.spatial, preferredTemporalLayer);
				auto responseOffset = FBS::Consumer::CreateSetPreferredLayersResponse(
				  request->GetBufferBuilder(), preferredLayersOffset);

				request->Accept(FBS::Response::Body::Consumer_SetPreferredLayersResponse, responseOffset);

				if (IsActive() && this->preferredLayers != previousPreferredLayers)
				{
					MayChangeLayers(/*force*/ true);
				}

				break;
			}

			default:
			{
				// Pass it to the parent class.
				RTC::Consumer::HandleRequest(request);
			}
		}
	}

	void SimulcastConsumer::ProducerRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc)
	{
		MS_TRACE();

		auto it = this->mapMappedSsrcSpatialLayer.find(mappedSsrc);

		MS_ASSERT(it != this->mapMappedSsrcSpatialLayer.end(), "unknown mappedSsrc");

		const int16_t spatialLayer = it->second;

		this->producerRtpStreams[spatialLayer] = rtpStream;
	}

	void SimulcastConsumer::ProducerNewRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc)
	{
		MS_TRACE();

		auto it = this->mapMappedSsrcSpatialLayer.find(mappedSsrc);

		MS_ASSERT(it != this->mapMappedSsrcSpatialLayer.end(), "unknown mappedSsrc");

		const int16_t spatialLayer = it->second;

		this->producerRtpStreams[spatialLayer] = rtpStream;

		// Emit the score event.
		EmitScore();

		if (IsActive())
		{
			MayChangeLayers();
		}
	}

	void SimulcastConsumer::ProducerRtpStreamScore(
	  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t score, uint8_t previousScore)
	{
		MS_TRACE();

		// Emit the score event.
		EmitScore();

		if (RTC::Consumer::IsActive())
		{
			// All Producer streams are dead.
			if (!IsActive())
			{
				UpdateTargetLayers(-1, -1);
			}
			// Just check target layers if the stream has died or reborned.
			// clang-format off
			else if (
				!this->externallyManagedBitrate ||
				(score == 0u || previousScore == 0u)
			)
			// clang-format on
			{
				MayChangeLayers();
			}
		}
	}

	void SimulcastConsumer::ProducerRtcpSenderReport(RTC::RtpStreamRecv* rtpStream, bool first)
	{
		MS_TRACE();

		// Just interested if this is the first Sender Report for a RTP stream.
		if (!first)
		{
			return;
		}

		MS_DEBUG_TAG(simulcast, "first SenderReport [ssrc:%" PRIu32 "]", rtpStream->GetSsrc());

		// If our RTP timestamp reference stream does not yet have SR, do nothing
		// since we know we won't be able to switch.
		auto* producerTsReferenceRtpStream = GetProducerTsReferenceRtpStream();

		if (!producerTsReferenceRtpStream || !producerTsReferenceRtpStream->GetSenderReportNtpMs())
		{
			return;
		}

		if (IsActive())
		{
			MayChangeLayers();
		}
	}

	uint8_t SimulcastConsumer::GetBitratePriority() const
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		if (!IsActive())
		{
			return 0u;
		}

		return this->priority;
	}

	uint32_t SimulcastConsumer::IncreaseLayer(uint32_t bitrate, bool considerLoss)
	{
		MS_TRACE();

		// yeon: slack 기반 layering
		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");
		MS_ASSERT(IsActive(), "should be active");

		// Preferred spatial layer에 slack 기반 cap을 반영한다.
		// slackMaxSpatialLayer == -1 이면 제한 없음.
		int16_t effectivePreferredSpatialLayer = this->preferredLayers.spatial;

		int16_t controllerMaxSpatialLayer{ -1 };

		if (this->knnControlActive && this->knnMaxSpatialLayer >= 0)
		{
			// KNN mode.
			controllerMaxSpatialLayer = this->knnMaxSpatialLayer;
		}
		else if (!this->knnColdStartCollectionComplete)
		{
			// Initial Cold Start에서만 collection layer를 cap으로 사용.
			controllerMaxSpatialLayer = this->knnColdStartCollectionLayer;
		}
		else
		{
			// Cold Start는 이미 완료되었지만 아직 유효 KNN cap 없음.
			// 절대로 cold-start layer cap을 다시 적용하지 않는다.
			// 기존 mediasoup BWE allocator에게 맡긴다.
			controllerMaxSpatialLayer = -1;
		}

		if (controllerMaxSpatialLayer >= 0)
		{
			if (effectivePreferredSpatialLayer < 0)
			{
				effectivePreferredSpatialLayer = controllerMaxSpatialLayer;
			}
			else
			{
				effectivePreferredSpatialLayer =
				  std::min<int16_t>(effectivePreferredSpatialLayer, controllerMaxSpatialLayer);
			}
		}

		// If already in the effective preferred layers, do nothing.
		if (
		  this->provisionalTargetLayers.spatial == effectivePreferredSpatialLayer &&
		  this->provisionalTargetLayers.temporal == this->preferredLayers.temporal)
		{
			return 0u;
		}

		// Slack cap이 현재 provisional target보다 낮으면,
		// 여기서는 layer를 increase하지 말고 provisional target을 즉시 낮춘다.
		// 이 반환값은 budget을 소모하지 않으므로 0을 반환한다.
		if (effectivePreferredSpatialLayer >= 0 && this->provisionalTargetLayers.spatial > effectivePreferredSpatialLayer)
		{
			MS_WARN_TAG(
			  bwe,
			  "forcing provisional spatial layer down due to controller cap "
			  "[provisional:%" PRIi16 ", effectivePreferred:%" PRIi16 ", preferred:%" PRIi16
			  ", controllerCap:%" PRIi16 "]",
			  this->provisionalTargetLayers.spatial,
			  effectivePreferredSpatialLayer,
			  this->preferredLayers.spatial,
			  controllerMaxSpatialLayer);

			this->provisionalTargetLayers.spatial  = effectivePreferredSpatialLayer;
			this->provisionalTargetLayers.temporal = 0;

			return 0u;
		}
		// ...... //

		uint32_t virtualBitrate;

		if (considerLoss)
		{
			// Calculate virtual available bitrate based on given bitrate and our
			// packet lost.
			auto lossPercentage = this->rtpStream->GetLossPercentage();

			if (lossPercentage < 2)
			{
				virtualBitrate = 1.08 * bitrate;
			}
			else if (lossPercentage > 10)
			{
				virtualBitrate = (1 - 0.5 * (lossPercentage / 100)) * bitrate;
			}
			else
			{
				virtualBitrate = bitrate;
			}
		}
		else
		{
			virtualBitrate = bitrate;
		}

		uint32_t requiredBitrate{ 0u };
		int16_t spatialLayer{ 0 };
		int16_t temporalLayer{ 0 };
		auto nowMs = DepLibUV::GetTimeMs();

		for (size_t sIdx{ 0u }; sIdx < this->producerRtpStreams.size(); ++sIdx)
		{
			spatialLayer = static_cast<int16_t>(sIdx);

			// If this is higher than current spatial layer and we moved to to current
			// spatial layer due to BWE limitations, check how much it has elapsed
			// since then.
			if (nowMs - this->lastBweDowngradeAtMs < BweDowngradeConservativeMs)
			{
				if (this->provisionalTargetLayers.spatial > -1 && spatialLayer > this->currentSpatialLayer)
				{
					MS_DEBUG_DEV(
					  "avoid upgrading to spatial layer %" PRIi16 " due to recent BWE downgrade", spatialLayer);

					goto done;
				}
			}

			// Ignore spatial layers lower than the one we already have.
			if (spatialLayer < this->provisionalTargetLayers.spatial)
			{
				continue;
			}
			// If this is the higher than preferred spatial layer, abort.
			// else if (spatialLayer > this->preferredLayers.spatial)
			// {
			// 	MS_DEBUG_DEV(
			// 	  "avoid upgrading to spatial layer %" PRIi16
			// 	  " since it's higher than preferred spatial layer %" PRIi16,
			// 	  spatialLayer,
			// 	  this->preferredLayers.spatial);

			// 	goto done;
			// }

			// yeon: slack 기반 layering
			// If this is higher than effective preferred spatial layer, abort.
			// effectivePreferredSpatialLayer는 preferredLayers.spatial에 slack cap을 반영한 값이다.
			else if (effectivePreferredSpatialLayer >= 0 && spatialLayer > effectivePreferredSpatialLayer)
			{
				MS_DEBUG_DEV(
				  "avoid upgrading to spatial layer %" PRIi16
				  " since it's higher than effective preferred spatial layer %" PRIi16
				  " [preferred:%" PRIi16 ", slackCap:%" PRIi8 "]",
				  spatialLayer,
				  effectivePreferredSpatialLayer,
				  this->preferredLayers.spatial,
				  controllerMaxSpatialLayer);

				goto done;
			}

			// This can be null.
			auto* producerRtpStream = this->producerRtpStreams.at(spatialLayer);

			// Producer stream does not exist. Ignore.
			if (!producerRtpStream)
			{
				continue;
			}

			// Ignore spatial layers (streams) with score 0.
			if (producerRtpStream->GetScore() == 0)
			{
				continue;
			}

			// If the stream has not been active time enough and we have an active one
			// already, move to the next spatial layer.
			// clang-format off
			if (
				spatialLayer != this->provisionalTargetLayers.spatial &&
				this->provisionalTargetLayers.spatial != -1 &&
				producerRtpStream->GetActiveMs() < StreamMinActiveMs
			)
			// clang-format on
			{
				const auto* provisionalProducerRtpStream =
				  this->producerRtpStreams.at(this->provisionalTargetLayers.spatial);

				// The stream for the current provisional spatial layer has been active
				// for enough time, move to the next spatial layer.
				if (provisionalProducerRtpStream->GetActiveMs() >= StreamMinActiveMs)
				{
					continue;
				}
			}

			// We may not yet switch to this spatial layer.
			if (!CanSwitchToSpatialLayer(spatialLayer))
			{
				continue;
			}

			temporalLayer = 0;

			// Check bitrate of every temporal layer.
			for (; temporalLayer < producerRtpStream->GetTemporalLayers(); ++temporalLayer)
			{
				// Ignore temporal layers lower than the one we already have (taking
				// into account the spatial layer too).
				// clang-format off
				if (
					spatialLayer == this->provisionalTargetLayers.spatial &&
					temporalLayer <= this->provisionalTargetLayers.temporal
				)
				// clang-format on
				{
					continue;
				}

				requiredBitrate = producerRtpStream->GetLayerBitrate(nowMs, 0, temporalLayer);

				// This is simulcast so we must substract the bitrate of the current
				// temporal spatial layer if this is the temporal layer 0 of a higher
				// spatial layer.
				//
				// clang-format off
				if (
					requiredBitrate &&
					temporalLayer == 0 &&
					this->provisionalTargetLayers.spatial > -1 &&
					spatialLayer > this->provisionalTargetLayers.spatial
				)
				// clang-format on
				{
					auto* provisionalProducerRtpStream =
					  this->producerRtpStreams.at(this->provisionalTargetLayers.spatial);
					auto provisionalRequiredBitrate = provisionalProducerRtpStream->GetBitrate(
					  nowMs, 0, this->provisionalTargetLayers.temporal);

					if (requiredBitrate > provisionalRequiredBitrate)
					{
						requiredBitrate -= provisionalRequiredBitrate;
					}
					else
					{
						requiredBitrate = 1u; // Don't set 0 since it would be ignored.
					}
				}

				MS_DEBUG_DEV(
				  "testing layers %" PRIi16 ":%" PRIi16 " [virtual bitrate:%" PRIu32
				  ", required bitrate:%" PRIu32 "]",
				  spatialLayer,
				  temporalLayer,
				  virtualBitrate,
				  requiredBitrate);

				// If active layer, end iterations here. Otherwise move to next spatial
				// layer.
				if (requiredBitrate)
				{
					goto done;
				}
				else
				{
					break;
				}
			}

			// If this is the preferred spatial layer or higher, take it and exit.
			// if (spatialLayer >= this->preferredLayers.spatial)
			// {
			// 	break;
			// }

			// yeon: slack 기반 layering
			// If this is the effective preferred spatial layer or higher, take it and exit.
			if (effectivePreferredSpatialLayer >= 0 && spatialLayer >= effectivePreferredSpatialLayer)
			{
				break;
			}
		}

	done:

		// No higher active layers found.
		if (!requiredBitrate)
		{
			return 0u;
		}

		// No luck.
		if (requiredBitrate > virtualBitrate)
		{
			return 0u;
		}

		// Set provisional layers.
		this->provisionalTargetLayers.spatial  = spatialLayer;
		this->provisionalTargetLayers.temporal = temporalLayer;

		MS_DEBUG_DEV(
		  "setting provisional layers to %" PRIi16 ":%" PRIi16 " [virtual bitrate:%" PRIu32
		  ", required bitrate:%" PRIu32 "]",
		  this->provisionalTargetLayers.spatial,
		  this->provisionalTargetLayers.temporal,
		  virtualBitrate,
		  requiredBitrate);

		if (requiredBitrate <= bitrate)
		{
			return requiredBitrate;
		}
		else if (requiredBitrate <= virtualBitrate)
		{
			return bitrate;
		}
		else
		{
			return requiredBitrate; // NOTE: This cannot happen.
		}
	}

	void SimulcastConsumer::ApplyLayers()
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");
		MS_ASSERT(IsActive(), "should be active");

		// ============================================================
		// yeon: [DIRECT LAYER CONTROL - DISABLED]
		//
		// direct mode에서는 kNN이 UpdateTargetLayers()를 직접 호출하므로
		// BWE allocator의 provisional target을 무시했다.
		//
		// CAP mode에서는 BWE allocator가 실제 target을 결정해야 하므로
		// 이 early-return을 비활성화한다.
		// ============================================================
		/*
		if (this->knnControlActive)
		{
		    this->provisionalTargetLayers.Reset();
		    return;
		}
		*/

		auto provisionalTargetLayers = this->provisionalTargetLayers;

		// Reset provisional target layers.
		this->provisionalTargetLayers.Reset();

		if (provisionalTargetLayers != this->targetLayers)
		{
			UpdateTargetLayers(provisionalTargetLayers.spatial, provisionalTargetLayers.temporal);

			// If this looks like a spatial layer downgrade due to BWE limitations, set member.
			// clang-format off
			if (
				this->rtpStream->GetActiveMs() > BweDowngradeMinActiveMs &&
				this->targetLayers.spatial < this->currentSpatialLayer &&
				this->currentSpatialLayer <= this->preferredLayers.spatial
			)
			// clang-format on
			{
				MS_DEBUG_DEV(
				  "possible target spatial layer downgrade (from %" PRIi16 " to %" PRIi16
				  ") due to BWE limitation",
				  this->currentSpatialLayer,
				  this->targetLayers.spatial);

				this->lastBweDowngradeAtMs = DepLibUV::GetTimeMs();
			}
		}
	}

	uint32_t SimulcastConsumer::GetDesiredBitrate() const
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		if (!IsActive())
		{
			return 0u;
		}

		auto nowMs = DepLibUV::GetTimeMs();
		uint32_t desiredBitrate{ 0u };

		// Let's iterate all streams of the Producer (from highest to lowest) and
		// obtain their bitrate. Choose the highest one.
		// NOTE: When the Producer enables a higher stream, initially the bitrate of
		// it could be less than the bitrate of a lower stream. That's why we
		// iterate all streams here anyway.
		for (auto sIdx{ static_cast<int16_t>(this->producerRtpStreams.size() - 1) }; sIdx >= 0; --sIdx)
		{
			auto* producerRtpStream = this->producerRtpStreams.at(sIdx);

			if (!producerRtpStream)
			{
				continue;
			}

			auto streamBitrate = producerRtpStream->GetBitrate(nowMs);

			desiredBitrate = std::max(streamBitrate, desiredBitrate);
		}

		// If consumer.rtpParameters.encodings[0].maxBitrate was given and it's
		// greater than computed one, then use it.
		auto maxBitrate = this->rtpParameters.encodings[0].maxBitrate;

		desiredBitrate = std::max(maxBitrate, desiredBitrate);

		return desiredBitrate;
	}

	bool SimulcastConsumer::HasProcessedSlackDecisionLogicalFrame(uint32_t logicalFrameId) const
	{
		return this->processedSlackDecisionLogicalFrameIds.find(logicalFrameId) !=
		       this->processedSlackDecisionLogicalFrameIds.end();
	}

	void SimulcastConsumer::RememberProcessedSlackDecisionLogicalFrame(uint32_t logicalFrameId)
	{
		// 이미 존재한다면 아무것도 하지 않는다.
		const auto result = this->processedSlackDecisionLogicalFrameIds.insert(logicalFrameId);

		if (!result.second)
		{
			return;
		}

		// 처리 순서를 같이 저장
		this->processedSlackDecisionLogicalFrameOrder.push_back(logicalFrameId);

		// Cache 크기를 일정하게 유지.
		// 모든 과거 frame을 기억할 필요는 없긴 함
		// late packet / 다른 simulcast layer / RTX에 의해
		// 비교적 최근 logicalFrameId가 다시 들어오는 것을 막기 위한 코드
		while (this->processedSlackDecisionLogicalFrameOrder.size() > SlackDecisionLogicalFrameCacheSize)
		{
			const uint32_t oldLogicalFrameId = this->processedSlackDecisionLogicalFrameOrder.front();

			this->processedSlackDecisionLogicalFrameOrder.pop_front();

			this->processedSlackDecisionLogicalFrameIds.erase(oldLogicalFrameId);
		}
	}

	// NOLINTNEXTLINE (misc-no-recursion)
	void SimulcastConsumer::SendRtpPacket(RTC::RtpPacket* packet, RTC::SharedRtpPacket& sharedPacket)
	{
		MS_TRACE();

		// custom metadata가 존재하는 frame 시작 packet에서만
		// action decision을 수행한다.
		if (packet && packet->HasFrameMeta())
		{
			// 1. logical frame ID
			const uint32_t logicalFrameId = packet->GetFrameMetaLogicalFrameId();

			const bool alreadyProcessed = this->HasProcessedSlackDecisionLogicalFrame(logicalFrameId);

			// 각 프레임에서 처음으로 들어온 레이어의 패킷인지 확인하고 그 때만 데이터 수집, 로그 출력
			if (!alreadyProcessed)
			{
				// decision 시각.
				// 모든 후보가 동일한 now를 사용
				const uint64_t nowMs = DepLibUV::GetTimeMs();

				// 2. 현재 logical frame의 L0/L1/L2 encoded size
				RTC::LogicalFrameSizeSnapshot frameSizeSnapshot;

				const bool hasFrameSizes = RTC::LogicalFrameSizeRegistry::Instance().Get(
				  this->producerId, logicalFrameId, frameSizeSnapshot);

				CandidateMediaFrameSizes mediaSizes;
				CandidateActionFrameSizes actionFrameSizes;

				if (hasFrameSizes)
				{
					mediaSizes       = BuildCandidateMediaFrameSizes(frameSizeSnapshot);
					actionFrameSizes = BuildCandidateActionFrameSizes(mediaSizes);
					// 이후 controller에서 사용.
				}

				// 3. 해당 viewer의 현재 NetworkState
				RTC::NetworkSnapshot networkSnapshot;
				const bool hasNetworkState =
				  this->listener->OnConsumerGetNetworkSnapshot(this, networkSnapshot);

				// 3.5. 현재 Pacer State

				RTC::PacerSnapshot pacerSnapshot;
				const bool hasPacerState =
				  this->listener->OnConsumerGetPacerSnapshot(this, nowMs, pacerSnapshot);

				// 4. logical RTP timestamp -> SFU clock으로 변환한 ConvertedRtpTimestamp 계산
				// 추후 SFUTime_hat(A)  = ConvertedRtpTimestamp - predictedOutTime(A) 계산
				// SFUTime_hat(A)  = ConvertedRtpTimestamp - (now + pacing delay)
				// 일단 pacing delay만 state로 쓰기로 결정하여 주석 처리함
				// double convertedRtpTimestampMs{ 0.0 };

				// const bool hasConvertedRtpTimestamp =
				//   RTC::LogicalFrameClockMapper::Instance().ConvertToSfuTimeMs(
				//     this->producerId, logicalFrameId, convertedRtpTimestampMs);

				// 모두 준비된 경우 30개 action timing 계산
				if (hasFrameSizes && hasNetworkState && hasPacerState /*&& hasConvertedRtpTimestamp*/)
				{
					// 이 logical frame에 대해 실제 decision 처리를 시작했으므로 처리 완료 대상으로 표시.
					this->RememberProcessedSlackDecisionLogicalFrame(logicalFrameId);

					const auto actionTimings = BuildCandidateActionTimings(
					  actionFrameSizes, pacerSnapshot /*, convertedRtpTimestampMs, nowMs*/);
					// 위 과정을 통해 Layer 3 x FEC 5 x Pacing 2 = 30개의 FrameSize와 pacing delay가 모두 준비됨

					// ============================================================
					// 1. 현재 decision 시점의 pacer 상태 출력
					// ============================================================
					// std::cout << std::fixed << std::setprecision(3) << "[PACER-STATE]"
					//           << " consumer=" << this->id << " logical=" << logicalFrameId
					//           << " nowMs=" << nowMs << " queueBytes=" << pacerSnapshot.queuedBytes
					//           << " effectiveTokensBytes=" << pacerSnapshot.effectiveTokensBytes
					//           << " bucketCapacityBytes=" << pacerSnapshot.bucketCapacityBytes
					//           << " tokenRateBytesPerMs=" << pacerSnapshot.tokenRateBytesPerMs << std::endl;

					// Pacing ON 후보만 확인.
					// OFF는 정의상 pacingDelayMs = 0 이므로 굳이 출력할 필요 없음.
					for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
					{
						for (size_t fecIdx{ 0u }; fecIdx < FecCandidateCount; ++fecIdx)
						{
							const uint32_t frameSizeBytes = actionFrameSizes.sizeBytes[layer][fecIdx];

							const double requiredBytes =
							  pacerSnapshot.queuedBytes + static_cast<double>(frameSizeBytes);

							const double rawDeficitBytes = requiredBytes - pacerSnapshot.effectiveTokensBytes;

							const double deficitBytes = std::max(0.0, rawDeficitBytes);

							const double pacingDelayMs = pacerSnapshot.tokenRateBytesPerMs > 0.0
							                               ? deficitBytes / pacerSnapshot.tokenRateBytesPerMs
							                               : std::numeric_limits<double>::infinity();

							const auto& timing = actionTimings.values[layer][fecIdx][1];
						}
					}

					ActionCandidateList candidates;

					size_t candidateIndex{ 0u };

					// 30개 candidate 중 실제 prediction 가능한 candidate 수.
					size_t predictedCandidateCount{ 0u };

					bool allCandidatesHavePrediction{ true };

					for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
					{
						for (size_t fecIdx{ 0u }; fecIdx < FecCandidateCount; ++fecIdx)
						{
							for (size_t pacingIdx{ 0u }; pacingIdx < PacingCandidateCount; ++pacingIdx)
							{
								auto& candidate = candidates[candidateIndex++];

								candidate.layer          = layer;
								candidate.fecIndex       = fecIdx;
								candidate.pacingIndex    = pacingIdx;
								candidate.frameSizeBytes = actionFrameSizes.sizeBytes[layer][fecIdx];
								candidate.pacingDelayMs =
								  actionTimings.values[layer][fecIdx][pacingIdx].pacingDelayMs;

								candidate.action.spatialLayer        = static_cast<uint8_t>(layer);
								candidate.action.fecProtectionFactor = FecProtectionFactors[fecIdx];
								candidate.action.pacingEnabled       = pacingIdx == 1u;

								// ================================================
								// kNN query X_t(A)
								//
								// network 부분은 30개 action 모두 동일한
								// 현재 decision 시점 snapshot.
								// ================================================

								candidate.feature.rttMs = networkSnapshot.rttMs;

								candidate.feature.lossRate = networkSnapshot.lossRate;

								candidate.feature.availableBitrateBps = networkSnapshot.availableBitratebps;

								// candidate.feature.congestion = networkSnapshot.twccDelayTrend;
								candidate.feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;

								// ================================================
								// action-dependent feature.
								// ================================================

								candidate.feature.frameSizeBytes = static_cast<double>(candidate.frameSizeBytes);

								candidate.feature.pacingDelayMs = candidate.pacingDelayMs;

								// ================================================
								// WebRtcTransport가 소유한 kNN predictor에 query.
								// ================================================

								RTC::SlackPrediction prediction;

								const bool hasPrediction = this->listener->OnConsumerPredictSlack(
								  this, candidate.feature, candidate.action, prediction);

								if (hasPrediction)
								{
									candidate.hasPrediction                    = true;
									candidate.predictedDecodeSlackMs           = prediction.predictedDecodeSlackMs;
									candidate.predictedDeadlineMissProbability = prediction.deadlineMissProbability;

									++predictedCandidateCount;

									candidate.predictorSampleCount = prediction.sampleCount;
									candidate.debugNeighborCount   = prediction.debugNeighborCount;
									candidate.debugNeighbors       = prediction.debugNeighbors;
								}
								else
								{
									allCandidatesHavePrediction = false;
								}
							}
						}
					}

					const bool enoughColdStartSamples =
					  this->knnColdStartCollectionComplete &&
					  this->knnColdStartTotalSampleCount >= this->knnColdStartRequiredTotalSamples;

					// ============================================================
					// 중요:
					//
					// Cold Start 여부는 "초기 collection 완료 여부"로만 결정한다.
					//
					// prediction 일부가 실패했다고 해서 Cold Start로 돌아가면 안 된다.
					// ============================================================
					const bool coldStartMode = !enoughColdStartSamples;

					// ============================================================
					// Prediction readiness.
					//
					// Action-conditioned + no balanced coverage 조합에서는
					// 일부 action에 exact-action sample이 k개보다 적을 수 있다.
					//
					// 이 경우 30개 전부의 prediction을 요구하면
					// KNN이 영원히 시작하지 못할 수 있다.
					//
					// 따라서:
					//
					//   ActionConditioned=true
					//   Coverage=false
					//
					// 일 때만 "prediction 가능한 candidate 중에서 선택"한다.
					//
					// 기존 true,true 실험에서는 여전히 30개 전부를 요구하므로
					// 기존 동작에 영향 없음.
					// ============================================================

					const bool allowPartialCandidatePrediction =
					  RTC::KnnExperiment::ActionConditionedPredictionEnabled &&
					  !RTC::KnnExperiment::ColdStartActionCoverageEnabled;

					const bool knnPredictionsReady = allowPartialCandidatePrediction
					                                   ? (predictedCandidateCount > 0u)
					                                   : allCandidatesHavePrediction;

					const ActionCandidate* selectedModelAction{ nullptr };

					bool holdingLastKnnAction{ false };

					// KNN phase.
					if (!coldStartMode)
					{
						// Case 1:
						//
						// 기본:
						//   모든 30개 candidate prediction 가능.
						// ActionConditioned=true + Coverage=false:
						//   prediction 가능한 candidate가 하나 이상 존재.
						// SelectBestKnnAction()은 hasPrediction=false candidate를
						// 자동으로 제외한다.
						if (knnPredictionsReady)
						{
							selectedModelAction = SelectBestKnnAction(candidates);
						}
						// Case 2:
						// KNN에는 이미 진입했지만 일부 candidate prediction 불가: 마지막으로 정상 선택했던 KNN
						// action 유지.
						else if (this->hasLastKnnAction)
						{
							for (const auto& candidate : candidates)
							{
								if (
								  candidate.layer == this->lastKnnLayer &&
								  candidate.fecIndex == this->lastKnnFecIndex &&
								  candidate.pacingIndex == this->lastKnnPacingIndex)
								{
									selectedModelAction  = &candidate;
									holdingLastKnnAction = true;

									break;
								}
							}
						}
					}

					// ============================================================
					// yeon: 각 spatial layer 내부의 best candidate.
					// ============================================================
					const ActionCandidate* bestL0{ nullptr };
					const ActionCandidate* bestL1{ nullptr };
					const ActionCandidate* bestL2{ nullptr };

					if (!coldStartMode)
					{
						bestL0 = SelectBestKnnActionForLayer(candidates, 0u);

						bestL1 = SelectBestKnnActionForLayer(candidates, 1u);

						bestL2 = SelectBestKnnActionForLayer(candidates, 2u);
					}

					// yeon: kNN layer ranking debug CSV.
					// 각 layer에서 가장 좋은 FEC/Pacing candidate 하나만 저장.
					if (knnPredictionsReady && g_knnLayerRankCsv.is_open())
					{
						if (!g_knnLayerRankHeaderWritten)
						{
							g_knnLayerRankCsv << "consumerId,"
							                  << "logicalFrameId,"
							                  << "rttMs,"
							                  << "lossRate,"
							                  << "availableBitrateBps,"
							                  << "congestion,"
							                  << "l0FecIdx,l0Pacing,l0FrameSize,l0PacingDelay,l0MissProb,l0Slack,"
							                  << "l1FecIdx,l1Pacing,l1FrameSize,l1PacingDelay,l1MissProb,l1Slack,"
							                  << "l2FecIdx,l2Pacing,l2FrameSize,l2PacingDelay,l2MissProb,l2Slack"
							                  << "\n";

							g_knnLayerRankHeaderWritten = true;
						}

						g_knnLayerRankCsv << this->id << "," << logicalFrameId << "," << networkSnapshot.rttMs
						                  << "," << networkSnapshot.lossRate << ","
						                  << networkSnapshot.availableBitratebps << ","
						                  << networkSnapshot.camelCongestionGradientMsPerKb << ",";

						auto writeLayer = [&](const ActionCandidate* c)
						{
							if (!c)
							{
								g_knnLayerRankCsv << ",,,,,";
								return;
							}

							g_knnLayerRankCsv << c->fecIndex << "," << c->pacingIndex << "," << c->frameSizeBytes
							                  << "," << c->pacingDelayMs << ","
							                  << c->predictedDeadlineMissProbability << ","
							                  << c->predictedDecodeSlackMs;
						};

						writeLayer(bestL0);
						g_knnLayerRankCsv << ",";

						writeLayer(bestL1);
						g_knnLayerRankCsv << ",";

						writeLayer(bestL2);

						g_knnLayerRankCsv << "\n";
					}

					// ============================================================
					// yeon: nearest-neighbor debug CSV.
					//
					// KNN frame마다 각 layer의 best candidate가 실제 prediction에서
					// 사용한 nearest 5개 historical sample을 저장한다.
					//
					// transition frame만 기록하지 않는 이유:
					// N-1 / N frame의 neighbor set 변화를 비교하기 위함.
					// ============================================================
					if (selectedModelAction && g_knnNeighborCsv.is_open())
					{
						if (!g_knnNeighborHeaderWritten)
						{
							g_knnNeighborCsv << "consumerId,"
							                 << "logicalFrameId,"
							                 << "selectedLayer,"
							                 << "candidateLayer,"
							                 << "candidateFecIdx,"
							                 << "candidatePacing,"
							                 << "sampleCount,"
							                 << "queryRttMs,"
							                 << "queryLossRate,"
							                 << "queryAvailableBitrateBps,"
							                 << "queryCongestion,"
							                 << "queryFrameSizeBytes,"
							                 << "queryPacingDelayMs,"
							                 << "candidateMissProb,"
							                 << "candidatePredictedSlack,"
							                 << "neighborRank,"
							                 << "neighborFrameId,"
							                 << "neighborDistance,"
							                 << "neighborRttMs,"
							                 << "neighborLossRate,"
							                 << "neighborAvailableBitrateBps,"
							                 << "neighborCongestion,"
							                 << "neighborFrameSizeBytes,"
							                 << "neighborPacingDelayMs,"
							                 << "neighborDecodeSlackMs,"
							                 << "neighborDeadlineMiss"
							                 << "\n";

							g_knnNeighborHeaderWritten = true;
						}

						const std::array<const ActionCandidate*, CandidateLayerCount> bestByLayer{ bestL0,
						                                                                           bestL1,
						                                                                           bestL2 };

						for (size_t layer{ 0u }; layer < CandidateLayerCount; ++layer)
						{
							const auto* candidate = bestByLayer[layer];

							if (!candidate)
							{
								continue;
							}

							for (size_t rank{ 0u }; rank < candidate->debugNeighborCount; ++rank)
							{
								const auto& neighbor = candidate->debugNeighbors[rank];

								g_knnNeighborCsv << this->id << "," << logicalFrameId
								                 << ","

								                 // 최종 30-action winner.
								                 << selectedModelAction->layer
								                 << ","

								                 // 이 neighbor가 어느 layer-best candidate에 대한 것인지.
								                 << candidate->layer << "," << candidate->fecIndex << ","
								                 << candidate->pacingIndex << ","

								                 << candidate->predictorSampleCount
								                 << ","

								                 // Query X_t(A).
								                 << candidate->feature.rttMs << "," << candidate->feature.lossRate
								                 << "," << candidate->feature.availableBitrateBps << ","
								                 << candidate->feature.congestion << ","
								                 << candidate->feature.frameSizeBytes << ","
								                 << candidate->feature.pacingDelayMs << ","

								                 << candidate->predictedDeadlineMissProbability << ","
								                 << candidate->predictedDecodeSlackMs
								                 << ","

								                 // Neighbor.
								                 << rank << "," << neighbor.frameId << "," << neighbor.distance << ","

								                 << neighbor.feature.rttMs << "," << neighbor.feature.lossRate
								                 << "," << neighbor.feature.availableBitrateBps << ","
								                 << neighbor.feature.congestion << ","
								                 << neighbor.feature.frameSizeBytes << ","
								                 << neighbor.feature.pacingDelayMs << ","

								                 << neighbor.decodeSlackMs << "," << (neighbor.deadlineMiss ? 1 : 0)

								                 << "\n";
							}
						}
					}

					if (selectedModelAction) // knn
					{
						const auto& selected = *selectedModelAction;

						// 새로운 KNN decision이 정상적으로 수행된 경우에만
						// last valid KNN action 갱신.
						// HOLD frame에서는 기존 값을 그대로 유지한다.
						if (!holdingLastKnnAction)
						{
							this->hasLastKnnAction   = true;
							this->lastKnnLayer       = selected.layer;
							this->lastKnnFecIndex    = selected.fecIndex;
							this->lastKnnPacingIndex = selected.pacingIndex;
						}
						else
						{ // KNN 단계에서 일부 action의 support가 부족했지만 cold-start 재진입은 하지 않고 이전
							// action을 유지
							MS_WARN_TAG(
							  simulcast,
							  "[KNN-HOLD] "
							  "consumer:%s "
							  "logical:%" PRIu32
							  " layer:%zu "
							  "fecIdx:%zu "
							  "pacing:%zu "
							  "reason=MISSING_CANDIDATE_PREDICTION",
							  this->id.c_str(),
							  logicalFrameId,
							  selected.layer,
							  selected.fecIndex,
							  selected.pacingIndex);
						}

						// yeon: Cold Start 종료
						if (this->knnColdStartForceLayerActive)
						{
							MS_WARN_TAG(
							  simulcast,
							  "[KNN-COLD-LAYER-FORCE-END] "
							  "consumer:%s "
							  "coldFrames:%zu "
							  "lastForcedLayer:%" PRIi16,
							  this->id.c_str(),
							  this->knnColdStartFrameCount,
							  this->knnColdStartForcedSpatialLayer);

							this->knnColdStartForceLayerActive   = false;
							this->knnColdStartForcedSpatialLayer = -1;
						}

						// yeon: KNN spatial-layer CAP mode.
						// selected.layer는 직접 target spatial layer가 아니라
						// 기존 mediasoup BWE allocator에 줄 maximum spatial layer.
						// 이 코드는 logical frame마다 실행되므로 kNN cap도
						// 매 logical frame마다 새로 계산된다.
						const int16_t selectedLayerCap = static_cast<int16_t>(selected.layer);

						const bool layerCapChanged =
						  !this->knnControlActive || this->knnMaxSpatialLayer != selectedLayerCap;

						this->knnControlActive   = true;
						this->knnMaxSpatialLayer = selectedLayerCap;

						// ============================================================
						// cap이 달라졌다면 BWE allocator를 다시 실행.
						//
						// 중요:
						// 여기서 UpdateTargetLayers()를 직접 호출하지 않는다.
						//
						// 실제 layer는:
						//   BWE available bitrate
						//   producer layer bitrate
						//   preferred layer
						//   kNN cap
						//
						// 을 기존 mediasoup allocator가 함께 고려해서 결정.
						// ============================================================
						if (layerCapChanged)
						{
							this->listener->OnConsumerNeedBitrateChange(this);
						}

						RTC::SlackActionDecision decision;
						decision.logicalFrameId      = logicalFrameId;
						decision.spatialLayer        = static_cast<uint8_t>(selected.layer);
						decision.fecIndex            = selected.fecIndex;
						decision.fecRedundancyRate   = FecRedundancyRates[selected.fecIndex];
						decision.fecProtectionFactor = FecProtectionFactors[selected.fecIndex];
						decision.pacingEnabled       = selected.pacingIndex == 1u;
						decision.modelSelected       = true;

						// 중요:
						// candidate.feature 자체가 바로 decision-time X_t(A).
						decision.feature = selected.feature;

						// ============================================================
						// 실제 전송 layer가 model-selected layer와 다를 수 있으므로,
						// 같은 FEC/Pacing에 대해 각 spatial layer의 feature를 보관.
						//
						// 나중에 실제 전송 layer를 확인한 뒤 historical sample에서는
						// 해당 layer feature를 사용한다.
						// ============================================================
						for (const auto& candidate : candidates)
						{
							if (candidate.fecIndex != selected.fecIndex || candidate.pacingIndex != selected.pacingIndex)
							{
								continue;
							}

							if (candidate.layer >= CandidateLayerCount)
							{
								continue;
							}

							decision.actionFeatureBySpatialLayer[candidate.layer]    = candidate.feature;
							decision.hasActionFeatureBySpatialLayer[candidate.layer] = true;

							if (candidate.hasPrediction)
							{
								decision.predictedDecodeSlackBySpatialLayer[candidate.layer] =
								  candidate.predictedDecodeSlackMs;

								decision.hasPredictedDecodeSlackBySpatialLayer[candidate.layer] = true;

								decision.predictedDeadlineMissProbabilityBySpatialLayer[candidate.layer] =
								  candidate.predictedDeadlineMissProbability;

								decision.hasPredictedDeadlineMissProbabilityBySpatialLayer[candidate.layer] = true;
							}
						}

						decision.aceQueueBytes = networkSnapshot.aceQueueBytes;

						decision.pacingBacklogBytes = pacerSnapshot.queuedBytes;

						decision.pacingBucketSizeBytes = pacerSnapshot.bucketCapacityBytes;

						decision.tokenRateBytesPerMs = pacerSnapshot.tokenRateBytesPerMs;

						// 선택된 action에 대해 kNN이 예측했던 Slack. : knn mode
						decision.hasPredictedDecodeSlack             = selected.hasPrediction;
						decision.hasPredictedDeadlineMissProbability = selected.hasPrediction;
						if (selected.hasPrediction)
						{
							decision.predictedDecodeSlackMs = selected.predictedDecodeSlackMs;

							decision.predictedDeadlineMissProbability = selected.predictedDeadlineMissProbability;
						}

						// ============================================================
						// KNN evaluation state metadata.
						//
						// prediction과 action 결정은 이미 정상적으로 끝난 상태이다.
						// 여기서는 이 KNN frame이:
						//   pause인지
						//   NORMAL collection인지
						//   2MBIT collection인지
						// 를 기록하고 state machine만 진행한다.
						// ============================================================

						AnnotateAndAdvanceKnnEvaluationDecision(decision);

						this->pendingSlackActionDecisionByLogicalFrame[logicalFrameId] = decision;
					}
					else if (coldStartMode)
					{
						// kNN NOT READY -> 기존 cold-start 코드

						this->knnControlActive   = false;
						this->knnMaxSpatialLayer = -1;

						if (KnnColdStartBalancedScenarioEnabled)
						{
							// ========================================================
							// NEW: Balanced Cold Start V2.
							//
							// Per layer:
							//   network phase #1 : 10 actions x 150 valid samples = 1500
							//   NETWORK_PAUSE   : 150 logical frames (training excluded)
							//   network phase #2 : 10 actions x 150 valid samples = 1500
							//   => 3000 valid samples / layer.
							//
							// Between layers:
							//   LAYER_PAUSE 150 logical frames, tc state unchanged.
							// ========================================================

							const bool networkPause =
							  this->knnColdStartScenarioState == KnnColdStartScenarioState::NETWORK_PAUSE;

							const bool layerPause =
							  this->knnColdStartScenarioState == KnnColdStartScenarioState::LAYER_PAUSE;

							const bool isPause = networkPause || layerPause;

							const int16_t coldStartCollectionLayer = this->knnColdStartCollectionLayer;

							// Balanced Cold Start always forces the currently collected layer.
							this->knnColdStartForceLayerActive   = true;
							this->knnColdStartForcedSpatialLayer = coldStartCollectionLayer;

							const bool validColdStartForcedLayer =
							  coldStartCollectionLayer >= 0 &&
							  static_cast<size_t>(coldStartCollectionLayer) < this->producerRtpStreams.size() &&
							  this->producerRtpStreams[coldStartCollectionLayer] != nullptr;

							if (
							  validColdStartForcedLayer && (this->targetLayers.spatial != coldStartCollectionLayer ||
								                              this->targetLayers.temporal != 0))
							{
								UpdateTargetLayers(coldStartCollectionLayer, 0);
							}

							// Initial L0 collection entry log.
							if (!this->knnColdStartInitialLayerEnterLogged)
							{
								this->knnColdStartInitialLayerEnterLogged = true;

								MS_WARN_TAG(
								  simulcast,
								  "[KNN-COLD-LAYER-ENTER] "
								  "consumer:%s layer:%" PRIi16 " network:%s targetSamples:%zu totalSamples:%zu/%zu",
								  this->id.c_str(),
								  this->knnColdStartCollectionLayer,
								  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
								  this->knnColdStartSamplesPerLayer,
								  this->knnColdStartTotalSampleCount,
								  this->knnColdStartRequiredTotalSamples);
							}

							size_t fecIdx{ 0u };
							size_t pacingIdx{ 0u };
							bool validColdStartAction{ false };

							if (isPause)
							{
								// Pause frames still execute normal media/FEC/pacing processing.
								// They rotate through all 10 FEC/pacing actions but are never
								// inserted into the SlackPredictor training set.
								const size_t slot = this->knnColdStartPauseFrameCount % KnnColdStartActionCount;

								fecIdx    = slot / KnnColdStartPacingActionCount;
								pacingIdx = slot % KnnColdStartPacingActionCount;

								validColdStartAction = true;
							}
							else
							{
								// COLLECT: choose an action that has not yet reached 150
								// accepted samples in the current network phase.
								validColdStartAction = GetNextKnnColdStartBalancedAction(fecIdx, pacingIdx);
							}

							if (validColdStartAction)
							{
								const size_t layer = static_cast<size_t>(coldStartCollectionLayer);

								RTC::SlackActionDecision decision;

								decision.logicalFrameId = logicalFrameId;
								decision.spatialLayer   = static_cast<uint8_t>(layer);

								decision.fecIndex            = fecIdx;
								decision.fecRedundancyRate   = FecRedundancyRates[fecIdx];
								decision.fecProtectionFactor = FecProtectionFactors[fecIdx];
								decision.pacingEnabled       = pacingIdx == 1u;

								// Still a cold-start decision, but its FEC/pacing action must
								// override the legacy global adaptive-FEC / ispacing state.
								decision.modelSelected      = false;
								decision.forceRuntimeAction = true;

								// Pause frames remain in FrameRecord/CSV but are excluded only
								// from SlackPredictor training.
								decision.skipKnnTrainingSample = isPause;

								if (networkPause)
								{
									decision.coldStartPhase = RTC::KnnColdStartPhase::NETWORK_PAUSE;
								}
								else if (layerPause)
								{
									decision.coldStartPhase = RTC::KnnColdStartPhase::LAYER_PAUSE;
								}
								else
								{
									decision.coldStartPhase = RTC::KnnColdStartPhase::COLLECT;
								}

								decision.coldStartNetworkProfile = this->knnColdStartNetworkProfile;
								decision.coldStartTargetNetworkProfile =
								  isPause ? this->knnColdStartPauseTargetNetworkProfile
									        : this->knnColdStartNetworkProfile;
								decision.coldStartPauseFrameIndex =
								  isPause ? this->knnColdStartPauseFrameCount + 1u : 0u;

								// Decision-time query feature for the selected cold-start action.
								decision.feature.rttMs               = networkSnapshot.rttMs;
								decision.feature.lossRate            = networkSnapshot.lossRate;
								decision.feature.availableBitrateBps = networkSnapshot.availableBitratebps;
								decision.feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;
								decision.feature.frameSizeBytes =
								  static_cast<double>(actionFrameSizes.sizeBytes[layer][fecIdx]);
								decision.feature.pacingDelayMs =
								  actionTimings.values[layer][fecIdx][pacingIdx].pacingDelayMs;

								// Keep per-layer features as well. Training later uses the actual
								// forwarded spatial layer rather than the selected/capped layer.
								for (size_t candidateLayer{ 0u }; candidateLayer < CandidateLayerCount;
								     ++candidateLayer)
								{
									auto& feature = decision.actionFeatureBySpatialLayer[candidateLayer];

									feature.rttMs               = networkSnapshot.rttMs;
									feature.lossRate            = networkSnapshot.lossRate;
									feature.availableBitrateBps = networkSnapshot.availableBitratebps;
									feature.congestion          = networkSnapshot.camelCongestionGradientMsPerKb;
									feature.frameSizeBytes =
									  static_cast<double>(actionFrameSizes.sizeBytes[candidateLayer][fecIdx]);
									feature.pacingDelayMs =
									  actionTimings.values[candidateLayer][fecIdx][pacingIdx].pacingDelayMs;

									decision.hasActionFeatureBySpatialLayer[candidateLayer] = true;
								}

								decision.aceQueueBytes         = networkSnapshot.aceQueueBytes;
								decision.pacingBacklogBytes    = pacerSnapshot.queuedBytes;
								decision.pacingBucketSizeBytes = pacerSnapshot.bucketCapacityBytes;
								decision.tokenRateBytesPerMs   = pacerSnapshot.tokenRateBytesPerMs;

								this->pendingSlackActionDecisionByLogicalFrame[logicalFrameId] = decision;

								// --------------------------------------------------------
								// Pause duration is counted in logical frames, not accepted
								// training samples. Decision is stored first so the final pause
								// frame is recorded as index=150 in FrameRecord/CSV.
								// --------------------------------------------------------
								if (isPause)
								{
									++this->knnColdStartPauseFrameCount;

									if (this->knnColdStartPauseFrameCount >= KnnColdStartPauseFrames)
									{
										if (networkPause)
										{
											// The external tc controller has had the entire pause
											// window to apply the requested transition.
											this->knnColdStartNetworkProfile = this->knnColdStartPauseTargetNetworkProfile;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-NETWORK-PAUSE-EXIT] "
											  "consumer:%s layer:%" PRIi16 " network:%s pauseFrames:%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartPauseFrameCount);

											ResetKnnColdStartNetworkPhaseCoverage();
											this->knnColdStartPauseFrameCount = 0u;
											this->knnColdStartScenarioState   = KnnColdStartScenarioState::COLLECT;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-NETWORK-COLLECT-START] "
											  "consumer:%s layer:%" PRIi16
											  " network:%s targetSamples:%zu layerSamples:%zu/%zu totalSamples:%zu/%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  KnnColdStartSamplesPerNetworkPhase,
											  this->knnColdStartLayerSampleCounts[static_cast<size_t>(
											    this->knnColdStartCollectionLayer)],
											  this->knnColdStartSamplesPerLayer,
											  this->knnColdStartTotalSampleCount,
											  this->knnColdStartRequiredTotalSamples);
										}
										else if (layerPause)
										{
											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-LAYER-PAUSE-EXIT] "
											  "consumer:%s layer:%" PRIi16 " network:%s pauseFrames:%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartPauseFrameCount);

											ResetKnnColdStartNetworkPhaseCoverage();
											this->knnColdStartPauseFrameCount = 0u;
											this->knnColdStartScenarioState   = KnnColdStartScenarioState::COLLECT;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-LAYER-ENTER] "
											  "consumer:%s layer:%" PRIi16
											  " network:%s targetSamples:%zu totalSamples:%zu/%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartSamplesPerLayer,
											  this->knnColdStartTotalSampleCount,
											  this->knnColdStartRequiredTotalSamples);
										}
									}
								}
							}
						}
						else
						{
							// ========================================================
							// LEGACY Cold Start
							// ========================================================

							RTC::SlackRuntimeActionState runtimeActionState;

							const bool hasRuntimeActionState =
							  this->listener->OnConsumerGetSlackRuntimeActionState(this, runtimeActionState);

							if (hasRuntimeActionState)
							{
								const bool networkPause =
								  KnnExperimentPauseStateEnabled &&
								  this->knnColdStartScenarioState == KnnColdStartScenarioState::NETWORK_PAUSE;

								const bool layerPause =
								  KnnExperimentPauseStateEnabled &&
								  this->knnColdStartScenarioState == KnnColdStartScenarioState::LAYER_PAUSE;

								const bool isPause = networkPause || layerPause;
								if (isPause)
								{
									// ============================================================
									// Legacy action distribution + explicit pause.
									//
									// ColdStartActionCoverageEnabled=false인 상태에서도
									// PauseStateEnabled=true이면 이 path를 탄다.
									//
									// 중요한 점:
									//   - balanced action rotation 사용 X
									//   - 기존 runtime FEC 사용
									//   - legacy cold-start와 동일하게 pacing OFF
									//   - FrameRecord/CSV O
									//   - SlackPredictor training X
									// ============================================================

									const int16_t coldStartCollectionLayer = this->knnColdStartCollectionLayer;

									// ------------------------------------------------------------
									// Cold Start layer force는 그대로 유지.
									// ------------------------------------------------------------
									this->knnColdStartForceLayerActive   = true;
									this->knnColdStartForcedSpatialLayer = coldStartCollectionLayer;

									const bool validColdStartForcedLayer =
									  coldStartCollectionLayer >= 0 &&
									  static_cast<size_t>(coldStartCollectionLayer) < this->producerRtpStreams.size() &&
									  this->producerRtpStreams[coldStartCollectionLayer] != nullptr;

									if (
									  validColdStartForcedLayer &&
									  (this->targetLayers.spatial != coldStartCollectionLayer ||
										 this->targetLayers.temporal != 0))
									{
										UpdateTargetLayers(coldStartCollectionLayer, 0);
									}

									// ------------------------------------------------------------
									// Layer pause 중에는 기존 legacy settle counter도 같이
									// 진행한다.
									//
									// 이렇게 해야 150-frame LAYER_PAUSE가 끝난 뒤
									// 불필요하게 다시 60 frame을 기다리지 않는다.
									//
									// 실제 target/current가 안정적이지 않았다면 counter가
									// 다시 0이 되므로 안전성도 유지된다.
									// ------------------------------------------------------------
									if (layerPause)
									{
										const bool actualLayerStable =
										  this->currentSpatialLayer == coldStartCollectionLayer &&
										  this->targetLayers.spatial == coldStartCollectionLayer &&
										  this->currentSpatialLayer >= 0 &&
										  static_cast<size_t>(this->currentSpatialLayer) < CandidateLayerCount;

										if (actualLayerStable)
										{
											++this->knnColdStartStableLayerFrameCount;
										}
										else
										{
											this->knnColdStartStableLayerFrameCount = 0u;
										}
									}

									// ------------------------------------------------------------
									// Coverage=false이므로 balanced action을 고르지 않는다.
									//
									// 기존 Legacy Cold Start와 동일하게 runtime FEC를 사용.
									// ------------------------------------------------------------
									size_t fecIdx{ 0u };

									bool validFecAction{ true };

									if (runtimeActionState.fecEnabled)
									{
										validFecAction =
										  GetFecCandidateIndex(runtimeActionState.fecProtectionFactor, fecIdx);
									}
									else
									{
										fecIdx = 0u;
									}

									// ------------------------------------------------------------
									// 기존 Legacy Cold Start가 pacing을 false로 강제하고 있으므로
									// pause에서도 동일하게 유지.
									// ------------------------------------------------------------
									const bool coldStartPacingEnabled = false;

									this->listener->OnConsumerSetColdStartPacing(this, coldStartPacingEnabled);

									runtimeActionState.pacingEnabled = coldStartPacingEnabled;

									if (validFecAction)
									{
										const size_t layer = static_cast<size_t>(coldStartCollectionLayer);

										const size_t pacingIdx{ 0u };

										RTC::SlackActionDecision decision;

										decision.logicalFrameId = logicalFrameId;

										decision.spatialLayer = static_cast<uint8_t>(layer);

										decision.fecIndex = fecIdx;

										decision.fecRedundancyRate = FecRedundancyRates[fecIdx];

										decision.fecProtectionFactor =
										  runtimeActionState.fecEnabled ? runtimeActionState.fecProtectionFactor : 0u;

										decision.pacingEnabled = coldStartPacingEnabled;

										// --------------------------------------------------------
										// Legacy action이므로 KNN model-selected가 아님.
										// Runtime FEC는 기존 runtime을 그대로 쓰므로
										// balanced mode처럼 강제 override하지 않는다.
										// --------------------------------------------------------
										decision.modelSelected = false;

										decision.forceRuntimeAction = false;

										// --------------------------------------------------------
										// pause frame은 CSV에는 남기되 predictor training에서는 제외.
										// --------------------------------------------------------
										decision.skipKnnTrainingSample = true;

										if (networkPause)
										{
											decision.coldStartPhase = RTC::KnnColdStartPhase::NETWORK_PAUSE;
										}
										else
										{
											decision.coldStartPhase = RTC::KnnColdStartPhase::LAYER_PAUSE;
										}

										decision.coldStartNetworkProfile = this->knnColdStartNetworkProfile;

										decision.coldStartTargetNetworkProfile =
										  this->knnColdStartPauseTargetNetworkProfile;

										decision.coldStartPauseFrameIndex = this->knnColdStartPauseFrameCount + 1u;

										// --------------------------------------------------------
										// Decision-time feature.
										// --------------------------------------------------------
										decision.feature.rttMs = networkSnapshot.rttMs;

										decision.feature.lossRate = networkSnapshot.lossRate;

										decision.feature.availableBitrateBps = networkSnapshot.availableBitratebps;

										decision.feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;

										decision.feature.frameSizeBytes =
										  static_cast<double>(actionFrameSizes.sizeBytes[layer][fecIdx]);

										decision.feature.pacingDelayMs =
										  actionTimings.values[layer][fecIdx][pacingIdx].pacingDelayMs;

										// --------------------------------------------------------
										// 실제 forwarded layer가 다를 수 있으므로
										// layer별 feature도 보존.
										// --------------------------------------------------------
										for (size_t candidateLayer{ 0u }; candidateLayer < CandidateLayerCount;
										     ++candidateLayer)
										{
											auto& feature = decision.actionFeatureBySpatialLayer[candidateLayer];

											feature.rttMs = networkSnapshot.rttMs;

											feature.lossRate = networkSnapshot.lossRate;

											feature.availableBitrateBps = networkSnapshot.availableBitratebps;

											feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;

											feature.frameSizeBytes =
											  static_cast<double>(actionFrameSizes.sizeBytes[candidateLayer][fecIdx]);

											feature.pacingDelayMs =
											  actionTimings.values[candidateLayer][fecIdx][pacingIdx].pacingDelayMs;

											decision.hasActionFeatureBySpatialLayer[candidateLayer] = true;
										}

										decision.aceQueueBytes = networkSnapshot.aceQueueBytes;

										decision.pacingBacklogBytes = pacerSnapshot.queuedBytes;

										decision.pacingBucketSizeBytes = pacerSnapshot.bucketCapacityBytes;

										decision.tokenRateBytesPerMs = pacerSnapshot.tokenRateBytesPerMs;

										// --------------------------------------------------------
										// FrameRecord에서 사용할 pending decision.
										// --------------------------------------------------------
										this->pendingSlackActionDecisionByLogicalFrame[logicalFrameId] = decision;
									}

									// ============================================================
									// Pause는 training sample 수가 아니라 logical-frame 기준.
									// ============================================================
									++this->knnColdStartPauseFrameCount;

									if (this->knnColdStartPauseFrameCount >= KnnColdStartPauseFrames)
									{
										if (networkPause)
										{
											// ====================================================
											// NETWORK_PAUSE 종료.
											//
											// controller는 pause 진입 로그를 보고 이미 tc를
											// target profile로 변경했다.
											// ====================================================

											this->knnColdStartNetworkProfile = this->knnColdStartPauseTargetNetworkProfile;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-NETWORK-PAUSE-EXIT] "
											  "consumer:%s "
											  "layer:%" PRIi16
											  " "
											  "network:%s "
											  "pauseFrames:%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartPauseFrameCount);

											// 새 network phase 1500개를 0부터 시작.
											ResetKnnColdStartNetworkPhaseCoverage();

											this->knnColdStartPauseFrameCount = 0u;

											this->knnColdStartScenarioState = KnnColdStartScenarioState::COLLECT;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-NETWORK-COLLECT-START] "
											  "consumer:%s "
											  "layer:%" PRIi16
											  " "
											  "network:%s "
											  "targetSamples:%zu "
											  "layerSamples:%zu/%zu "
											  "totalSamples:%zu/%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  KnnColdStartSamplesPerNetworkPhase,
											  this->knnColdStartLayerSampleCounts[static_cast<size_t>(
											    this->knnColdStartCollectionLayer)],
											  this->knnColdStartSamplesPerLayer,
											  this->knnColdStartTotalSampleCount,
											  this->knnColdStartRequiredTotalSamples);
										}
										else if (layerPause)
										{
											// ====================================================
											// LAYER_PAUSE 종료.
											//
											// network profile은 절대로 바꾸지 않는다.
											// ====================================================

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-LAYER-PAUSE-EXIT] "
											  "consumer:%s "
											  "layer:%" PRIi16
											  " "
											  "network:%s "
											  "pauseFrames:%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartPauseFrameCount);

											ResetKnnColdStartNetworkPhaseCoverage();

											this->knnColdStartPauseFrameCount = 0u;

											this->knnColdStartScenarioState = KnnColdStartScenarioState::COLLECT;

											MS_WARN_TAG(
											  simulcast,
											  "[KNN-COLD-LAYER-ENTER] "
											  "consumer:%s "
											  "layer:%" PRIi16
											  " "
											  "network:%s "
											  "targetSamples:%zu "
											  "totalSamples:%zu/%zu",
											  this->id.c_str(),
											  this->knnColdStartCollectionLayer,
											  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
											  this->knnColdStartSamplesPerLayer,
											  this->knnColdStartTotalSampleCount,
											  this->knnColdStartRequiredTotalSamples);
										}
									}
								}
								else
								{
									// 최초 Cold Start frame에서 runtime default를 한 번 저장
									if (!this->knnColdStartDefaultPacingCaptured)
									{
										this->knnColdStartDefaultPacingEnabled = runtimeActionState.pacingEnabled;

										this->knnColdStartDefaultPacingCaptured = true;

										MS_WARN_TAG(
										  simulcast,
										  "[KNN-COLD-PACING-INIT] "
										  "consumer:%s default:%s",
										  this->id.c_str(),
										  this->knnColdStartDefaultPacingEnabled ? "ON" : "OFF");
									}

									++this->knnColdStartFrameCount;

									const int16_t coldStartCollectionLayer = this->knnColdStartCollectionLayer;

									// Network-coverage mode:
									// actual target layer까지 collection layer로 강제.
									this->knnColdStartForceLayerActive   = true;
									this->knnColdStartForcedSpatialLayer = coldStartCollectionLayer;

									const bool validColdStartForcedLayer =
									  coldStartCollectionLayer >= 0 &&
									  static_cast<size_t>(coldStartCollectionLayer) < this->producerRtpStreams.size() &&
									  this->producerRtpStreams[coldStartCollectionLayer] != nullptr;

									if (
									  validColdStartForcedLayer &&
									  (this->targetLayers.spatial != coldStartCollectionLayer ||
										 this->targetLayers.temporal != 0))
									{
										UpdateTargetLayers(coldStartCollectionLayer, 0);
									}

									const size_t defaultPhaseEnd = KnnColdStartDefaultPacingFrames; // 100

									const size_t oppositePhaseEnd =
									  KnnColdStartDefaultPacingFrames + KnnColdStartOppositePacingFrames; // 200

									// 기본적으로 default 사용.
									// bool coldStartPacingEnabled = this->knnColdStartDefaultPacingEnabled;
									bool coldStartPacingEnabled = false;

									// 실제 WebRtcTransport runtime pacing 변경.
									this->listener->OnConsumerSetColdStartPacing(this, coldStartPacingEnabled);

									// 위 callback에서 실제 runtime도 변경했으므로
									// 아래 decision 생성에 사용할 local snapshot도 일치시킨다.
									runtimeActionState.pacingEnabled = coldStartPacingEnabled;

									// phase 전환 확인용 로그.
									if (
									  this->knnColdStartFrameCount == 1u ||
									  this->knnColdStartFrameCount == defaultPhaseEnd + 1u ||
									  this->knnColdStartFrameCount == oppositePhaseEnd + 1u)
									{
										// MS_WARN_TAG(
										//   simulcast,
										//   "[KNN-COLD-PACING-PHASE] "
										//   "consumer:%s "
										//   "frameCount:%zu "
										//   "pacing:%s",
										//   this->id.c_str(),
										//   this->knnColdStartFrameCount,
										//   coldStartPacingEnabled ? "ON" : "OFF");
									}

									bool stableCollectionLayer{ false };

									const bool actualLayerStable =
									  this->currentSpatialLayer == coldStartCollectionLayer &&
									  this->targetLayers.spatial == coldStartCollectionLayer &&
									  this->currentSpatialLayer >= 0 &&
									  static_cast<size_t>(this->currentSpatialLayer) < CandidateLayerCount;

									if (actualLayerStable)
									{
										++this->knnColdStartStableLayerFrameCount;
									}
									else
									{
										this->knnColdStartStableLayerFrameCount = 0u;
									}

									const bool layerSettleFinished =
									  this->knnColdStartStableLayerFrameCount > KnnColdStartLayerSettleFrames;

									stableCollectionLayer = actualLayerStable && layerSettleFinished;

									if (stableCollectionLayer)
									{
										size_t fecIdx{ 0u };

										bool validFecAction{ true };

										if (runtimeActionState.fecEnabled)
										{
											validFecAction =
											  GetFecCandidateIndex(runtimeActionState.fecProtectionFactor, fecIdx);
										}
										else
										{
											// FlexFEC가 실제 비활성화되어 있으면 FEC 0%.
											fecIdx = 0u;
										}

										if (validFecAction)
										{
											const size_t layer     = static_cast<size_t>(this->currentSpatialLayer);
											const size_t pacingIdx = coldStartPacingEnabled ? 1u : 0u;

											RTC::SlackActionDecision decision;

											decision.logicalFrameId = logicalFrameId;

											// CAP mode:
											decision.spatialLayer = static_cast<uint8_t>(layer);

											decision.fecIndex = fecIdx;

											decision.fecRedundancyRate = FecRedundancyRates[fecIdx];

											decision.fecProtectionFactor =
											  runtimeActionState.fecEnabled ? runtimeActionState.fecProtectionFactor : 0u;

											// 이번 Cold Start frame에서 실제 적용한 pacing.
											decision.pacingEnabled = coldStartPacingEnabled;

											decision.modelSelected = false;

											decision.forceRuntimeAction    = false;
											decision.skipKnnTrainingSample = false;

											decision.coldStartPhase = RTC::KnnColdStartPhase::COLLECT;

											decision.coldStartNetworkProfile = this->knnColdStartNetworkProfile;

											decision.coldStartTargetNetworkProfile = this->knnColdStartNetworkProfile;

											decision.coldStartPauseFrameIndex = 0u;

											// ====================================================
											// decision-time kNN feature.
											// ====================================================
											decision.feature.rttMs = networkSnapshot.rttMs;

											decision.feature.lossRate = networkSnapshot.lossRate;

											decision.feature.availableBitrateBps = networkSnapshot.availableBitratebps;

											decision.feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;

											decision.feature.frameSizeBytes =
											  static_cast<double>(actionFrameSizes.sizeBytes[layer][fecIdx]);

											decision.feature.pacingDelayMs =
											  actionTimings.values[layer][fecIdx][pacingIdx].pacingDelayMs;

											for (size_t candidateLayer{ 0u }; candidateLayer < CandidateLayerCount;
											     ++candidateLayer)
											{
												auto& feature = decision.actionFeatureBySpatialLayer[candidateLayer];

												feature.rttMs = networkSnapshot.rttMs;

												feature.lossRate = networkSnapshot.lossRate;

												feature.availableBitrateBps = networkSnapshot.availableBitratebps;

												feature.congestion = networkSnapshot.camelCongestionGradientMsPerKb;

												feature.frameSizeBytes =
												  static_cast<double>(actionFrameSizes.sizeBytes[candidateLayer][fecIdx]);

												feature.pacingDelayMs =
												  actionTimings.values[candidateLayer][fecIdx][pacingIdx].pacingDelayMs;

												decision.hasActionFeatureBySpatialLayer[candidateLayer] = true;
											}

											// ====================================================
											// 기존에 추가해둔 decision-time raw diagnostics.
											// 이 부분은 삭제하면 안 됨.
											// ====================================================
											decision.aceQueueBytes = networkSnapshot.aceQueueBytes;

											decision.pacingBacklogBytes = pacerSnapshot.queuedBytes;

											decision.pacingBucketSizeBytes = pacerSnapshot.bucketCapacityBytes;

											decision.tokenRateBytesPerMs = pacerSnapshot.tokenRateBytesPerMs;

											// 아직 WebRtcTransport에는 commit하지 않고,
											// 기존과 동일하게 pending 상태로 둔다.
											this->pendingSlackActionDecisionByLogicalFrame[logicalFrameId] = decision;
										}
									}
								}
							}
						}
					}
					else
					{
						// Cold Start는 이미 끝났음.
						// 그런데 prediction도 불완전하고
						// 이전 KNN action도 아직 없는 특수 상황.
						this->knnControlActive   = false;
						this->knnMaxSpatialLayer = -1;

						// Cold Start direct force가 아직 남아있다면 해제.
						// FEC/Pacing 쪽은 기존 runtime/default 상태를 사용
						if (this->knnColdStartForceLayerActive)
						{
							this->knnColdStartForceLayerActive   = false;
							this->knnColdStartForcedSpatialLayer = -1;

							this->listener->OnConsumerNeedBitrateChange(this);
						}

						MS_WARN_TAG(
						  simulcast,
						  "[KNN-WAIT] "
						  "consumer:%s "
						  "logical:%" PRIu32 " reason=NO_COMPLETE_PREDICTION_AND_NO_PREVIOUS_ACTION",
						  this->id.c_str(),
						  logicalFrameId);
					}
				}
			}
			else
			{
				// std::cout << "[SLACK-DECISION-DUPLICATE-SKIP]"
				//           << " consumer=" << this->id << " logical=" << logicalFrameId << std::endl;
			}
		}

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.consumerId = this->id;
#endif

		auto spatialLayer = this->mapMappedSsrcSpatialLayer.at(packet->GetSsrc());

		if (!IsActive())
		{
			// Only drop the packet in the RTP sequence manager if it belongs to the
			// current spatial layer.
			if (spatialLayer == this->currentSpatialLayer)
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::CONSUMER_INACTIVE);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
			}

			return;
		}

		if (this->targetLayers.temporal == -1)
		{
			// Only drop the packet in the RTP sequence manager if it belongs to the
			// current spatial layer.
			if (spatialLayer == this->currentSpatialLayer)
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::INVALID_TARGET_LAYER);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
			}

			return;
		}

		auto payloadType = packet->GetPayloadType();

		// NOTE: This may happen if this Consumer supports just some codecs of those
		// in the corresponding Producer.
		if (!this->supportedCodecPayloadTypes[payloadType])
		{
			// Only drop the packet in the RTP sequence manager if it belongs to the
			// current spatial layer.
			if (spatialLayer == this->currentSpatialLayer)
			{
				MS_WARN_DEV("payload type not supported [payloadType:%" PRIu8 "]", payloadType);

#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::UNSUPPORTED_PAYLOAD_TYPE);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
			}

			return;
		}

		bool shouldSwitchCurrentSpatialLayer{ false };

		// Check whether this is the packet we are waiting for in order to update
		// the current spatial layer.
		// clang-format off
		// 로그가 없는 원인 1
		if (
		  this->currentSpatialLayer != this->targetLayers.spatial &&
		  spatialLayer == this->targetLayers.spatial
		)
		// clang-format on
		{
			// Ignore if not a key frame.
			if (!packet->IsKeyFrame())
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::NOT_A_KEYFRAME);
#endif

				// NOTE: Don't drop the packet in the RTP sequence manager since this
				// packet doesn't belong to the current spatial layer.

				// Store the packet for the scenario in which this packet is part of the
				// key frame and it arrived before the first packet of the key frame.
				StorePacketInTargetLayerRetransmissionBuffer(packet, sharedPacket);
				// MS_ERROR_STD("drop1");
				return;
			}

			shouldSwitchCurrentSpatialLayer = true;

			// Need to resync the stream.
			this->syncRequired       = true;
			this->spatialLayerToSync = spatialLayer;
		}
		// If the packet belongs to different spatial layer than the one being sent,
		// drop it.
		else if (spatialLayer != this->currentSpatialLayer)
		{
			// NOTE: Don't drop the packet in the RTP sequence manager since this
			// packet doesn't belong to the current spatial layer.
			// MS_ERROR_STD("drop2");
			return;
		}

		// If we need to sync and this is not a key frame, ignore the packet.
		// NOTE: syncRequired is true if packet is a key frame of the target spatial
		// layer or if transport just connected or consumer resumed.
		if (this->syncRequired && !packet->IsKeyFrame())
		{
#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::NOT_A_KEYFRAME);
#endif

			// NOTE: No need to drop the packet in the RTP sequence manager since here
			// we are blocking all packets but the key frame that would trigger sync
			// below.

			// Store the packet for the scenario in which this packet is part of the
			// key frame and it arrived before the first packet of the key frame.
			MS_ERROR_STD("drop3");
			StorePacketInTargetLayerRetransmissionBuffer(packet, sharedPacket);

			return;
		}

		// Packets with only padding are not forwarded.
		if (packet->GetPayloadLength() == 0)
		{
			// Only drop the packet in the RTP sequence manager if it belongs to the
			// current spatial layer.
			if (spatialLayer == this->currentSpatialLayer)
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::EMPTY_PAYLOAD);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
			}
			MS_ERROR_STD("drop4");
			return;
		}

		// Whether this is the first packet after re-sync.
		const bool isSyncPacket = this->syncRequired;

		// Whether packets stored in the target layer retransmission buffer must be
		// sent once this packet is sent.
		bool sendPacketsInTargetLayerRetransmissionBuffer{ false };

		// Sync sequence number and timestamp if required.
		if (isSyncPacket && (this->spatialLayerToSync == -1 || spatialLayer == this->spatialLayerToSync))
		{
			if (packet->IsKeyFrame())
			{
				MS_DEBUG_TAG(
				  rtp,
				  "sync key frame received [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32 "]",
				  packet->GetSsrc(),
				  packet->GetSequenceNumber(),
				  packet->GetTimestamp());

				sendPacketsInTargetLayerRetransmissionBuffer = true;
			}

			uint32_t tsOffset{ 0u };

			// Sync our RTP stream's RTP timestamp.
			if (spatialLayer == this->tsReferenceSpatialLayer)
			{
				tsOffset = 0u;
			}
			// If this is not the RTP stream we use as TS reference, do NTP based RTP
			// TS synchronization.
			else
			{
				auto* producerTsReferenceRtpStream = GetProducerTsReferenceRtpStream();
				auto* producerTargetRtpStream      = GetProducerTargetRtpStream();

				// NOTE: If we are here is because we have Sender Reports for both the
				// TS reference stream and the target one.
				MS_ASSERT(
				  producerTsReferenceRtpStream->GetSenderReportNtpMs(),
				  "no Sender Report for TS reference RTP stream");
				MS_ASSERT(
				  producerTargetRtpStream->GetSenderReportNtpMs(), "no Sender Report for current RTP stream");

				// Calculate NTP and TS stuff.
				auto ntpMs1 = producerTsReferenceRtpStream->GetSenderReportNtpMs();
				auto ts1    = producerTsReferenceRtpStream->GetSenderReportTs();
				auto ntpMs2 = producerTargetRtpStream->GetSenderReportNtpMs();
				auto ts2    = producerTargetRtpStream->GetSenderReportTs();
				int64_t diffMs;

				if (ntpMs2 >= ntpMs1)
				{
					diffMs = ntpMs2 - ntpMs1;
				}
				else
				{
					diffMs = -1 * (ntpMs1 - ntpMs2);
				}

				const int64_t diffTs  = diffMs * this->rtpStream->GetClockRate() / 1000;
				const uint32_t newTs2 = ts2 - diffTs;

				// Apply offset. This is the difference that later must be removed from the
				// sending RTP packet.
				tsOffset = newTs2 - ts1;
			}

			// When switching to a new stream it may happen that the timestamp of this
			// key frame is lower than the highest timestamp sent to the remote endpoint.
			// If so, apply an extra offset to "fix" it for the whole live of this selected
			// Producer stream.
			//
			// clang-format off
			if (
				shouldSwitchCurrentSpatialLayer &&
				(packet->GetTimestamp() - tsOffset <= this->rtpStream->GetMaxPacketTs())
			)
			// clang-format on
			{
				// Max delay in ms we allow for the stream when switching.
				// https://en.wikipedia.org/wiki/Audio-to-video_synchronization#Recommendations
				static const uint32_t MaxExtraOffsetMs{ 75u };

				// Outgoing packet matches the highest timestamp seen in the previous
				// stream. Apply an expected offset for a new frame in a 30fps stream.
				static const uint8_t MsOffset{ 33u }; // (1 / 30 * 1000).

				const int64_t maxTsExtraOffset = MaxExtraOffsetMs * this->rtpStream->GetClockRate() / 1000;
				uint32_t tsExtraOffset = this->rtpStream->GetMaxPacketTs() - packet->GetTimestamp() +
				                         tsOffset + (MsOffset * this->rtpStream->GetClockRate() / 1000);

				// NOTE: Don't ask for a key frame if already done.
				if (this->keyFrameForTsOffsetRequested)
				{
					// Give up and use the theoretical offset.
					if (tsExtraOffset > maxTsExtraOffset)
					{
						MS_WARN_TAG(
						  simulcast,
						  "giving up on proper stream switching after got a requested keyframe for which still too high RTP timestamp extra offset is needed (%" PRIu32
						  ")",
						  tsExtraOffset);

						tsExtraOffset = 1u;
					}
				}
				else if (tsExtraOffset > maxTsExtraOffset)
				{
					MS_WARN_TAG(
					  simulcast,
					  "cannot switch stream due to too high RTP timestamp extra offset needed (%" PRIu32
					  "), requesting keyframe",
					  tsExtraOffset);

					RequestKeyFrameForTargetSpatialLayer();

					this->keyFrameForTsOffsetRequested = true;

					// Reset flags since we are discarding this key frame.
					this->syncRequired       = false;
					this->spatialLayerToSync = -1;

#ifdef MS_RTC_LOGGER_RTP
					packet->logger.Discarded(
					  RtcLogger::RtpPacket::DiscardReason::TOO_HIGH_TIMESTAMP_EXTRA_NEEDED);
#endif

					// NOTE: Don't drop the packet in the RTP sequence manager since this
					// packet doesn't belong to the current spatial layer.

					return;
				}

				if (tsExtraOffset > 0u)
				{
					MS_DEBUG_TAG(
					  simulcast,
					  "RTP timestamp extra offset generated for stream switching: %" PRIu32,
					  tsExtraOffset);

					// Increase the timestamp offset for the whole life of this Producer stream
					// (until switched to a different one).
					tsOffset -= tsExtraOffset;
				}
			}

			this->tsOffset = tsOffset;

			// Sync our RTP stream's sequence number.
			// If previous frame has not been sent completely when we switch layer,
			// we can tell libwebrtc that previous frame is incomplete by skipping
			// one RTP sequence number.
			// 'packet->GetSequenceNumber() -2' may increase SeqManager::base and
			// increase the output sequence number.
			// https://github.com/versatica/mediasoup/issues/408
			this->rtpSeqManager.Sync(packet->GetSequenceNumber() - (this->lastSentPacketHasMarker ? 1 : 2));

			this->encodingContext->SyncRequired();

			this->syncRequired                 = false;
			this->spatialLayerToSync           = -1;
			this->keyFrameForTsOffsetRequested = false;
		}

		if (!shouldSwitchCurrentSpatialLayer && this->checkingForOldPacketsInSpatialLayer)
		{
			// If this is a packet previous to the spatial layer switch, ignore the
			// packet.
			// NOTE: We drop it in RTP sequence manager because this packet belongs
			// to current spatial layer.
			if (SeqManager<uint16_t>::IsSeqLowerThan(packet->GetSequenceNumber(), this->snReferenceSpatialLayer))
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(
				  RtcLogger::RtpPacket::DiscardReason::PACKET_PREVIOUS_TO_SPATIAL_LAYER_SWITCH);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
				MS_ERROR_STD("drop5");
				return;
			}
			else if (
			  SeqManager<uint16_t>::IsSeqHigherThan(
			    packet->GetSequenceNumber(), this->snReferenceSpatialLayer + MaxSequenceNumberGap))
			{
				this->checkingForOldPacketsInSpatialLayer = false;
			}
		}

		bool marker{ false };

		if (shouldSwitchCurrentSpatialLayer)
		{
			// Update current spatial layer.
			this->currentSpatialLayer = this->targetLayers.spatial;

			// ============================================================
			// yeon: [DIRECT LAYER CONTROL - DISABLED]
			//
			// Direct kNN layer switching에서 target keyframe의
			// 시작을 추적하던 코드.
			//
			// CAP mode에서는 layer switching을 mediasoup BWE path가
			// 관리하므로 별도의 kNN pending state를 사용하지 않는다.
			// ============================================================
			/*
			if (
			  this->knnControlActive &&
			  this->knnLayerSwitchPending &&
			  this->currentSpatialLayer ==
			    this->knnPendingTargetSpatialLayer)
			{
			    this->knnLayerSwitchFrameStarted = true;

			    this->knnLayerSwitchFrameTimestamp =
			      packet->GetTimestamp();

			    std::cout
			      << "[KNN-LAYER-SWITCH-REACHED]"
			      << " consumer=" << this->id
			      << " current=" << this->currentSpatialLayer
			      << " target=" << this->targetLayers.spatial
			      << " sourceTs="
			      << this->knnLayerSwitchFrameTimestamp
			      << std::endl;
			}
			*/

			this->snReferenceSpatialLayer = packet->GetSequenceNumber();

			this->checkingForOldPacketsInSpatialLayer = true;

			// 이하 기존 코드 그대로.
			this->encodingContext->SetTargetTemporalLayer(this->targetLayers.temporal);

			this->encodingContext->SetCurrentTemporalLayer(packet->GetTemporalLayer());

			this->rtpStream->ResetScore(10u, /*notify*/ false);

			EmitLayersChange();
			EmitScore();

			packet->ProcessPayload(this->encodingContext.get(), marker);
		}
		else
		{
			auto previousTemporalLayer = this->encodingContext->GetCurrentTemporalLayer();

			// Rewrite payload if needed. Drop packet if necessary.
			// NOTE: We drop it in RTP sequence manager because this packet belongs
			// to current spatial layer.
			if (!packet->ProcessPayload(this->encodingContext.get(), marker))
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::DROPPED_BY_CODEC);
#endif

				this->rtpSeqManager.Drop(packet->GetSequenceNumber());
				MS_ERROR_STD("drop6");
				return;
			}

			if (previousTemporalLayer != this->encodingContext->GetCurrentTemporalLayer())
			{
				EmitLayersChange();
			}
		}

		// Update RTP seq number and timestamp based on NTP offset.
		uint16_t seq;
		const uint32_t timestamp = packet->GetTimestamp() - this->tsOffset;

		this->rtpSeqManager.Input(packet->GetSequenceNumber(), seq);

		// Save original packet fields.
		auto origSsrc      = packet->GetSsrc();
		auto origSeq       = packet->GetSequenceNumber();
		auto origTimestamp = packet->GetTimestamp();

		// Rewrite packet.
		packet->SetSsrc(this->rtpParameters.encodings[0].ssrc);
		packet->SetSequenceNumber(seq);
		packet->SetTimestamp(timestamp);

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.sendRtpTimestamp = timestamp;
		packet->logger.sendSeqNumber    = seq;
#endif

		if (isSyncPacket)
		{
			MS_DEBUG_TAG(
			  rtp,
			  "sending sync packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSsrc,
			  origSeq,
			  origTimestamp);
		}

		const RTC::RtpStreamSend::ReceivePacketResult result =
		  this->rtpStream->ReceivePacket(packet, sharedPacket);

		if (result != RTC::RtpStreamSend::ReceivePacketResult::DISCARDED)
		{
			if (this->rtpSeqManager.GetMaxOutput() == packet->GetSequenceNumber())
			{
				this->lastSentPacketHasMarker = packet->HasMarker();
			}

			// ============================================================
			// yeon: cold-start decision commit.
			//
			// 여기까지 왔다는 것은:
			// - layer filtering 통과
			// - codec filtering 통과
			// - RtpStreamSend에서도 DISCARD되지 않음
			//
			// 즉 이 packet은 실제 WebRtcTransport로 내려간다.
			// ============================================================
			if (packet->HasFrameMeta())
			{
				const uint32_t logicalFrameId = packet->GetFrameMetaLogicalFrameId();

				auto decisionIt = this->pendingSlackActionDecisionByLogicalFrame.find(logicalFrameId);

				if (decisionIt != this->pendingSlackActionDecisionByLogicalFrame.end())
				{
					const auto& decision = decisionIt->second;

					// 여기까지 왔다는 것은 실제 forwarding되는 frame.
					// decisionLayer와 actualLayer가 다르더라도 decision을
					// FrameRecord 쪽으로 전달한다.
					this->listener->OnConsumerSetSlackActionDecision(this, decision);

					// std::cout << "[SLACK-ACTION-ATTACH]"
					//           << " mode=" << (decision.modelSelected ? "KNN" : "COLD")
					//           << " consumer=" << this->id << " logical=" << logicalFrameId
					//           << " decisionLayer=" << static_cast<unsigned int>(decision.spatialLayer)
					//           << " actualLayer=" << spatialLayer << " current=" << this->currentSpatialLayer
					//           << " target=" << this->targetLayers.spatial
					//           << " fecPF=" << static_cast<unsigned int>(decision.fecProtectionFactor)
					//           << " pacing=" << (decision.pacingEnabled ? "ON" : "OFF") << std::endl;

					this->pendingSlackActionDecisionByLogicalFrame.erase(decisionIt);
				}
			}

			// Send the packet.
			this->listener->OnConsumerSendRtpPacket(this, packet);

			// ============================================================
			// yeon: [DIRECT LAYER CONTROL - DISABLED]
			//
			// Direct kNN layer switching 완료 시점 추적.
			// CAP mode에서는 사용하지 않는다.
			// ============================================================
			/*
			if (
			  this->knnControlActive &&
			  this->knnLayerSwitchPending &&
			  this->knnLayerSwitchFrameStarted &&
			  this->currentSpatialLayer ==
			    this->knnPendingTargetSpatialLayer &&
			  origTimestamp ==
			    this->knnLayerSwitchFrameTimestamp &&
			  packet->HasMarker())
			{
			    std::cout
			      << "[KNN-LAYER-SWITCH-COMPLETE]"
			      << " consumer=" << this->id
			      << " current=" << this->currentSpatialLayer
			      << " target=" << this->targetLayers.spatial
			      << " sourceTs=" << origTimestamp
			      << std::endl;

			    this->knnLayerSwitchPending = false;
			    this->knnPendingTargetSpatialLayer = -1;

			    this->knnLayerSwitchFrameStarted = false;
			    this->knnLayerSwitchFrameTimestamp = 0u;
			}
			*/

			// May emit 'trace' event.
			EmitTraceEventRtpAndKeyFrameTypes(packet);
		}
		else
		{
			MS_WARN_TAG(
			  rtp,
			  "failed to send packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSsrc,
			  origSeq,
			  origTimestamp);

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::SEND_RTP_STREAM_DISCARDED);
#endif
		}

		// Restore packet fields.
		packet->SetSsrc(origSsrc);
		packet->SetSequenceNumber(origSeq);
		packet->SetTimestamp(origTimestamp);

		// Restore the original payload if needed.
		packet->RestorePayload();

		// If sharedPacket doesn't have a packet inside and it has been stored we
		// need to clone the packet into it.
		if (!sharedPacket.HasPacket() && result == RTC::RtpStreamSend::ReceivePacketResult::ACCEPTED_AND_STORED)
		{
			sharedPacket.Assign(packet);
		}

		// If sent packet was the first packet of a key frame, let's send buffered
		// packets belonging to the same key frame that arrived earlier due to
		// packet misorder.
		if (sendPacketsInTargetLayerRetransmissionBuffer)
		{
			// NOTE: Only send buffered packets if the first packet containing the key
			// frame was sent.
			if (result != RTC::RtpStreamSend::ReceivePacketResult::DISCARDED)
			{
				for (auto& kv : this->targetLayerRetransmissionBuffer)
				{
					auto& bufferedSharedPacket = kv.second;
					auto* bufferedPacket       = bufferedSharedPacket.GetPacket();

					if (bufferedPacket->GetSequenceNumber() > origSeq)
					{
						MS_DEBUG_DEV(
						  "sending packet buffered in the target layer retransmission buffer [ssrc:%" PRIu32
						  ", seq:%" PRIu16 ", ts:%" PRIu32
						  "] after sending first packet of the key frame [ssrc:%" PRIu32 ", seq:%" PRIu16
						  ", ts:%" PRIu32 "]",
						  bufferedPacket->GetSsrc(),
						  bufferedPacket->GetSequenceNumber(),
						  bufferedPacket->GetTimestamp(),
						  packet->GetSsrc(),
						  packet->GetSequenceNumber(),
						  packet->GetTimestamp());

						SendRtpPacket(bufferedPacket, bufferedSharedPacket);

						// Be sure that the target layer retransmission buffer has not been
						// emptied as a result of sending this packet. If so, exit the loop.
						if (this->targetLayerRetransmissionBuffer.empty())
						{
							MS_DEBUG_DEV(
							  "target layer retransmission buffer emptied while iterating it, exiting the loop");

							break;
						}
					}
				}
			}

			this->targetLayerRetransmissionBuffer.clear();
		}
	}

	bool SimulcastConsumer::GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs)
	{
		MS_TRACE();

		if (static_cast<float>((nowMs - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval)
		{
			return true;
		}

		auto* senderReport = this->rtpStream->GetRtcpSenderReport(nowMs);

		if (!senderReport)
		{
			return true;
		}

		// Build SDES chunk for this sender.
		auto* sdesChunk = this->rtpStream->GetRtcpSdesChunk();

		auto* delaySinceLastRrSsrcInfo = this->rtpStream->GetRtcpXrDelaySinceLastRrSsrcInfo(nowMs);

		// RTCP Compound packet buffer cannot hold the data.
		if (!packet->Add(senderReport, sdesChunk, delaySinceLastRrSsrcInfo))
		{
			return false;
		}

		this->lastRtcpSentTime = nowMs;

		return true;
	}

	void SimulcastConsumer::NeedWorstRemoteFractionLost(
	  uint32_t /*mappedSsrc*/, uint8_t& worstRemoteFractionLost)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		auto fractionLost = this->rtpStream->GetFractionLost();

		// If our fraction lost is worse than the given one, update it.
		worstRemoteFractionLost = std::max(fractionLost, worstRemoteFractionLost);
	}

	void SimulcastConsumer::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		// May emit 'trace' event.
		EmitTraceEventNackType();

		this->rtpStream->ReceiveNack(nackPacket);
	}

	void SimulcastConsumer::ReceiveKeyFrameRequest(
	  RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc)
	{
		MS_TRACE();

		switch (messageType)
		{
			case RTC::RTCP::FeedbackPs::MessageType::PLI:
			{
				EmitTraceEventPliType(ssrc);

				break;
			}

			case RTC::RTCP::FeedbackPs::MessageType::FIR:
			{
				EmitTraceEventFirType(ssrc);

				break;
			}

			default:;
		}

		this->rtpStream->ReceiveKeyFrameRequest(messageType);

		if (IsActive())
		{
			RequestKeyFrameForCurrentSpatialLayer();
		}
	}

	void SimulcastConsumer::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();
		// MS_ERROR_STD("");
		this->rtpStream->ReceiveRtcpReceiverReport(report);
	}

	void SimulcastConsumer::ReceiveRtcpXrReceiverReferenceTime(RTC::RTCP::ReceiverReferenceTime* report)
	{
		MS_TRACE();

		this->rtpStream->ReceiveRtcpXrReceiverReferenceTime(report);
	}

	uint32_t SimulcastConsumer::GetTransmissionRate(uint64_t nowMs)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return 0u;
		}

		return this->rtpStream->GetBitrate(nowMs);
	}

	float SimulcastConsumer::GetRtt() const
	{
		MS_TRACE();

		return this->rtpStream->GetRtt();
	}

	void SimulcastConsumer::UserOnTransportConnected()
	{
		MS_TRACE();

		this->syncRequired                 = true;
		this->spatialLayerToSync           = -1;
		this->keyFrameForTsOffsetRequested = false;

		if (IsActive())
		{
			MayChangeLayers();
		}
	}

	void SimulcastConsumer::UserOnTransportDisconnected()
	{
		MS_TRACE();

		this->lastBweDowngradeAtMs = 0u;

		this->rtpStream->Pause();
		this->targetLayerRetransmissionBuffer.clear();

		// CAP state reset.
		this->knnControlActive   = false;
		this->knnMaxSpatialLayer = -1;

		// Cold Start force state reset.
		this->knnColdStartForceLayerActive   = false;
		this->knnColdStartForcedSpatialLayer = -1;

		// ============================================================
		// yeon: [DIRECT LAYER CONTROL - DISABLED]
		// ============================================================
		/*
		this->knnLayerSwitchPending        = false;
		this->knnPendingTargetSpatialLayer = -1;
		this->knnLayerSwitchFrameStarted   = false;
		this->knnLayerSwitchFrameTimestamp = 0u;
		*/

		UpdateTargetLayers(-1, -1);
	}

	void SimulcastConsumer::UserOnPaused()
	{
		MS_TRACE();

		this->lastBweDowngradeAtMs = 0u;

		this->rtpStream->Pause();
		this->targetLayerRetransmissionBuffer.clear();

		// CAP state reset.
		this->knnControlActive   = false;
		this->knnMaxSpatialLayer = -1;

		// Cold Start force state reset.
		this->knnColdStartForceLayerActive   = false;
		this->knnColdStartForcedSpatialLayer = -1;

		// ============================================================
		// yeon: [DIRECT LAYER CONTROL - DISABLED]
		// ============================================================
		/*
		this->knnLayerSwitchPending        = false;
		this->knnPendingTargetSpatialLayer = -1;
		this->knnLayerSwitchFrameStarted   = false;
		this->knnLayerSwitchFrameTimestamp = 0u;
		*/

		UpdateTargetLayers(-1, -1);

		if (this->externallyManagedBitrate)
		{
			this->listener->OnConsumerNeedZeroBitrate(this);
		}
	}

	void SimulcastConsumer::UserOnResumed()
	{
		MS_TRACE();

		this->syncRequired                        = true;
		this->spatialLayerToSync                  = -1;
		this->keyFrameForTsOffsetRequested        = false;
		this->checkingForOldPacketsInSpatialLayer = false;

		if (IsActive())
		{
			MayChangeLayers();
		}
	}

	void SimulcastConsumer::CreateRtpStream()
	{
		MS_TRACE();

		auto& encoding         = this->rtpParameters.encodings[0];
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		MS_DEBUG_TAG(
		  rtp, "[ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]", encoding.ssrc, mediaCodec->payloadType);

		// Set stream params.
		RTC::RtpStream::Params params;

		params.ssrc           = encoding.ssrc;
		params.payloadType    = mediaCodec->payloadType;
		params.mimeType       = mediaCodec->mimeType;
		params.clockRate      = mediaCodec->clockRate;
		params.cname          = this->rtpParameters.rtcp.cname;
		params.spatialLayers  = encoding.spatialLayers;
		params.temporalLayers = encoding.temporalLayers;

		// Check in band FEC in codec parameters.
		if (mediaCodec->parameters.HasInteger("useinbandfec") && mediaCodec->parameters.GetInteger("useinbandfec") == 1)
		{
			MS_DEBUG_TAG(rtp, "in band FEC enabled");

			params.useInBandFec = true;
		}

		// Check DTX in codec parameters.
		if (mediaCodec->parameters.HasInteger("usedtx") && mediaCodec->parameters.GetInteger("usedtx") == 1)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		// Check DTX in the encoding.
		if (encoding.dtx)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		for (const auto& fb : mediaCodec->rtcpFeedback)
		{
			if (!params.useNack && fb.type == "nack" && fb.parameter.empty())
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "NACK supported");

				params.useNack = true;
			}
			else if (!params.usePli && fb.type == "nack" && fb.parameter == "pli")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "PLI supported");

				params.usePli = true;
			}
			else if (!params.useFir && fb.type == "ccm" && fb.parameter == "fir")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "FIR supported");

				params.useFir = true;
			}
		}

		this->rtpStream = new RTC::RtpStreamSend(this, params, this->rtpParameters.mid);
		this->rtpStreams.push_back(this->rtpStream);

		// If the Consumer is paused, tell the RtpStreamSend.
		if (IsPaused() || IsProducerPaused())
		{
			this->rtpStream->Pause();
		}

		const auto* rtxCodec = this->rtpParameters.GetRtxCodecForEncoding(encoding);

		if (rtxCodec && encoding.hasRtx)
		{
			this->rtpStream->SetRtx(rtxCodec->payloadType, encoding.rtx.ssrc);
		}
	}

	void SimulcastConsumer::RequestKeyFrames()
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return;
		}

		auto* producerTargetRtpStream  = GetProducerTargetRtpStream();
		auto* producerCurrentRtpStream = GetProducerCurrentRtpStream();

		if (producerTargetRtpStream)
		{
			auto mappedSsrc = this->consumableRtpEncodings[this->targetLayers.spatial].ssrc;

			this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc);
		}

		if (producerCurrentRtpStream && producerCurrentRtpStream != producerTargetRtpStream)
		{
			auto mappedSsrc = this->consumableRtpEncodings[this->currentSpatialLayer].ssrc;

			this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc);
		}
	}

	void SimulcastConsumer::RequestKeyFrameForTargetSpatialLayer()
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return;
		}

		auto* producerTargetRtpStream = GetProducerTargetRtpStream();

		if (!producerTargetRtpStream)
		{
			return;
		}

		auto mappedSsrc = this->consumableRtpEncodings[this->targetLayers.spatial].ssrc;

		this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc);
	}

	void SimulcastConsumer::RequestKeyFrameForCurrentSpatialLayer()
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return;
		}

		auto* producerCurrentRtpStream = GetProducerCurrentRtpStream();

		if (!producerCurrentRtpStream)
		{
			return;
		}

		auto mappedSsrc = this->consumableRtpEncodings[this->currentSpatialLayer].ssrc;

		this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc);
	}

	void SimulcastConsumer::MayChangeLayers(bool force)
	{
		MS_TRACE();

		VideoLayers newTargetLayers;

		if (RecalculateTargetLayers(newTargetLayers))
		{
			// If bitrate externally managed, don't bother the transport unless
			// the newTargetSpatialLayer has changed (or force is true).
			// This is because, if bitrate is externally managed, the target temporal
			// layer is managed by the available given bitrate so the transport
			// will let us change it when it considers.
			if (this->externallyManagedBitrate)
			{
				if (newTargetLayers.spatial != this->targetLayers.spatial || force)
				{
					this->listener->OnConsumerNeedBitrateChange(this);
				}
			}
			else
			{
				UpdateTargetLayers(newTargetLayers.spatial, newTargetLayers.temporal);
			}
		}
	}

	bool SimulcastConsumer::RecalculateTargetLayers(VideoLayers& newTargetLayers) const
	{
		MS_TRACE();

		// Start with no layers.
		newTargetLayers.Reset();

		auto nowMs = DepLibUV::GetTimeMs();

		for (size_t sIdx{ 0u }; sIdx < this->producerRtpStreams.size(); ++sIdx)
		{
			auto spatialLayer       = static_cast<int16_t>(sIdx);
			auto* producerRtpStream = this->producerRtpStreams.at(sIdx);
			auto producerScore      = producerRtpStream ? producerRtpStream->GetScore() : 0u;

			// If this is higher than current spatial layer and we moved to to current spatial
			// layer due to BWE limitations, check how much it has elapsed since then.
			if (nowMs - this->lastBweDowngradeAtMs < BweDowngradeConservativeMs)
			{
				if (newTargetLayers.spatial > -1 && spatialLayer > this->currentSpatialLayer)
				{
					continue;
				}
			}

			// Ignore spatial layers for non existing Producer streams or for those
			// with score 0.
			if (producerScore == 0u)
			{
				continue;
			}

			// If the stream has not been active time enough and we have an active one
			// already, move to the next spatial layer.
			// NOTE: Require bitrate externally managed for this.
			// clang-format off
			if (
				this->externallyManagedBitrate &&
				newTargetLayers.spatial != -1 &&
				producerRtpStream->GetActiveMs() < StreamMinActiveMs
			)
			// clang-format on
			{
				continue;
			}

			// We may not yet switch to this spatial layer.
			if (!CanSwitchToSpatialLayer(spatialLayer))
			{
				continue;
			}

			newTargetLayers.spatial = spatialLayer;

			// If this is the preferred or higher spatial layer take it and exit.
			if (spatialLayer >= this->preferredLayers.spatial)
			{
				break;
			}
		}

		if (newTargetLayers.spatial != -1)
		{
			if (newTargetLayers.spatial == this->preferredLayers.spatial)
			{
				newTargetLayers.temporal = this->preferredLayers.temporal;
			}
			else if (newTargetLayers.spatial < this->preferredLayers.spatial)
			{
				newTargetLayers.temporal = static_cast<int16_t>(this->rtpStream->GetTemporalLayers() - 1);
			}
			else
			{
				newTargetLayers.temporal = 0;
			}
		}

		// Return true if any target layer changed.
		return (newTargetLayers != this->targetLayers);
	}

	void SimulcastConsumer::UpdateTargetLayers(int16_t newTargetSpatialLayer, int16_t newTargetTemporalLayer)
	{
		MS_TRACE();

		// yeon: Cold Start spatial-layer force.
		// Cold Start exploration 동안에는
		// BWE allocator / MayChangeLayers 등이 어떤 target을 요청하더라도
		// 최종 target spatial layer는 cold-start 지정 layer로 고정한다.
		if (this->knnColdStartForceLayerActive && IsActive())
		{
			const int16_t forcedSpatialLayer = this->knnColdStartForcedSpatialLayer;

			const bool validForcedLayer =
			  forcedSpatialLayer >= 0 &&
			  static_cast<size_t>(forcedSpatialLayer) < this->producerRtpStreams.size() &&
			  this->producerRtpStreams[forcedSpatialLayer] != nullptr;

			if (validForcedLayer)
			{
				// 이미 원하는 target이라면 다시 Update하지 않는다.
				// 특히 current != target 상태에서 매번 keyframe을
				// 재요청하는 것을 방지한다.
				if (this->targetLayers.spatial == forcedSpatialLayer && this->targetLayers.temporal == 0)
				{
					return;
				}

				if (newTargetSpatialLayer != forcedSpatialLayer || newTargetTemporalLayer != 0)
				{
					MS_WARN_TAG(
					  simulcast,
					  "[KNN-COLD-LAYER-FORCE] "
					  "requested:%" PRIi16 ":%" PRIi16 " forced:%" PRIi16
					  ":0 "
					  "current:%" PRIi16 " [consumerId:%s]",
					  newTargetSpatialLayer,
					  newTargetTemporalLayer,
					  forcedSpatialLayer,
					  this->currentSpatialLayer,
					  this->id.c_str());
				}

				newTargetSpatialLayer  = forcedSpatialLayer;
				newTargetTemporalLayer = 0;
			}
		}

		// If we don't have yet a RTP timestamp reference, set it now.
		if (
		  newTargetSpatialLayer != -1 && (this->tsReferenceSpatialLayer == -1 ||
			                                !GetProducerTsReferenceRtpStream()->GetSenderReportNtpMs()))
		{
			MS_DEBUG_TAG(
			  simulcast, "using spatial layer %" PRIi16 " as RTP timestamp reference", newTargetSpatialLayer);

			this->tsReferenceSpatialLayer = newTargetSpatialLayer;
		}

		// If the new target spatial layer doesn't match the current one, clear the
		// target layer retransmission buffer.
		if (newTargetSpatialLayer != this->targetLayers.spatial)
		{
			this->targetLayerRetransmissionBuffer.clear();
		}

		if (newTargetSpatialLayer == -1)
		{
			MS_ERROR_STD("UpdateTargetLayers: -1");
			// Unset current and target layers.
			this->targetLayers.spatial  = -1;
			this->targetLayers.temporal = -1;
			this->currentSpatialLayer   = -1;

			this->encodingContext->SetTargetTemporalLayer(-1);
			this->encodingContext->SetCurrentTemporalLayer(-1);

			MS_DEBUG_TAG(
			  simulcast, "target layers changed [spatial:-1, temporal:-1, consumerId:%s]", this->id.c_str());

			EmitLayersChange();

			return;
		}

		this->targetLayers.spatial  = newTargetSpatialLayer;
		this->targetLayers.temporal = newTargetTemporalLayer;

		// If the new target spatial layer matches the current one, apply the new
		// target temporal layer now.
		if (this->targetLayers.spatial == this->currentSpatialLayer)
		{
			this->encodingContext->SetTargetTemporalLayer(this->targetLayers.temporal);
		}

		MS_DEBUG_TAG(
		  simulcast,
		  "target layers changed [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
		  this->targetLayers.spatial,
		  this->targetLayers.temporal,
		  this->id.c_str());

		// If the target spatial layer is different than the current one, request
		// a key frame.
		if (this->targetLayers.spatial != this->currentSpatialLayer)
		{
			RequestKeyFrameForTargetSpatialLayer();
		}
	}

	bool SimulcastConsumer::CanSwitchToSpatialLayer(int16_t spatialLayer) const
	{
		MS_TRACE();

		// This method assumes that the caller has verified that there is a valid
		// Producer RtpStream for the given spatial layer.
		MS_ASSERT(
		  this->producerRtpStreams.at(spatialLayer),
		  "no Producer RtpStream for the given spatialLayer:%" PRIi16,
		  spatialLayer);

		// We can switch to the given spatial layer if:
		// - we don't have any TS reference spatial layer yet, or
		// - the given spatial layer matches the TS reference spatial layer, or
		// - both , the RTP streams of our TS reference spatial layer and the given
		//   spatial layer, have Sender Report.
		//
		// clang-format off
		return (
			this->tsReferenceSpatialLayer == -1 ||
			spatialLayer == this->tsReferenceSpatialLayer ||
			this->producerRtpStreams.at(spatialLayer)->GetSenderReportNtpMs()
		);
		// clang-format on
	}

	void SimulcastConsumer::StorePacketInTargetLayerRetransmissionBuffer(
	  RTC::RtpPacket* packet, RTC::SharedRtpPacket& sharedPacket)
	{
		MS_TRACE();

		MS_DEBUG_DEV(
		  "storing packet in target layer retransmission buffer [ssrc:%" PRIu32 ", seq:%" PRIu16
		  ", ts:%" PRIu32 "]",
		  packet->GetSsrc(),
		  packet->GetSequenceNumber(),
		  packet->GetTimestamp());

		// Store original packet into the buffer. Only clone once and only if
		// necessary.
		if (!sharedPacket.HasPacket())
		{
			sharedPacket.Assign(packet);
		}
		// Assert that, if sharedPacket was already filled, both packet and
		// sharedPacket are the very same RTP packet.
		else
		{
			sharedPacket.AssertSamePacket(packet);
		}

		this->targetLayerRetransmissionBuffer[packet->GetSequenceNumber()] = sharedPacket;

		if (this->targetLayerRetransmissionBuffer.size() > TargetLayerRetransmissionBufferSize)
		{
			this->targetLayerRetransmissionBuffer.erase(this->targetLayerRetransmissionBuffer.begin());
		}
	}

	void SimulcastConsumer::EmitScore() const
	{
		MS_TRACE();

		auto scoreOffset = FillBufferScore(this->shared->channelNotifier->GetBufferBuilder());

		auto notificationOffset = FBS::Consumer::CreateScoreNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), scoreOffset);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::CONSUMER_SCORE,
		  FBS::Notification::Body::Consumer_ScoreNotification,
		  notificationOffset);
	}

	void SimulcastConsumer::EmitLayersChange() const
	{
		MS_TRACE();

		MS_DEBUG_DEV(
		  "current layers changed to [spatial:%" PRIi16 ", temporal:%" PRIi16 ", consumerId:%s]",
		  this->currentSpatialLayer,
		  this->encodingContext->GetCurrentTemporalLayer(),
		  this->id.c_str());

		flatbuffers::Offset<FBS::Consumer::ConsumerLayers> layersOffset;

		if (this->currentSpatialLayer >= 0)
		{
			layersOffset = FBS::Consumer::CreateConsumerLayers(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  this->currentSpatialLayer,
			  this->encodingContext->GetCurrentTemporalLayer());
		}

		auto notificationOffset = FBS::Consumer::CreateLayersChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), layersOffset);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::CONSUMER_LAYERS_CHANGE,
		  FBS::Notification::Body::Consumer_LayersChangeNotification,
		  notificationOffset);
	}

	RTC::RtpStreamRecv* SimulcastConsumer::GetProducerCurrentRtpStream() const
	{
		MS_TRACE();

		if (this->currentSpatialLayer == -1)
		{
			return nullptr;
		}

		// This may return nullptr.
		return this->producerRtpStreams.at(this->currentSpatialLayer);
	}

	RTC::RtpStreamRecv* SimulcastConsumer::GetProducerTargetRtpStream() const
	{
		MS_TRACE();

		if (this->targetLayers.spatial == -1)
		{
			return nullptr;
		}

		// This may return nullptr.
		return this->producerRtpStreams.at(this->targetLayers.spatial);
	}

	RTC::RtpStreamRecv* SimulcastConsumer::GetProducerTsReferenceRtpStream() const
	{
		MS_TRACE();

		if (this->tsReferenceSpatialLayer == -1)
		{
			return nullptr;
		}

		// This may return nullptr.
		return this->producerRtpStreams.at(this->tsReferenceSpatialLayer);
	}

	void SimulcastConsumer::OnRtpStreamScore(
	  RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Emit the score event.
		EmitScore();

		if (IsActive())
		{
			// Just check target layers if our bitrate is not externally managed.
			// NOTE: For now this is a bit useless since, when locally managed, we do
			// not check the Consumer score at all.
			if (!this->externallyManagedBitrate)
			{
				MayChangeLayers();
			}
		}
	}

	void SimulcastConsumer::OnRtpStreamRetransmitRtpPacket(
	  RTC::RtpStreamSend* /*rtpStream*/, RTC::RtpPacket* packet)
	{
		MS_TRACE();

		this->listener->OnConsumerRetransmitRtpPacket(this, packet);

		// May emit 'trace' event.
		EmitTraceEventRtpAndKeyFrameTypes(packet, this->rtpStream->HasRtx());
	}

	static const char* KnnColdStartNetworkProfileToString(RTC::KnnColdStartNetworkProfile profile)
	{
		switch (profile)
		{
			case RTC::KnnColdStartNetworkProfile::NORMAL:
				return "NORMAL";

			case RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT:
				return "6MBIT";

			case RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT:
				return "2MBIT";

			case RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT:
				return "4MBIT";

			case RTC::KnnColdStartNetworkProfile::LIMIT_8MBIT:
				return "8MBIT";

			case RTC::KnnColdStartNetworkProfile::LOSS_2PCT:
				return "LOSS_2PCT";

			case RTC::KnnColdStartNetworkProfile::LOSS_5PCT:
				return "LOSS_5PCT";

			case RTC::KnnColdStartNetworkProfile::LOSS_15PCT:
				return "LOSS_15PCT";
		}

		return "UNKNOWN";
	}

	bool SimulcastConsumer::GetNextKnnColdStartBalancedAction(size_t& fecIdx, size_t& pacingIdx)
	{
		for (size_t offset{ 0u }; offset < KnnColdStartActionCount; ++offset)
		{
			const size_t slot = (this->knnColdStartActionCursor + offset) % KnnColdStartActionCount;

			const size_t candidateFecIdx = slot / KnnColdStartPacingActionCount;

			const size_t candidatePacingIdx = slot % KnnColdStartPacingActionCount;

			if (this->knnColdStartPhaseActionSampleCounts[candidateFecIdx][candidatePacingIdx] >= KnnColdStartSamplesPerActionPerNetworkPhase)
			{
				continue;
			}

			fecIdx    = candidateFecIdx;
			pacingIdx = candidatePacingIdx;

			this->knnColdStartActionCursor = (slot + 1u) % KnnColdStartActionCount;

			return true;
		}

		return false;
	}

	void SimulcastConsumer::ResetKnnColdStartNetworkPhaseCoverage()
	{
		this->knnColdStartPhaseSampleCount = 0u;
		this->knnColdStartActionCursor     = 0u;

		for (auto& fecCounts : this->knnColdStartPhaseActionSampleCounts)
		{
			fecCounts.fill(0u);
		}
	}

	bool SimulcastConsumer::IsKnnColdStartNetworkPhaseComplete() const
	{
		// ============================================================
		// Legacy / non-balanced Cold Start.
		//
		// Action별 quota는 보지 않는다.
		// 현재 network condition에서 valid sample 1500개면 완료.
		// ============================================================
		if (!KnnColdStartBalancedScenarioEnabled)
		{
			return this->knnColdStartPhaseSampleCount >= KnnColdStartSamplesPerNetworkPhase;
		}

		// ============================================================
		// Balanced Cold Start.
		//
		// 10 actions x 150 samples = 1500.
		// ============================================================
		for (size_t fecIdx{ 0u }; fecIdx < KnnColdStartFecActionCount; ++fecIdx)
		{
			for (size_t pacingIdx{ 0u }; pacingIdx < KnnColdStartPacingActionCount; ++pacingIdx)
			{
				if (this->knnColdStartPhaseActionSampleCounts[fecIdx][pacingIdx] < KnnColdStartSamplesPerActionPerNetworkPhase)
				{
					return false;
				}
			}
		}

		return true;
	}

	void SimulcastConsumer::EnterKnnColdStartNetworkPause()
	{
		const auto fromProfile = this->knnColdStartNetworkProfile;

		const size_t currentIndex =
		  GetKnnNetworkProfileIndex(this->knnNetworkExperimentOption, fromProfile);

		const bool ascending = (this->knnColdStartCollectionLayer % 2) == 0;

		size_t targetIndex{ 0u };

		if (ascending)
		{
			MS_ASSERT(
			  currentIndex + 1u < this->knnNetworkProfileCount,
			  "invalid ascending KNN cold-start network transition");
			targetIndex = currentIndex + 1u;
		}
		else
		{
			MS_ASSERT(currentIndex > 0u, "invalid descending KNN cold-start network transition");
			targetIndex = currentIndex - 1u;
		}

		const auto targetProfile =
		  GetKnnNetworkProfileAt(this->knnNetworkExperimentOption, targetIndex);

		this->knnColdStartPauseTargetNetworkProfile = targetProfile;

		// ============================================================
		// Controller trigger.
		//
		// PauseStateEnabled=false여도 동일 event를 사용한다.
		// 그래서 experiment_controller.py 수정 필요 없음.
		// ============================================================
		MS_WARN_TAG(
		  simulcast,
		  "[KNN-COLD-NETWORK-PAUSE-ENTER] "
		  "consumer:%s "
		  "layer:%" PRIi16
		  " "
		  "from:%s "
		  "to:%s "
		  "pauseFrames:%zu "
		  "layerSamples:%zu/%zu "
		  "totalSamples:%zu/%zu",
		  this->id.c_str(),
		  this->knnColdStartCollectionLayer,
		  KnnColdStartNetworkProfileToString(fromProfile),
		  KnnColdStartNetworkProfileToString(targetProfile),
		  KnnExperimentPauseStateEnabled ? KnnColdStartPauseFrames : 0u,
		  this->knnColdStartLayerSampleCounts[static_cast<size_t>(this->knnColdStartCollectionLayer)],
		  this->knnColdStartSamplesPerLayer,
		  this->knnColdStartTotalSampleCount,
		  this->knnColdStartRequiredTotalSamples);

		if (KnnExperimentPauseStateEnabled)
		{
			this->knnColdStartScenarioState = KnnColdStartScenarioState::NETWORK_PAUSE;

			this->knnColdStartPauseFrameCount = 0u;

			return;
		}

		// ============================================================
		// NO-PAUSE mode.
		//
		// Controller가 위 로그를 보고 tc를 적용한다.
		// SFU 측 state는 즉시 다음 network collection으로 전환.
		// ============================================================
		this->knnColdStartNetworkProfile = targetProfile;

		ResetKnnColdStartNetworkPhaseCoverage();

		this->knnColdStartScenarioState = KnnColdStartScenarioState::COLLECT;

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-COLD-NETWORK-COLLECT-START] "
		  "consumer:%s "
		  "layer:%" PRIi16
		  " "
		  "network:%s "
		  "targetSamples:%zu "
		  "layerSamples:%zu/%zu "
		  "totalSamples:%zu/%zu "
		  "pause:DISABLED",
		  this->id.c_str(),
		  this->knnColdStartCollectionLayer,
		  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
		  KnnColdStartSamplesPerNetworkPhase,
		  this->knnColdStartLayerSampleCounts[static_cast<size_t>(this->knnColdStartCollectionLayer)],
		  this->knnColdStartSamplesPerLayer,
		  this->knnColdStartTotalSampleCount,
		  this->knnColdStartRequiredTotalSamples);
	}

	void SimulcastConsumer::EnterKnnColdStartLayerPause(int16_t nextLayer)
	{
		// ============================================================
		// Pause-enabled mode.
		//
		// 기존 Balanced Cold Start 실험 동작을 그대로 유지
		//   L0 -> L1 또는 L1 -> L2
		//   150 logical-frame LAYER_PAUSE
		//   tc는 변경하지 않음
		// ============================================================
		if (KnnExperimentPauseStateEnabled)
		{
			const int16_t previousLayer = this->knnColdStartCollectionLayer;

			this->knnColdStartScenarioState = KnnColdStartScenarioState::LAYER_PAUSE;

			this->knnColdStartPauseFrameCount       = 0u;
			this->knnColdStartStableLayerFrameCount = 0u;

			// tc state는 그대로 유지.
			this->knnColdStartPauseTargetNetworkProfile = this->knnColdStartNetworkProfile;

			this->knnColdStartCollectionLayer = nextLayer;

			this->knnColdStartForcedSpatialLayer = nextLayer;

			this->knnColdStartForceLayerActive = true;

			const bool validNextLayer = nextLayer >= 0 &&
			                            static_cast<size_t>(nextLayer) < this->producerRtpStreams.size() &&
			                            this->producerRtpStreams[nextLayer] != nullptr;

			if (validNextLayer)
			{
				UpdateTargetLayers(nextLayer, 0);

				this->listener->OnConsumerNeedBitrateChange(this);
			}

			MS_WARN_TAG(
			  simulcast,
			  "[KNN-COLD-LAYER-PAUSE-ENTER] "
			  "consumer:%s "
			  "fromLayer:%" PRIi16
			  " "
			  "toLayer:%" PRIi16
			  " "
			  "network:%s "
			  "pauseFrames:%zu "
			  "totalSamples:%zu/%zu",
			  this->id.c_str(),
			  previousLayer,
			  nextLayer,
			  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
			  KnnColdStartPauseFrames,
			  this->knnColdStartTotalSampleCount,
			  this->knnColdStartRequiredTotalSamples);

			return;
		}

		// ============================================================
		// No-pause mode.
		//
		// Layer transition은 즉시 수행한다.
		//
		// 예:
		//   L0 / 2MBIT collection 완료
		//       ↓
		//   즉시 L1로 target 변경
		//       ↓
		//   tc는 그대로 2MBIT
		//       ↓
		//   L1 collection 시작
		//
		// 별도의 150-frame LAYER_PAUSE는 없다.
		//
		// 단, 기존 KnnColdStartLayerSettleFrames는 그대로 유지한다.
		// 이것은 experiment pause가 아니라 실제 layer가 target에
		// 도달했는지 확인하기 위한 기존 validity/stabilization gate이다.
		// ============================================================

		const int16_t previousLayer = this->knnColdStartCollectionLayer;

		// pause state로 들어가지 않는다.
		this->knnColdStartScenarioState = KnnColdStartScenarioState::COLLECT;

		this->knnColdStartPauseFrameCount = 0u;

		// Layer 변경 시에는 network/tc를 절대 바꾸지 않는다.
		this->knnColdStartPauseTargetNetworkProfile = this->knnColdStartNetworkProfile;

		// 새로운 collection layer.
		this->knnColdStartCollectionLayer = nextLayer;

		this->knnColdStartForcedSpatialLayer = nextLayer;

		this->knnColdStartForceLayerActive = true;

		// 새 layer에 실제로 도달한 뒤 기존 settle 조건을
		// 처음부터 다시 확인한다.
		this->knnColdStartStableLayerFrameCount = 0u;

		// 이전 layer의 second network phase count를 제거하고,
		// 새 layer의 첫 network phase count를 0부터 시작한다.
		ResetKnnColdStartNetworkPhaseCoverage();

		const bool validNextLayer = nextLayer >= 0 &&
		                            static_cast<size_t>(nextLayer) < this->producerRtpStreams.size() &&
		                            this->producerRtpStreams[nextLayer] != nullptr;

		if (validNextLayer)
		{
			UpdateTargetLayers(nextLayer, 0);

			this->listener->OnConsumerNeedBitrateChange(this);
		}

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-COLD-LAYER-ENTER] "
		  "consumer:%s "
		  "fromLayer:%" PRIi16
		  " "
		  "layer:%" PRIi16
		  " "
		  "network:%s "
		  "pause:DISABLED "
		  "targetSamples:%zu "
		  "totalSamples:%zu/%zu",
		  this->id.c_str(),
		  previousLayer,
		  nextLayer,
		  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
		  this->knnColdStartSamplesPerLayer,
		  this->knnColdStartTotalSampleCount,
		  this->knnColdStartRequiredTotalSamples);
	}

	void SimulcastConsumer::BeginKnnEvaluation()
	{
		if (this->knnEvaluationScenarioState != KnnEvaluationScenarioState::NOT_STARTED)
		{
			return;
		}

		// Cold Start의 마지막 network를 현재 상태로 이어받는다.
		// 실제 KNN collection은 항상 NORMAL부터 시작한다.
		this->knnEvaluationNetworkProfile            = this->knnColdStartNetworkProfile;
		this->knnEvaluationPauseTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::NORMAL;

		this->knnEvaluationPhaseFrameCount    = 0u;
		this->knnEvaluationNormalFrameCount   = 0u;
		this->knnEvaluation6MbitFrameCount    = 0u;
		this->knnEvaluation2MbitFrameCount    = 0u;
		this->knnEvaluation4MbitFrameCount    = 0u;
		this->knnEvaluation8MbitFrameCount    = 0u;
		this->knnEvaluationLoss2PctFrameCount  = 0u;
		this->knnEvaluationLoss5PctFrameCount  = 0u;
		this->knnEvaluationLoss15PctFrameCount = 0u;

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-EVAL-START] "
		  "consumer:%s "
		  "option:%c "
		  "profiles:%zu "
		  "fromColdNetwork:%s "
		  "pauseFrames:%zu "
		  "collectionFramesPerNetwork:%zu",
		  this->id.c_str(),
		  KnnNetworkExperimentOptionToChar(this->knnNetworkExperimentOption),
		  this->knnNetworkProfileCount,
		  KnnColdStartNetworkProfileToString(this->knnEvaluationNetworkProfile),
		  KnnExperimentPauseStateEnabled ? KnnEvaluationPauseFrames : 0u,
		  KnnEvaluationCollectionFrames);

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-EVAL-NETWORK-PAUSE-ENTER] "
		  "consumer:%s "
		  "phase:START "
		  "from:%s "
		  "to:NORMAL "
		  "pauseFrames:%zu",
		  this->id.c_str(),
		  KnnColdStartNetworkProfileToString(this->knnEvaluationNetworkProfile),
		  KnnExperimentPauseStateEnabled ? KnnEvaluationPauseFrames : 0u);

		if (KnnExperimentPauseStateEnabled)
		{
			this->knnEvaluationScenarioState = KnnEvaluationScenarioState::START_NETWORK_PAUSE;
			return;
		}

		// No-pause mode: controller still receives the transition event above.
		this->knnEvaluationNetworkProfile = RTC::KnnColdStartNetworkProfile::NORMAL;
		this->knnEvaluationScenarioState  = KnnEvaluationScenarioState::COLLECT_NORMAL;

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-EVAL-COLLECT-ENTER] consumer:%s phase:NORMAL network:NORMAL targetFrames:%zu pause:DISABLED",
		  this->id.c_str(),
		  KnnEvaluationCollectionFrames);
	}

	void SimulcastConsumer::AnnotateAndAdvanceKnnEvaluationDecision(RTC::SlackActionDecision& decision)
	{
		decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::NONE;
		decision.knnEvaluationNetworkProfile       = this->knnEvaluationNetworkProfile;
		decision.knnEvaluationTargetNetworkProfile = this->knnEvaluationNetworkProfile;
		decision.knnEvaluationPhaseFrameIndex      = 0u;
		decision.knnEvaluationTerminalFrame        = false;

		auto enterCollectStateForProfile = [this](RTC::KnnColdStartNetworkProfile profile)
		{
			switch (profile)
			{
				case RTC::KnnColdStartNetworkProfile::NORMAL:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_NORMAL;
					break;
				case RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_6MBIT;
					break;
				case RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_2MBIT;
					break;
				case RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_4MBIT;
					break;
				case RTC::KnnColdStartNetworkProfile::LIMIT_8MBIT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_8MBIT;
					break;
				case RTC::KnnColdStartNetworkProfile::LOSS_2PCT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_LOSS_2PCT;
					break;
				case RTC::KnnColdStartNetworkProfile::LOSS_5PCT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_LOSS_5PCT;
					break;
				case RTC::KnnColdStartNetworkProfile::LOSS_15PCT:
					this->knnEvaluationScenarioState = KnnEvaluationScenarioState::COLLECT_LOSS_15PCT;
					break;
			}
		};

		auto scheduleNextProfile =
		  [this, &enterCollectStateForProfile](RTC::KnnColdStartNetworkProfile targetProfile)
		{
			this->knnEvaluationPauseTargetNetworkProfile = targetProfile;
			this->knnEvaluationPhaseFrameCount           = 0u;

			if (KnnExperimentPauseStateEnabled)
			{
				this->knnEvaluationScenarioState = KnnEvaluationScenarioState::MID_NETWORK_PAUSE;
				return;
			}

			MS_WARN_TAG(
			  simulcast,
			  "[KNN-EVAL-NETWORK-PAUSE-ENTER] consumer:%s phase:MID from:%s to:%s pauseFrames:0",
			  this->id.c_str(),
			  KnnColdStartNetworkProfileToString(this->knnEvaluationNetworkProfile),
			  KnnColdStartNetworkProfileToString(targetProfile));

			this->knnEvaluationNetworkProfile = targetProfile;
			enterCollectStateForProfile(targetProfile);

			MS_WARN_TAG(
			  simulcast,
			  "[KNN-EVAL-COLLECT-ENTER] consumer:%s network:%s targetFrames:%zu pause:DISABLED",
			  this->id.c_str(),
			  KnnColdStartNetworkProfileToString(targetProfile),
			  KnnEvaluationCollectionFrames);
		};

		auto finishEvaluation = [this, &decision](const char* phaseName, size_t frameCount)
		{
			decision.knnEvaluationTerminalFrame = true;
			this->knnEvaluationScenarioState    = KnnEvaluationScenarioState::COMPLETE;
			this->knnEvaluationPhaseFrameCount  = 0u;

			MS_WARN_TAG(
			  simulcast,
			  "[KNN-EVAL-COLLECT-DONE] consumer:%s phase:%s frames:%zu/%zu logical:%" PRIu32
			  " waitingForFinalRecord:1",
			  this->id.c_str(),
			  phaseName,
			  frameCount,
			  KnnEvaluationCollectionFrames,
			  decision.logicalFrameId);
		};

		auto advanceOrFinishEvaluation =
		  [this, &scheduleNextProfile, &finishEvaluation](
		    RTC::KnnColdStartNetworkProfile profile, const char* phaseName, size_t frameCount)
		{
			const size_t currentIndex =
			  GetKnnNetworkProfileIndex(this->knnNetworkExperimentOption, profile);

			if (currentIndex + 1u >= this->knnNetworkProfileCount)
			{
				finishEvaluation(phaseName, frameCount);
				return;
			}

			MS_WARN_TAG(
			  simulcast,
			  "[KNN-EVAL-COLLECT-DONE] consumer:%s phase:%s frames:%zu/%zu",
			  this->id.c_str(),
			  phaseName,
			  frameCount,
			  KnnEvaluationCollectionFrames);

			const auto nextProfile =
			  GetKnnNetworkProfileAt(this->knnNetworkExperimentOption, currentIndex + 1u);

			scheduleNextProfile(nextProfile);
		};

		switch (this->knnEvaluationScenarioState)
		{
			case KnnEvaluationScenarioState::NOT_STARTED:
			{
				break;
			}

			case KnnEvaluationScenarioState::START_NETWORK_PAUSE:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::START_NETWORK_PAUSE;
				decision.knnEvaluationNetworkProfile       = this->knnEvaluationNetworkProfile;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::NORMAL;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = true;

				++this->knnEvaluationPhaseFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationPauseFrames)
				{
					this->knnEvaluationNetworkProfile  = RTC::KnnColdStartNetworkProfile::NORMAL;
					this->knnEvaluationScenarioState   = KnnEvaluationScenarioState::COLLECT_NORMAL;
					this->knnEvaluationPhaseFrameCount = 0u;

					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-NETWORK-PAUSE-EXIT] consumer:%s phase:START network:NORMAL pauseFrames:%zu",
					  this->id.c_str(),
					  KnnEvaluationPauseFrames);

					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-COLLECT-ENTER] consumer:%s phase:NORMAL network:NORMAL targetFrames:%zu",
					  this->id.c_str(),
					  KnnEvaluationCollectionFrames);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_NORMAL:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_NORMAL;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::NORMAL;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::NORMAL;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluationNormalFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-COLLECT-DONE] consumer:%s phase:NORMAL frames:%zu/%zu",
					  this->id.c_str(),
					  this->knnEvaluationNormalFrameCount,
					  KnnEvaluationCollectionFrames);

					MS_ASSERT(this->knnNetworkProfileCount > 1u, "KNN evaluation requires at least 2 profiles");

					scheduleNextProfile(GetKnnNetworkProfileAt(this->knnNetworkExperimentOption, 1u));
				}

				break;
			}

			case KnnEvaluationScenarioState::MID_NETWORK_PAUSE:
			{
				const auto fromProfile   = this->knnEvaluationNetworkProfile;
				const auto targetProfile = this->knnEvaluationPauseTargetNetworkProfile;

				if (this->knnEvaluationPhaseFrameCount == 0u)
				{
					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-NETWORK-PAUSE-ENTER] consumer:%s phase:MID from:%s to:%s pauseFrames:%zu",
					  this->id.c_str(),
					  KnnColdStartNetworkProfileToString(fromProfile),
					  KnnColdStartNetworkProfileToString(targetProfile),
					  KnnEvaluationPauseFrames);
				}

				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::MID_NETWORK_PAUSE;
				decision.knnEvaluationNetworkProfile       = fromProfile;
				decision.knnEvaluationTargetNetworkProfile = targetProfile;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = true;

				++this->knnEvaluationPhaseFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationPauseFrames)
				{
					this->knnEvaluationNetworkProfile  = targetProfile;
					this->knnEvaluationPhaseFrameCount = 0u;
					enterCollectStateForProfile(targetProfile);

					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-NETWORK-PAUSE-EXIT] consumer:%s phase:MID network:%s pauseFrames:%zu",
					  this->id.c_str(),
					  KnnColdStartNetworkProfileToString(targetProfile),
					  KnnEvaluationPauseFrames);

					MS_WARN_TAG(
					  simulcast,
					  "[KNN-EVAL-COLLECT-ENTER] consumer:%s network:%s targetFrames:%zu",
					  this->id.c_str(),
					  KnnColdStartNetworkProfileToString(targetProfile),
					  KnnEvaluationCollectionFrames);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_6MBIT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_6MBIT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluation6MbitFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT,
					  "6MBIT",
					  this->knnEvaluation6MbitFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_2MBIT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_2MBIT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluation2MbitFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT,
					  "2MBIT",
					  this->knnEvaluation2MbitFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_4MBIT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_4MBIT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluation4MbitFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT,
					  "4MBIT",
					  this->knnEvaluation4MbitFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_8MBIT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_8MBIT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LIMIT_8MBIT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LIMIT_8MBIT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluation8MbitFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					// LIMIT_8MBIT is retained only for legacy compatibility.
					finishEvaluation("8MBIT", this->knnEvaluation8MbitFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_LOSS_2PCT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_LOSS_2PCT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LOSS_2PCT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LOSS_2PCT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluationLoss2PctFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LOSS_2PCT,
					  "LOSS_2PCT",
					  this->knnEvaluationLoss2PctFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_LOSS_5PCT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_LOSS_5PCT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LOSS_5PCT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LOSS_5PCT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluationLoss5PctFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LOSS_5PCT,
					  "LOSS_5PCT",
					  this->knnEvaluationLoss5PctFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COLLECT_LOSS_15PCT:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COLLECT_LOSS_15PCT;
				decision.knnEvaluationNetworkProfile       = RTC::KnnColdStartNetworkProfile::LOSS_15PCT;
				decision.knnEvaluationTargetNetworkProfile = RTC::KnnColdStartNetworkProfile::LOSS_15PCT;
				decision.knnEvaluationPhaseFrameIndex      = this->knnEvaluationPhaseFrameCount + 1u;
				decision.skipKnnTrainingSample             = false;

				++this->knnEvaluationPhaseFrameCount;
				++this->knnEvaluationLoss15PctFrameCount;

				if (this->knnEvaluationPhaseFrameCount >= KnnEvaluationCollectionFrames)
				{
					advanceOrFinishEvaluation(
					  RTC::KnnColdStartNetworkProfile::LOSS_15PCT,
					  "LOSS_15PCT",
					  this->knnEvaluationLoss15PctFrameCount);
				}

				break;
			}

			case KnnEvaluationScenarioState::COMPLETE:
			{
				decision.knnEvaluationPhase                = RTC::KnnEvaluationPhase::COMPLETE;
				decision.knnEvaluationNetworkProfile       = this->knnEvaluationNetworkProfile;
				decision.knnEvaluationTargetNetworkProfile = this->knnEvaluationNetworkProfile;
				decision.knnEvaluationPhaseFrameIndex      = 0u;
				decision.knnEvaluationTerminalFrame = true;  // 추가
				decision.skipKnnTrainingSample             = true;
				break;
			}
		}
	}

	void SimulcastConsumer::CompleteKnnColdStart()
	{
		const bool allLayersComplete =
		  this->knnColdStartLayerSampleCounts[0] >= this->knnColdStartSamplesPerLayer &&
		  this->knnColdStartLayerSampleCounts[1] >= this->knnColdStartSamplesPerLayer &&
		  this->knnColdStartLayerSampleCounts[2] >= this->knnColdStartSamplesPerLayer;

		const bool enoughTotalSamples =
		  this->knnColdStartTotalSampleCount >= this->knnColdStartRequiredTotalSamples;

		if (!allLayersComplete || !enoughTotalSamples)
		{
			MS_WARN_TAG(
			  simulcast,
			  "[KNN-COLD-COMPLETE-REJECT] consumer:%s L0:%zu L1:%zu L2:%zu total:%zu/%zu",
			  this->id.c_str(),
			  this->knnColdStartLayerSampleCounts[0],
			  this->knnColdStartLayerSampleCounts[1],
			  this->knnColdStartLayerSampleCounts[2],
			  this->knnColdStartTotalSampleCount,
			  this->knnColdStartRequiredTotalSamples);
			return;
		}

		this->knnColdStartCollectionComplete = true;
		this->knnColdStartScenarioState      = KnnColdStartScenarioState::COMPLETE;
		this->knnColdStartPauseFrameCount    = 0u;

		// Release the direct cold-start layer force. The next logical frame will
		// enter the normal KNN/CAP path.
		this->knnColdStartForceLayerActive   = false;
		this->knnColdStartForcedSpatialLayer = -1;

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-COLD-COLLECTION-COMPLETE] "
		  "consumer:%s L0:%zu L1:%zu L2:%zu total:%zu/%zu network:%s KNN_READY",
		  this->id.c_str(),
		  this->knnColdStartLayerSampleCounts[0],
		  this->knnColdStartLayerSampleCounts[1],
		  this->knnColdStartLayerSampleCounts[2],
		  this->knnColdStartTotalSampleCount,
		  this->knnColdStartRequiredTotalSamples,
		  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile));

		// ============================================================
		// Cold Start가 끝났으므로 KNN evaluation 시작.
		//
		// 현재 network는 option의 마지막 profile이다.
		// Controller는 BeginKnnEvaluation()에서 출력되는
		// [KNN-EVAL-NETWORK-PAUSE-ENTER]를 보고 NORMAL로 복원한다.
		// ============================================================

		BeginKnnEvaluation();

		this->listener->OnConsumerNeedBitrateChange(this);
	}

	void SimulcastConsumer::OnKnnColdStartTrainingSampleAdded(
	  int16_t spatialLayer, uint8_t fecProtectionFactor, bool pacingEnabled)
	{
		if (!ShouldAcceptKnnColdStartTrainingSample(spatialLayer, fecProtectionFactor, pacingEnabled))
		{
			return;
		}

		const size_t layer = static_cast<size_t>(spatialLayer);

		// ============================================================
		// Balanced mode에서만 action별 count를 증가.
		// ============================================================
		if (KnnColdStartBalancedScenarioEnabled)
		{
			size_t fecIdx{ 0u };

			if (!GetFecCandidateIndex(fecProtectionFactor, fecIdx))
			{
				return;
			}

			const size_t pacingIdx = pacingEnabled ? 1u : 0u;

			++this->knnColdStartPhaseActionSampleCounts[fecIdx][pacingIdx];

			++this->knnColdStartLayerActionSampleCounts[layer][fecIdx][pacingIdx];
		}

		// ============================================================
		// Balanced / Legacy 공통 counters.
		// ============================================================
		++this->knnColdStartPhaseSampleCount;
		++this->knnColdStartLayerSampleCounts[layer];
		++this->knnColdStartTotalSampleCount;

		if (!IsKnnColdStartNetworkPhaseComplete())
		{
			return;
		}

		MS_WARN_TAG(
		  simulcast,
		  "[KNN-COLD-NETWORK-COLLECT-DONE] "
		  "consumer:%s "
		  "layer:%zu "
		  "network:%s "
		  "phaseSamples:%zu "
		  "layerSamples:%zu/%zu "
		  "total:%zu/%zu",
		  this->id.c_str(),
		  layer,
		  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
		  this->knnColdStartPhaseSampleCount,
		  this->knnColdStartLayerSampleCounts[layer],
		  this->knnColdStartSamplesPerLayer,
		  this->knnColdStartTotalSampleCount,
		  this->knnColdStartRequiredTotalSamples);

		// ============================================================
		// 매우 중요:
		//
		// layer 완료 검사를 network switch보다 먼저 한다.
		//
		// 현재 option의 마지막 network phase가 끝난 뒤 또 network를 바꾸면 안 된다.
		// ============================================================
		if (this->knnColdStartLayerSampleCounts[layer] >= this->knnColdStartSamplesPerLayer)
		{
			MS_WARN_TAG(
			  simulcast,
			  "[KNN-COLD-LAYER-DONE] "
			  "consumer:%s "
			  "layer:%zu "
			  "network:%s "
			  "samples:%zu/%zu "
			  "total:%zu/%zu",
			  this->id.c_str(),
			  layer,
			  KnnColdStartNetworkProfileToString(this->knnColdStartNetworkProfile),
			  this->knnColdStartLayerSampleCounts[layer],
			  this->knnColdStartSamplesPerLayer,
			  this->knnColdStartTotalSampleCount,
			  this->knnColdStartRequiredTotalSamples);

			if (spatialLayer < 2)
			{
				EnterKnnColdStartLayerPause(spatialLayer + 1);

				return;
			}

			CompleteKnnColdStart();

			return;
		}

		// ============================================================
		// 여기까지 왔다는 것은:
		//
		// 현재 layer 첫 1500 samples 완료.
		// network만 toggle.
		// ============================================================
		EnterKnnColdStartNetworkPause();
	}

	bool SimulcastConsumer::ShouldAcceptKnnColdStartTrainingSample(
	  int16_t spatialLayer, uint8_t fecProtectionFactor, bool pacingEnabled) const
	{
		if (this->knnColdStartCollectionComplete)
		{
			return false;
		}

		if (spatialLayer < 0 || spatialLayer >= 3)
		{
			return false;
		}

		if (spatialLayer != this->knnColdStartCollectionLayer)
		{
			return false;
		}

		const size_t layer = static_cast<size_t>(spatialLayer);

		if (this->knnColdStartLayerSampleCounts[layer] >= this->knnColdStartSamplesPerLayer)
		{
			return false;
		}

		// ============================================================
		// Explicit pause를 사용하는 실험에서만 pause 동안 제외.
		//
		// PauseStateEnabled=false이면 state가 애초에 pause로
		// 들어가지 않으므로 바로 계속 collection된다.
		// ============================================================
		if (KnnExperimentPauseStateEnabled && this->knnColdStartScenarioState != KnnColdStartScenarioState::COLLECT)
		{
			return false;
		}

		// ============================================================
		// Legacy Cold Start.
		//
		// action별 quota 없음.
		// 현재 network phase에서 1500개까지만 수집.
		// ============================================================
		if (!KnnColdStartBalancedScenarioEnabled)
		{
			return this->knnColdStartPhaseSampleCount < KnnColdStartSamplesPerNetworkPhase;
		}

		// ============================================================
		// Balanced Cold Start.
		// ============================================================
		size_t fecIdx{ 0u };

		if (!GetFecCandidateIndex(fecProtectionFactor, fecIdx))
		{
			return false;
		}

		const size_t pacingIdx = pacingEnabled ? 1u : 0u;

		return this->knnColdStartPhaseActionSampleCounts[fecIdx][pacingIdx] <
		       KnnColdStartSamplesPerActionPerNetworkPhase;
	}

} // namespace RTC
