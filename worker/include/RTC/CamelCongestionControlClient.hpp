#ifndef MS_RTC_CAMEL_CONGESTION_CONTROL_CLIENT_HPP
#define MS_RTC_CAMEL_CONGESTION_CONTROL_CLIENT_HPP

#include "RTC/RTCP/FeedbackRtpTransport.hpp"
#include <libwebrtc/modules/rtp_rtcp/include/rtp_rtcp_defines.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

namespace RTC
{
	class RtpPacket;
	class Consumer;

	namespace RTCP
	{
		class ReceiverReportPacket;
	}

	class CamelCongestionControlClient
	{
	private:
		struct FrameKey
		{
			uint32_t ssrc{ 0u };
			uint32_t rtpTimestamp{ 0u };

			bool operator==(const FrameKey& other) const
			{
				return this->ssrc == other.ssrc && this->rtpTimestamp == other.rtpTimestamp;
			}
		};

		struct FrameKeyHasher
		{
			std::size_t operator()(const FrameKey& key) const
			{
				return std::hash<uint32_t>{}(key.ssrc) ^ (std::hash<uint32_t>{}(key.rtpTimestamp) << 1);
			}
		};

		struct PacketRecord
		{
			uint16_t wideSeq{ 0u };

			uint32_t ssrc{ 0u };
			uint32_t rtpTimestamp{ 0u };
			uint16_t rtpSequenceNumber{ 0u };

			size_t sizeBytes{ 0u };
			size_t offsetInFrameBytes{ 0u };

			bool marker{ false };

			int64_t insertTimeMs{ 0 };
			int64_t sentTimeMs{ 0 };

			bool feedbackSeen{ false };
			bool received{ false };
			bool hasRecvTime{ false };
			int64_t recvTimeMs{ 0 };
			int64_t recvTimeUs{ 0 };

			// 피드백이 왔는지 확인하는 플래그 (inflight 판정)
			bool inFlightCounted{ false };
		};

		struct FrameState
		{
			FrameKey key;

			uint16_t firstWideSeq{ 0u };
			uint16_t lastWideSeq{ 0u };

			size_t packetCount{ 0u };
			size_t frameSizeBytes{ 0u };

			size_t feedbackedPackets{ 0u };
			size_t receivedPackets{ 0u };
			size_t lostPackets{ 0u };

			bool markerSeen{ false };
			bool finalized{ false };

			int64_t firstSentTimeMs{ 0 };
			int64_t lastSentTimeMs{ 0 };

			int64_t firstRecvTimeMs{ 0 };
			int64_t lastRecvTimeMs{ 0 };

			int64_t lastUpdateMs{ 0 };

			// congestion detector
			size_t inflightBytesAtFirstSent{ 0u };
		};

		struct FrameSample
		{
			FrameKey key;

			double bandwidthBps{ 0.0 };
			double delayMs{ 0.0 };
			double bdpBytes{ 0.0 };

			size_t frameSizeBytes{ 0u };
			size_t packetCount{ 0u };

			int64_t firstRecvTimeMs{ 0 };
			int64_t firstRecvTimeUs{ 0 };
			int64_t lastRecvTimeMs{ 0 };
			int64_t lastRecvTimeUs{ 0 };
			int64_t sampleTimeMs{ 0 };

			// congestion detector
			double inflightBytes{ 0.0 };
		};

		struct BurstLossSample
		{
			int64_t sampleTimeMs{ 0 };

			// 2KB, 4KB, 6KB ... 처럼 packet이 속한 frame offset interval의 upper bound.
			size_t intervalUpperBytes{ 0u };

			bool lost{ false };
		};

	public:
		struct Bitrates
		{
			uint32_t availableBitrate{ 0u };
			uint32_t desiredBitrate{ 0u };
			uint32_t minBitrate{ 0u };
			uint32_t maxBitrate{ 0u };

			// Camel-specific values. 지금은 stub.
			double packetLoss{ 0.0 };
			double avgFrameBandwidthBps{ 0.0 };
			double minFrameDelayMs{ 0.0 };
			double bdpBytes{ 0.0 };
			double gamma{ 1.0 };
			double latestRttMs{ 0.0 };
			uint32_t burstLengthBytes{ 0u };

			// congestion detector
			double inflightBytes{ 0.0 };
			double delayInflightGradientMsPerKb{ 0.0 };
			double cwndBytes{ 0.0 };
			bool congested{ false };

			// Bursting Length Controller.
			bool fallbackToGcc{ false };
			double burstLossRate{ 0.0 };
			double baselineLossRate{ 0.0 };
		};

		class Listener
		{
		public:
			virtual void OnCamelCongestionControlClientBitrates(
			  RTC::CamelCongestionControlClient* camelClient,
			  RTC::CamelCongestionControlClient::Bitrates& bitrates) = 0;
		};

	public:
		CamelCongestionControlClient(
		  Listener* listener,
		  uint32_t initialAvailableBitrate,
		  uint32_t maxOutgoingBitrate,
		  uint32_t minOutgoingBitrate);

		~CamelCongestionControlClient() = default;

	public:
		void InsertPacket(
		  const webrtc::RtpPacketSendInfo& packetInfo,
		  RTC::RtpPacket* packet,
		  RTC::Consumer* consumer,
		  int64_t nowMs);

		void PacketSent(const webrtc::RtpPacketSendInfo& packetInfo, int64_t nowMs);

		void ReceiveRtcpTransportFeedback(const RTC::RTCP::FeedbackRtpTransportPacket* feedback);
		void ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReportPacket* packet, float rtt, int64_t nowMs);

		void SetDesiredBitrate(uint32_t desiredBitrate, bool force = false);
		void SetMaxOutgoingBitrate(uint32_t maxBitrate);
		void SetMinOutgoingBitrate(uint32_t minBitrate);

		uint32_t GetAvailableBitrate() const
		{
			return this->bitrates.availableBitrate;
		}

		uint32_t GetBurstLengthBytes() const
		{
			return this->bitrates.burstLengthBytes;
		}

		double GetPacketLoss() const
		{
			return this->bitrates.packetLoss;
		}

		const Bitrates& GetBitrates() const
		{
			return this->bitrates;
		}

	private:
		void MaybeClampAvailableBitrate();
		void PruneOldRecords(int64_t nowMs);
		void MaybeFinalizeFrame(const FrameKey& frameKey, int64_t nowMs);
		void RecomputeEstimates(int64_t nowMs);
		void PruneFrameSamples(int64_t nowMs);
		void EraseFrameRecords(const FrameKey& frameKey);
		void UpdateCongestionDetector();
		void AddBurstLossSample(const PacketRecord& record, bool lost, int64_t nowMs);
		void PruneBurstLossSamples(int64_t nowMs);
		void UpdateBurstingLengthController(int64_t nowMs);

	private:
		Listener* listener{ nullptr };
		Bitrates bitrates;
		static constexpr int64_t PacketRecordTtlMs{ 10000 };
		static constexpr int64_t FrameRecordTtlMs{ 10000 };

		std::unordered_map<uint16_t, PacketRecord> packetRecords;
		std::unordered_map<FrameKey, FrameState, FrameKeyHasher> frameStates;
		std::unordered_map<FrameKey, size_t, FrameKeyHasher> frameBytesSoFar;

		static constexpr int64_t EstimateWindowMs{ 5000 };
		std::deque<FrameSample> frameSamples;

		static constexpr double GammaDecreaseFactor{ 0.95 };
		static constexpr double MinGamma{ 0.50 };

		// 논문은 predefined threshold라고만 설명하고,
		// 구체 값은 구현 환경별 튜닝 영역으로 둔다.
		// 단위: ms / KB.
		static constexpr double CongestionGradientThresholdMsPerKb{ 0.0 };

		size_t outstandingBytes{ 0u };

		static constexpr size_t BurstIntervalBytes{ 2048u };
		static constexpr size_t MinBurstLengthBytes{ 2048u };
		static constexpr size_t MaxBurstLengthBytes{ 32768u };
		static constexpr size_t InitialBurstLengthBytes{ 12000u };

		// 논문에서는 Li > L0 + 0.1이면 burst length를 줄이는 식으로 설명됨.
		// 여기서는 0.1 = 10 percentage points로 둔다.
		static constexpr double BurstLossExtraThreshold{ 0.10 };

		// min burst에서도 이 정도 tail loss가 계속 나오면 shallow buffer로 보고 fallback flag를 세움.
		static constexpr double ShallowBufferLossThreshold{ 0.10 };

		std::deque<BurstLossSample> burstLossSamples;
	};
} // namespace RTC

#endif