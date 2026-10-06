#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <string>

#include <array>
#include <deque>

#include "RTC/NetworkState.hpp"
#include "RTC/SlackPredictor.hpp"

namespace RTC
{
	// yeon: frame record용 임시 registry => layer와 각 layer에 따른 프레임 사이즈
	static constexpr size_t SimulcastLayerCount{ 3u };

	struct LogicalFrameSizeSnapshot
	{
		std::array<bool, SimulcastLayerCount> available{ false, false, false };

		std::array<uint32_t, SimulcastLayerCount> sizeBytes{ 0u, 0u, 0u };
	};

	struct PacketReceiveInfo
	{
		uint16_t sequenceNumber{ 0 };
		uint64_t receiveTimeMs{ 0 };
	};
	struct FrameRecord
	{
		// multi viewer 지원을 위한 식별자
		std::string transportId;
		std::string consumerId;
		std::string producerId;

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
		int64_t latestDecodeTimeMs{ 0 };

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

		// RTP timestamp rewrite로 인해 outgoing timestamp가
		// logical timestamp에서 얼마나 이동했는지.
		bool hasRtpTimestampRewriteOffsetMs{ false };
		double rtpTimestampRewriteOffsetMs{ 0.0 };

		// RTP rewrite 영향을 제거한 KNN용 logical slack.
		bool hasDecodeSlackLogicalMs{ false };
		double decodeSlackLogicalMs{ 0.0 };

		bool hasFrameBufferResidenceMs{ false };
		double frameBufferResidenceMs{ 0.0 };

		bool hasDecodeQueueResidenceMs{ false };
		double decodeQueueResidenceMs{ 0.0 };

		bool hasDecodeSlackEffectiveMs{ false };
		double decodeSlackEffectiveMs{ 0.0 };

		bool hasActualSlackEffectiveMs{ false };
		double actualSlackEffectiveMs{ 0.0 };

		// 패킷 수신 정보를 넣기 위해 추가함
		bool hasPacketReceiveTimes{ false };
		std::vector<PacketReceiveInfo> packetReceiveTimes;

		// === 추가: 브라우저 timing 내부 계산값 ===
		bool hasnow{ false };
		int64_t now{ 0 };

		bool hasrender_time{ false };
		int64_t render_time{ 0 };

		bool hasmax_wait{ false };
		int64_t max_wait{ 0 };

		// yeon: Camel burst length controller output.
		uint32_t camelBurstLengthBytes{ 0u };

		// yeon: FEC.
		bool hasFecRedundancy{ false };

		uint8_t fecProtectionFactor{ 0u };
		double fecRedundancyPercent{ 0.0 };

		// yeon: frame record: 각 layer 별 frame size
		bool hasLogicalFrameId{ false };
		uint32_t logicalFrameId{ 0 };

		std::array<bool, SimulcastLayerCount> hasLayerEncodedFrameSize{ false, false, false };

		std::array<uint32_t, SimulcastLayerCount> layerEncodedFrameSizeBytes{ 0u, 0u, 0u };

		// yeon: VP8 PictureID trace.
		// Incoming: Producer에서 수신한 원래 VP8 PictureID.
		// Outgoing: Consumer로 forwarding할 때 rewrite 이후 실제 VP8 PictureID.
		bool hasVp8PictureIdTrace{ false };

		bool hasIncomingPictureId{ false };
		uint16_t incomingPictureId{ 0u };

		bool hasOutgoingPictureId{ false };
		uint16_t outgoingPictureId{ 0u };

		// VP8 descriptor에 TL0PICIDX가 실제 존재했는지.
		bool vp8HasTl0PictureIndex{ false };

		// 해당 frame 처리 중 PictureID sync/rewrite가 실제 수행되었는지.
		// 여러 RTP packet으로 구성된 frame이므로 packet별 값을 OR해서 저장.
		bool pictureIdSyncApplied{ false };
		bool pictureIdRewriteApplied{ false };

		// yeon: kNN training용 decision-time feature.
		// 실제로 해당 frame에 적용된 action의 feature만 저장.
		bool hasSlackActionDecision{ false };
		RTC::SlackActionDecision slackActionDecision;

		// 동일 Slack feedback 중복 등으로
		// 같은 sample이 predictor에 두 번 들어가는 것을 방지.
		bool knnSampleAdded{ false };

		// SFU-side frame outcome classification.
		bool hasFrameOutcome{ false };
		bool deadlineMiss{ false };
		RTC::FrameOutcome frameOutcome{ RTC::FrameOutcome::NORMAL };
	};

	struct FrameBuilder
	{
		// multi viewer 지원을 위한 식별자
		std::string transportId;
		std::string consumerId;
		std::string producerId;

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

		bool hasPacketReceiveTimes{ false };
		std::vector<PacketReceiveInfo> packetReceiveTimes;

		// yeon: fec
		bool hasFecRedundancy{ false };

		uint8_t fecProtectionFactor{ 0u };
		double fecRedundancyPercent{ 0.0 };

		// yeon: layer 별 frame size
		bool hasLogicalFrameId{ false };
		uint32_t logicalFrameId{ 0 };

		std::array<bool, SimulcastLayerCount> hasLayerEncodedFrameSize{ false, false, false };

		std::array<uint32_t, SimulcastLayerCount> layerEncodedFrameSizeBytes{ 0u, 0u, 0u };

		// yeon: VP8 PictureID trace.
		bool hasVp8PictureIdTrace{ false };

		bool hasIncomingPictureId{ false };
		uint16_t incomingPictureId{ 0u };

		bool hasOutgoingPictureId{ false };
		uint16_t outgoingPictureId{ 0u };

		bool vp8HasTl0PictureIndex{ false };
		bool pictureIdSyncApplied{ false };
		bool pictureIdRewriteApplied{ false };

		bool hasSlackActionDecision{ false };
		RTC::SlackActionDecision slackActionDecision;
	};

	class FrameRecordTable
	{
	public:
		explicit FrameRecordTable(size_t maxCompletedRecords = 5000);
		~FrameRecordTable() = default;

	public:
		// 같은 frameId의 패킷들이 들어올 때마다 호출.
		void OnPacketSent(
		  const std::string& transportId,
		  const std::string& consumerId,
		  const std::string& producerId,
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
		  int64_t latestDecodeTimeMs,
		  double frameBufferInsertTimeMs,
		  double frameBufferExtractTimeMs,
		  double decodeQueueInsertTimeMs,
		  double decodeQueueExtractTimeMs,
		  double decodeStartMs,
		  double decodeFinishMs,
		  int64_t now,
		  int64_t render_time,
		  int64_t max_wait);

		bool AttachPacketReceiveTimes(
		  uint32_t frameId, const std::vector<PacketReceiveInfo>& packetReceiveTimes);

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

		// yeon: fec
		void AttachFecRedundancy(uint32_t frameId, uint8_t protectionFactor);

		struct PendingFecInfo
		{
			uint8_t protectionFactor{ 0u };
			double redundancyPercent{ 0.0 };
		};

		std::unordered_map<uint32_t, PendingFecInfo> pendingFecInfoByFrameId;

		// yeon: layer 별 frame size
		void AttachIngressFrameSizes(
		  uint32_t frameId, uint32_t logicalFrameId, const LogicalFrameSizeSnapshot& snapshot);

		void AttachVp8PictureIdTrace(
		  uint32_t frameId,
		  bool hasIncomingPictureId,
		  uint16_t incomingPictureId,
		  bool hasOutgoingPictureId,
		  uint16_t outgoingPictureId,
		  bool hasTl0PictureIndex,
		  bool pictureIdSyncApplied,
		  bool pictureIdRewriteApplied);
		// yeon: slack predict에 사용할 과거 데이터에 실제 action 적용한 결정만 저장
		void AttachSlackActionDecision(uint32_t frameId, const RTC::SlackActionDecision& decision);

		bool TakeKnnTrainingSample(uint32_t frameId, RTC::SlackSample& sample);

	private:
		struct PendingIngressFrameSizes
		{
			uint32_t logicalFrameId{ 0 };
			LogicalFrameSizeSnapshot snapshot;
		};

		std::unordered_map<uint32_t, PendingIngressFrameSizes> pendingIngressFrameSizesByFrameId;

		struct PendingVp8PictureIdTrace
		{
			bool hasVp8PictureIdTrace{ false };

			bool hasIncomingPictureId{ false };
			uint16_t incomingPictureId{ 0u };

			bool hasOutgoingPictureId{ false };
			uint16_t outgoingPictureId{ 0u };

			bool vp8HasTl0PictureIndex{ false };

			bool pictureIdSyncApplied{ false };
			bool pictureIdRewriteApplied{ false };
		};

		std::unordered_map<uint32_t, PendingVp8PictureIdTrace> pendingVp8PictureIdTraceByFrameId;

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

		// yeon: decision이 FrameBuilder 생성보다 먼저 들어올 수 있으므로
		std::unordered_map<uint32_t, RTC::SlackActionDecision> pendingSlackActionDecisions;

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

	class LogicalFrameSizeRegistry
	{
	public:
		static LogicalFrameSizeRegistry& Instance();

		void Update(
		  const std::string& producerId, uint32_t logicalFrameId, size_t layer, uint32_t frameSizeBytes);

		bool Get(const std::string& producerId, uint32_t logicalFrameId, LogicalFrameSizeSnapshot& out) const;

	private:
		struct ProducerFrames
		{
			std::unordered_map<uint32_t, LogicalFrameSizeSnapshot> frames;
			std::deque<uint32_t> order;
		};

	private:
		static constexpr size_t MaxFramesPerProducer{ 512u };

		mutable std::mutex mutex;

		std::unordered_map<std::string, ProducerFrames> framesByProducer;
	};

	class LogicalFrameClockMapper
	{
	public:
		static LogicalFrameClockMapper& Instance();

		// reference layer(L0)의 첫 logical frame으로 anchor 설정.
		// 이미 설정되어 있으면 아무것도 하지 않는다.
		void InitializeIfNeeded(const std::string& producerId, uint32_t logicalFrameId, uint64_t sfuTimeMs);

		// logicalFrameId를 SFU monotonic clock(ms)으로 변환.
		bool ConvertToSfuTimeMs(
		  const std::string& producerId, uint32_t logicalFrameId, double& outSfuTimeMs) const;

	private:
		LogicalFrameClockMapper() = default;

		struct ClockAnchor
		{
			bool initialized{ false };

			uint32_t baseLogicalFrameId{ 0u };
			uint64_t baseSfuTimeMs{ 0u };
		};

	private:
		mutable std::mutex mutex;

		std::unordered_map<std::string, ClockAnchor> anchorsByProducerId;
	};

} // namespace RTC