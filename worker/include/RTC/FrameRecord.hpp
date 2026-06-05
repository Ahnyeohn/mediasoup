#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "RTC/NetworkState.hpp"

namespace RTC
{
	struct FrameRecord
	{
		uint32_t frameId{ 0 }; // 추천: RTP timestamp 사용
		uint64_t firstPacketSentAtMs{ 0 };
		uint64_t lastPacketSentAtMs{ 0 };

		size_t frameSizeBytes{ 0 };
		uint32_t packetCount{ 0 };

		bool isKeyFrame{ false };
		int temporalLayer{ 0 };
		int SpatialLayer{ -1 };
		int currentSpatialLayer{ -1 };
		int targetSpatialLayer{ -1 };
		int preferredSpatialLayer{ -1 };

		NetworkSnapshot network;

		bool hasSlack{ false };
		double slackMs{ 0.0 };

		bool hasPredictedSlack{ false };
		double predictedSlackMs{ 0.0 };

		// === 추가 telemetry ===
		bool hasReceiveTimeMs{ false };
		uint64_t receiveTimeMs{ 0 };

		bool hasDecodeStartMs{ false };
		uint64_t decodeStartMs{ 0 };

		bool hasDecodeFinishMs{ false };
		uint64_t decodeFinishMs{ 0 };

		// === 추가: SFU에서 계산한 desired time ===
		bool hasDesiredReceiveTimeMs{ false };
		double desiredReceiveTimeMs{ 0.0 };

		bool hasDesiredDecodeStartMs{ false };
		double desiredDecodeStartMs{ 0.0 };

		// === 새로 추가: receive slack ===
		bool hasReceiveSlackMs{ false };
		double receiveSlackMs{ 0.0 };

		// 실험용 코드: pacing off와 pacing on 상황에서의 receive slack
		bool hasExperimentFrameIndex{ false };
		uint64_t experimentFrameIndex{ 0 };
		bool hasPacingPhase{ false };
		int pacingPhase{ -1 }; // 0: ON, 1: OFF
		bool pacingEnabled{ false };

		// === 새로 추가: effective slack: 큐에 들어간 시간 고려 ===

		// === 새 telemetry ===
		bool hasLatestDecodeTimeMs{ false };
		uint64_t latestDecodeTimeMs{ 0 };

		bool hasFrameBufferInsertTimeMs{ false };
		uint64_t frameBufferInsertTimeMs{ 0 };

		bool hasFrameBufferExtractTimeMs{ false };
		uint64_t frameBufferExtractTimeMs{ 0 };

		bool hasDecodeQueueInsertTimeMs{ false };
		uint64_t decodeQueueInsertTimeMs{ 0 };

		bool hasDecodeQueueExtractTimeMs{ false };
		uint64_t decodeQueueExtractTimeMs{ 0 };

		// === 새 파생 지표 ===
		bool hasDecodeSlackNominalMs{ false };
		double decodeSlackNominalMs{ 0.0 };

		bool hasQueueResidenceMs{ false };
		double queueResidenceMs{ 0.0 };

		bool hasFrameBufferResidenceMs{ false };
		double frameBufferResidenceMs{ 0.0 };

		bool hasDecodeQueueResidenceMs{ false };
		double decodeQueueResidenceMs{ 0.0 };

		bool hasDecodeSlackEffectiveMs{ false };
		double decodeSlackEffectiveMs{ 0.0 };

		bool hasActualSlackEffectiveMs{ false };
		double actualSlackEffectiveMs{ 0.0 };
	};

	struct FrameBuilder
	{
		uint32_t frameId{ 0 };
		uint64_t firstPacketSentAtMs{ 0 };
		uint64_t lastPacketSentAtMs{ 0 };

		size_t frameSizeBytes{ 0 };
		uint32_t packetCount{ 0 };

		// raw frame info
		int frameType{ -1 };     // 1=key, 0=inter, -1=unknown
		int temporalLayer{ -1 }; // -1=unknown
		int SpatialLayer{ -1 };
		int currentSpatialLayer{ -1 };
		int targetSpatialLayer{ -1 };
		int preferredSpatialLayer{ -1 };

		bool initialized{ false };

		bool hasValidFrameType{ false };
		bool hasValidTemporalLayer{ false };

		bool pacingEnabled{ false };
	};

	class FrameRecordTable
	{
	public:
		explicit FrameRecordTable(size_t maxCompletedRecords = 5000);
		~FrameRecordTable() = default;

	public:
		// 같은 frameId의 패킷들이 들어올 때마다 호출.
		void OnPacketSent(
		  uint32_t frameId,
		  size_t packetSize,
		  bool isLastPacketOfFrame,
		  int frameType,
		  int temporalLayer,
		  int SpatialLayer,
		  int currentSpatialLayer,
		  int targetSpatialLayer,
		  int preferredSpatialLayer,
		  bool pacingEnabled,
		  uint64_t nowMs,
		  const NetworkSnapshot& snapshot);

		// client가 나중에 slack을 보내면 frameId로 매칭.
		bool AttachSlack(uint32_t frameId, double slackMs);
		bool AttachTiming(
		  uint32_t frameId, uint64_t receiveTimeUs, uint64_t decodeStartMs, uint64_t decodeFinishMs);

		// bool AttachTimingAndDesiredTimes(
		//   uint32_t frameId, double receiveTimeMs, double decodeStartMs, double decodeFinishMs);
		bool AttachTimingAndDesiredTimes(
		  uint32_t frameId,
		  double receiveTimeMs,
		  double latestDecodeTimeMs,
		  double frameBufferInsertTimeMs,
		  double frameBufferExtractTimeMs,
		  double decodeQueueInsertTimeMs,
		  double decodeQueueExtractTimeMs,
		  double decodeStartMs,
		  double decodeFinishMs);

		// 학습용으로 추출할 completed record 조회.
		std::optional<FrameRecord> GetCompletedRecord(uint32_t frameId) const;

		// 오래된 completed record 삭제
		void RemoveOldCompletedRecords(uint64_t minLastPacketSentAtMs);

		size_t GetCompletedCount() const;

		double GetAverageSlackMs() const;
		uint64_t GetSlackCount() const;
		double GetMinSlackMs() const;
		double GetMaxSlackMs() const;
		bool AttachPredictedSlack(uint32_t frameId, double predictedSlackMs);
		// === 추가: receive slack 통계 getter ===
		uint64_t GetReceiveSlackCount() const;
		double GetAverageReceiveSlackMs() const;
		double GetMinReceiveSlackMs() const;
		double GetMaxReceiveSlackMs() const;

	private:
		void FinalizeFrame(uint32_t frameId, uint64_t nowMs, const NetworkSnapshot& snapshot);

		// effective slack
		void UpdateDerivedTimingMetrics(FrameRecord& record);

	private:
		const size_t maxCompletedRecords{ 5000 };

		mutable std::mutex mutex;

		// 아직 끝나지 않은 frame 집계용
		std::unordered_map<uint32_t, FrameBuilder> inProgressFrames;

		// 완료된 frame record
		std::unordered_map<uint32_t, FrameRecord> completedRecords;

		// 오래된 것 정리용 순서 기록
		std::vector<uint32_t> completedOrder;

		double slackSumMs{ 0.0 };
		uint64_t slackCount{ 0 };
		double slackMinMs{ std::numeric_limits<double>::max() };
		double slackMaxMs{ std::numeric_limits<double>::lowest() };

		// === 추가: receive slack 통계 ===
		double receiveSlackSumMs{ 0.0 };
		uint64_t receiveSlackCount{ 0 };
		double receiveSlackMinMs{ std::numeric_limits<double>::max() };
		double receiveSlackMaxMs{ std::numeric_limits<double>::lowest() };

		// === 첫 기준 프레임(reference) ===
		bool hasReferenceFrame{ false };
		uint32_t referenceFrameId{ 0 };
		double referenceReceiveTimeMs{ 0.0 };
		double referenceDecodeStartMs{ 0.0 };

		// 실험용
		bool hasExperimentStarted{ false };
		uint64_t nextExperimentFrameIndex{ 1 };
	};
} // namespace RTC