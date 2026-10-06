#include "RTC/FrameRecord.hpp"
#include <iostream>

namespace
{
	constexpr int64_t KnnLateThresholdMs{ -5 };
}

namespace RTC
{

	LogicalFrameSizeRegistry& LogicalFrameSizeRegistry::Instance()
	{
		static LogicalFrameSizeRegistry instance;

		return instance;
	}

	void LogicalFrameSizeRegistry::Update(
	  const std::string& producerId, uint32_t logicalFrameId, size_t layer, uint32_t frameSizeBytes)
	{
		if (layer >= SimulcastLayerCount)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(this->mutex);

		auto& producerFrames = this->framesByProducer[producerId];

		auto [it, inserted] = producerFrames.frames.try_emplace(logicalFrameId);

		if (inserted)
		{
			producerFrames.order.push_back(logicalFrameId);
		}

		auto& snapshot = it->second;

		snapshot.available[layer] = true;
		snapshot.sizeBytes[layer] = frameSizeBytes;

		while (producerFrames.order.size() > MaxFramesPerProducer)
		{
			const uint32_t oldest = producerFrames.order.front();

			producerFrames.order.pop_front();
			producerFrames.frames.erase(oldest);
		}
	}

	bool LogicalFrameSizeRegistry::Get(
	  const std::string& producerId, uint32_t logicalFrameId, LogicalFrameSizeSnapshot& out) const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto producerIt = this->framesByProducer.find(producerId);

		if (producerIt == this->framesByProducer.end())
		{
			return false;
		}

		auto frameIt = producerIt->second.frames.find(logicalFrameId);

		if (frameIt == producerIt->second.frames.end())
		{
			return false;
		}

		out = frameIt->second;

		return true;
	}

	RTC::LogicalFrameClockMapper& RTC::LogicalFrameClockMapper::Instance()
	{
		static LogicalFrameClockMapper instance;

		return instance;
	}

	void RTC::LogicalFrameClockMapper::InitializeIfNeeded(
	  const std::string& producerId, uint32_t logicalFrameId, uint64_t sfuTimeMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto& anchor = this->anchorsByProducerId[producerId];

		if (anchor.initialized)
		{
			return;
		}

		anchor.initialized        = true;
		anchor.baseLogicalFrameId = logicalFrameId;
		anchor.baseSfuTimeMs      = sfuTimeMs;
	}

	bool RTC::LogicalFrameClockMapper::ConvertToSfuTimeMs(
	  const std::string& producerId, uint32_t logicalFrameId, double& outSfuTimeMs) const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->anchorsByProducerId.find(producerId);

		if (it == this->anchorsByProducerId.end() || !it->second.initialized)
		{
			return false;
		}

		const auto& anchor = it->second;

		/*
		 * uint32_t arithmetic를 사용하므로 RTP timestamp의
		 * 32-bit wrap-around 하나는 자연스럽게 처리된다.
		 *
		 * video RTP clock rate = 90 kHz.
		 */
		const uint32_t deltaTicks = logicalFrameId - anchor.baseLogicalFrameId;

		const double deltaMs = static_cast<double>(deltaTicks) * 1000.0 / 90000.0;

		outSfuTimeMs = static_cast<double>(anchor.baseSfuTimeMs) + deltaMs;

		return true;
	}

	FrameRecordTable::FrameRecordTable(size_t maxCompletedRecords)
	  : maxCompletedRecords(maxCompletedRecords)
	{
	}

	void FrameRecordTable::OnPacketSent(
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
	  const NetworkSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		// 이미 완료된 frame은 다시 만들지 않는다.
		// RTX 등 동일 RTP timestamp를 가진 후속 패킷이
		// 기존 FrameRecord를 덮어쓰는 것을 방지.
		if (this->completedRecords.find(frameId) != this->completedRecords.end())
		{
			return;
		}

		auto& builder = this->inProgressFrames[frameId];

		if (!builder.initialized)
		{
			builder.transportId = transportId;
			builder.consumerId  = consumerId;
			builder.producerId  = producerId;

			builder.frameId             = frameId;
			builder.firstPacketSentAtMs = nowMs;
			builder.initialized         = true;

			// yeon: slack action에 대해 pending되어 있던 데이터들 빌더에 추가하기
			// decision-time feature가 먼저 들어와 있었다면 연결.
			auto decisionIt = this->pendingSlackActionDecisions.find(frameId);

			if (decisionIt != this->pendingSlackActionDecisions.end())
			{
				builder.hasSlackActionDecision = true;

				builder.slackActionDecision = decisionIt->second;

				this->pendingSlackActionDecisions.erase(decisionIt);
			}

			// 첫 패킷 기준 초기 layer 정보 저장
			builder.SpatialLayer          = SpatialLayer;
			builder.currentSpatialLayer   = currentSpatialLayer;
			builder.targetSpatialLayer    = targetSpatialLayer;
			builder.preferredSpatialLayer = preferredSpatialLayer;

			builder.pacingEnabled = pacingEnabled;
		}

		// yeon: layer 별 frame size
		auto ingressIt = this->pendingIngressFrameSizesByFrameId.find(frameId);

		if (ingressIt != this->pendingIngressFrameSizesByFrameId.end())
		{
			builder.hasLogicalFrameId = true;
			builder.logicalFrameId    = ingressIt->second.logicalFrameId;

			for (size_t i = 0; i < SimulcastLayerCount; ++i)
			{
				if (!ingressIt->second.snapshot.available[i])
				{
					continue;
				}

				builder.hasLayerEncodedFrameSize[i]   = true;
				builder.layerEncodedFrameSizeBytes[i] = ingressIt->second.snapshot.sizeBytes[i];
			}

			this->pendingIngressFrameSizesByFrameId.erase(ingressIt);
		}

		// yeon: fec
		auto fecIt = this->pendingFecInfoByFrameId.find(frameId);

		if (fecIt != this->pendingFecInfoByFrameId.end())
		{
			builder.hasFecRedundancy     = true;
			builder.fecProtectionFactor  = fecIt->second.protectionFactor;
			builder.fecRedundancyPercent = fecIt->second.redundancyPercent;

			this->pendingFecInfoByFrameId.erase(fecIt);
		}

		// yeon: VP8 PictureID trace.
		// SendRtpPacket()에서 trace가 먼저 들어오고,
		// 실제 OnPacketSent()가 나중에 호출될 수 있음.
		auto vp8TraceIt = this->pendingVp8PictureIdTraceByFrameId.find(frameId);

		if (vp8TraceIt != this->pendingVp8PictureIdTraceByFrameId.end())
		{
			const auto& trace = vp8TraceIt->second;

			builder.hasVp8PictureIdTrace = trace.hasVp8PictureIdTrace;

			builder.hasIncomingPictureId = trace.hasIncomingPictureId;
			builder.incomingPictureId    = trace.incomingPictureId;

			builder.hasOutgoingPictureId = trace.hasOutgoingPictureId;
			builder.outgoingPictureId    = trace.outgoingPictureId;

			builder.vp8HasTl0PictureIndex = trace.vp8HasTl0PictureIndex;

			builder.pictureIdSyncApplied = builder.pictureIdSyncApplied || trace.pictureIdSyncApplied;

			builder.pictureIdRewriteApplied =
			  builder.pictureIdRewriteApplied || trace.pictureIdRewriteApplied;

			this->pendingVp8PictureIdTraceByFrameId.erase(vp8TraceIt);
		}

		builder.lastPacketSentAtMs = nowMs;
		builder.frameSizeBytes += packetSize;
		builder.packetCount += 1;

		// frameType은 처음으로 유효하게 판별된 값만 채택
		if (!builder.hasValidFrameType && frameType != -1)
		{
			builder.frameType         = frameType;
			builder.hasValidFrameType = true;
		}

		// temporal layer도 처음으로 유효하게 판별된 값만 채택
		if (!builder.hasValidTemporalLayer && temporalLayer != -1)
		{
			builder.temporalLayer         = temporalLayer;
			builder.hasValidTemporalLayer = true;
		}

		// consumer 상태 layer는 최신 값 유지
		builder.currentSpatialLayer   = currentSpatialLayer;
		builder.targetSpatialLayer    = targetSpatialLayer;
		builder.preferredSpatialLayer = preferredSpatialLayer;

		builder.pacingEnabled = pacingEnabled;

		if (isLastPacketOfFrame)
		{
			FinalizeFrame(frameId, nowMs, snapshot);
		}
	}

	void FrameRecordTable::FinalizeFrame(
	  uint32_t frameId, uint64_t /*nowMs*/, const NetworkSnapshot& snapshot)
	{
		auto it = this->inProgressFrames.find(frameId);
		if (it == this->inProgressFrames.end())
		{
			return;
		}

		const auto& builder = it->second;

		FrameRecord record;

		record.transportId = builder.transportId;
		record.consumerId  = builder.consumerId;
		record.producerId  = builder.producerId;

		record.frameId             = builder.frameId;
		record.firstPacketSentAtMs = builder.firstPacketSentAtMs;
		record.lastPacketSentAtMs  = builder.lastPacketSentAtMs;
		record.frameSizeBytes      = builder.frameSizeBytes;
		record.packetCount         = builder.packetCount;
		record.isKeyFrame          = (builder.frameType == 1);
		record.temporalLayer =
		  (builder.temporalLayer >= 0) ? static_cast<uint8_t>(builder.temporalLayer) : 0;

		record.SpatialLayer          = builder.SpatialLayer;
		record.currentSpatialLayer   = builder.currentSpatialLayer;
		record.targetSpatialLayer    = builder.targetSpatialLayer;
		record.preferredSpatialLayer = builder.preferredSpatialLayer;

		record.pacingEnabled = builder.pacingEnabled;

		// 패킷 정보도 포함
		record.packetReceiveTimes    = builder.packetReceiveTimes;
		record.hasPacketReceiveTimes = builder.hasPacketReceiveTimes;

		// fec 관련 정보 포함
		record.hasFecRedundancy     = builder.hasFecRedundancy;
		record.fecProtectionFactor  = builder.fecProtectionFactor;
		record.fecRedundancyPercent = builder.fecRedundancyPercent;

		// layer 별 frame size 정보 및 이를 구분하기 위한 정보 포함
		record.hasLogicalFrameId          = builder.hasLogicalFrameId;
		record.logicalFrameId             = builder.logicalFrameId;
		record.hasLayerEncodedFrameSize   = builder.hasLayerEncodedFrameSize;
		record.layerEncodedFrameSizeBytes = builder.layerEncodedFrameSizeBytes;

		// VP8 PictureID trace.
		record.hasVp8PictureIdTrace = builder.hasVp8PictureIdTrace;

		record.hasIncomingPictureId = builder.hasIncomingPictureId;
		record.incomingPictureId    = builder.incomingPictureId;

		record.hasOutgoingPictureId = builder.hasOutgoingPictureId;
		record.outgoingPictureId    = builder.outgoingPictureId;

		record.vp8HasTl0PictureIndex = builder.vp8HasTl0PictureIndex;

		record.pictureIdSyncApplied    = builder.pictureIdSyncApplied;
		record.pictureIdRewriteApplied = builder.pictureIdRewriteApplied;

		// decision-time kNN feature와 나중에 도착하는 Slack 정보들
		record.hasSlackActionDecision = builder.hasSlackActionDecision;
		if (builder.hasSlackActionDecision)
		{
			record.slackActionDecision = builder.slackActionDecision;
		}

		record.network = snapshot;

		this->completedRecords[frameId] = record;
		this->completedOrder.push_back(frameId);

		this->inProgressFrames.erase(it);

		// 최대 개수 제한
		while (this->completedOrder.size() > this->maxCompletedRecords)
		{
			uint32_t oldestFrameId = this->completedOrder.front();
			this->completedOrder.erase(this->completedOrder.begin());
			this->completedRecords.erase(oldestFrameId);
		}
	}

	bool FrameRecordTable::AttachSlack(uint32_t frameId, double slackMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it == this->completedRecords.end())
		{
			return false;
		}

		// 이미 slack이 붙은 frame에 또 붙는 상황은 중복 집계 방지
		if (!it->second.hasSlack)
		{
			this->slackSumMs += slackMs;
			this->slackCount += 1;

			if (slackMs < this->slackMinMs)
			{
				this->slackMinMs = slackMs;
			}
			if (slackMs > this->slackMaxMs)
			{
				this->slackMaxMs = slackMs;
			}
		}

		it->second.hasSlack = true;
		it->second.slackMs  = slackMs;

		// actualSlackEffective 갱신
		UpdateDerivedTimingMetrics(it->second);

		return true;
	}

	bool FrameRecordTable::AttachTiming(
	  uint32_t frameId, uint64_t receiveTimeMs, uint64_t decodeStartMs, uint64_t decodeFinishMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it == this->completedRecords.end())
		{
			return false;
		}

		it->second.hasReceiveTimeMs = true;
		it->second.receiveTimeMs    = receiveTimeMs;

		it->second.hasDecodeStartMs = true;
		it->second.decodeStartMs    = decodeStartMs;

		it->second.hasDecodeFinishMs = true;
		it->second.decodeFinishMs    = decodeFinishMs;

		return true;
	}

	// effective slack
	void FrameRecordTable::UpdateDerivedTimingMetrics(FrameRecord& record)
	{
		// 1) decodeSlackNominal = desiredDecodeStartTime - receiveTime
		if (record.hasDesiredDecodeStartMs && record.hasReceiveTimeMs)
		{
			record.hasDecodeSlackNominalMs = true;
			record.decodeSlackNominalMs =
			  record.desiredDecodeStartMs - static_cast<double>(record.receiveTimeMs);
		}

		// ============================================================
		// 1.5) RTP timestamp rewrite offset + logical slack.
		//
		// frameId:
		//   Consumer가 실제 viewer로 전송한 rewritten RTP timestamp.
		//
		// logicalFrameId:
		//   Producer/source 기준 logical RTP timestamp.
		//
		// mediasoup spatial-layer switching 과정에서 frameId가
		// +33ms 등으로 보정될 수 있으므로 그 영향을 제거한다.
		//
		// int32_t subtraction을 사용하여 RTP uint32 wrap-around도
		// signed timestamp difference로 처리한다.
		// ============================================================
		if (record.hasDecodeSlackNominalMs && record.hasLogicalFrameId)
		{
			const int32_t rewriteOffsetTicks = static_cast<int32_t>(record.frameId - record.logicalFrameId);

			record.hasRtpTimestampRewriteOffsetMs = true;
			record.rtpTimestampRewriteOffsetMs = static_cast<double>(rewriteOffsetTicks) * 1000.0 / 90000.0;

			record.hasDecodeSlackLogicalMs = true;
			record.decodeSlackLogicalMs = record.decodeSlackNominalMs - record.rtpTimestampRewriteOffsetMs;
		}

		// 2) queueResidence =
		//    (frame buffer residence) + (decode queue residence)
		bool validFrameBufferResidence =
		  record.hasFrameBufferInsertTimeMs && record.hasFrameBufferExtractTimeMs &&
		  record.frameBufferExtractTimeMs >= record.frameBufferInsertTimeMs;

		bool validDecodeQueueResidence =
		  record.hasDecodeQueueInsertTimeMs && record.hasDecodeQueueExtractTimeMs &&
		  record.decodeQueueExtractTimeMs >= record.decodeQueueInsertTimeMs;

		if (validFrameBufferResidence && validDecodeQueueResidence)
		{
			record.hasFrameBufferResidenceMs = true;
			record.hasDecodeQueueResidenceMs = true;

			record.frameBufferResidenceMs =
			  static_cast<double>(record.frameBufferExtractTimeMs - record.frameBufferInsertTimeMs);

			record.decodeQueueResidenceMs =
			  static_cast<double>(record.decodeQueueExtractTimeMs - record.decodeQueueInsertTimeMs);

			record.hasQueueResidenceMs = true;
			record.queueResidenceMs    = record.frameBufferResidenceMs + record.decodeQueueResidenceMs;
		}

		// 3) decodeSlackEffective = decodeSlackNominal - queueResidence
		if (record.hasDecodeSlackNominalMs && record.hasQueueResidenceMs)
		{
			record.hasDecodeSlackEffectiveMs = true;
			record.decodeSlackEffectiveMs    = record.decodeSlackNominalMs - record.queueResidenceMs;
		}

		// 4) actualSlackEffective = actualSlack - queueResidence
		if (record.hasSlack && record.hasQueueResidenceMs)
		{
			record.hasActualSlackEffectiveMs = true;
			record.actualSlackEffectiveMs    = record.slackMs - record.queueResidenceMs;
		}
	}

	// bool FrameRecordTable::AttachTimingAndDesiredTimes(
	//   uint32_t frameId, double receiveTimeMs, double decodeStartMs, double decodeFinishMs)
	// {
	// 	std::lock_guard<std::mutex> lock(this->mutex);

	// 	auto it = this->completedRecords.find(frameId);
	// 	if (it == this->completedRecords.end())
	// 	{
	// 		return false;
	// 	}

	// 	// 실제 telemetry 저장
	// 	it->second.hasReceiveTimeMs = true;
	// 	it->second.receiveTimeMs    = receiveTimeMs;

	// 	it->second.hasDecodeStartMs = true;
	// 	it->second.decodeStartMs    = decodeStartMs;

	// 	it->second.hasDecodeFinishMs = true;
	// 	it->second.decodeFinishMs    = decodeFinishMs;

	// 	// 첫 프레임이면 기준 프레임으로 설정
	// 	if (!this->hasReferenceFrame)
	// 	{
	// 		this->hasReferenceFrame      = true;
	// 		this->referenceFrameId       = frameId;
	// 		this->referenceReceiveTimeMs = receiveTimeMs;
	// 		this->referenceDecodeStartMs = decodeStartMs;

	// 		// 기준 프레임 자신의 desired time은 자기 실제값으로 둠
	// 		it->second.hasDesiredReceiveTimeMs = true;
	// 		it->second.desiredReceiveTimeMs    = receiveTimeMs;

	// 		it->second.hasDesiredDecodeStartMs = true;
	// 		it->second.desiredDecodeStartMs    = decodeStartMs;

	// 		// 기준 프레임의 receive slack = 0
	// 		if (!it->second.hasReceiveSlackMs)
	// 		{
	// 			it->second.hasReceiveSlackMs = true;
	// 			it->second.receiveSlackMs    = 0.0;

	// 			this->receiveSlackSumMs += it->second.receiveSlackMs;
	// 			this->receiveSlackCount += 1;

	// 			if (it->second.receiveSlackMs < this->receiveSlackMinMs)
	// 			{
	// 				this->receiveSlackMinMs = it->second.receiveSlackMs;
	// 			}
	// 			if (it->second.receiveSlackMs > this->receiveSlackMaxMs)
	// 			{
	// 				this->receiveSlackMaxMs = it->second.receiveSlackMs;
	// 			}
	// 		}

	// 		return true;
	// 	}

	// 	// 후속 프레임이면 RTP timestamp 차이 기반으로 desired time 계산
	// 	// RTP clock rate = 90000Hz
	// 	int64_t deltaRtp = static_cast<int64_t>(frameId) - static_cast<int64_t>(this->referenceFrameId);
	// 	double deltaMs   = static_cast<double>(deltaRtp) * 1000.0 / 90000.0;

	// 	it->second.hasDesiredReceiveTimeMs = true;
	// 	it->second.desiredReceiveTimeMs    = this->referenceReceiveTimeMs + deltaMs;

	// 	it->second.hasDesiredDecodeStartMs = true;
	// 	it->second.desiredDecodeStartMs    = this->referenceDecodeStartMs + deltaMs;

	// 	const double receiveSlackMs = it->second.desiredReceiveTimeMs - it->second.receiveTimeMs;

	// 	// 중복 집계 방지
	// 	if (!it->second.hasReceiveSlackMs)
	// 	{
	// 		this->receiveSlackSumMs += receiveSlackMs;
	// 		this->receiveSlackCount += 1;

	// 		if (receiveSlackMs < this->receiveSlackMinMs)
	// 		{
	// 			this->receiveSlackMinMs = receiveSlackMs;
	// 		}
	// 		if (receiveSlackMs > this->receiveSlackMaxMs)
	// 		{
	// 			this->receiveSlackMaxMs = receiveSlackMs;
	// 		}
	// 	}

	// 	it->second.hasReceiveSlackMs = true;
	// 	it->second.receiveSlackMs    = receiveSlackMs;

	// 	return true;
	// }

	bool FrameRecordTable::AttachTimingAndDesiredTimes(
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
	  int64_t max_wait)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it == this->completedRecords.end())
		{
			return false;
		}

		auto& record = it->second;

		// === 실제 telemetry 저장 ===
		record.hasReceiveTimeMs = true;
		record.receiveTimeMs    = receiveTimeMs;

		record.hasLatestDecodeTimeMs = true;
		record.latestDecodeTimeMs    = latestDecodeTimeMs;

		record.hasFrameBufferInsertTimeMs = true;
		record.frameBufferInsertTimeMs    = frameBufferInsertTimeMs;

		record.hasFrameBufferExtractTimeMs = true;
		record.frameBufferExtractTimeMs    = frameBufferExtractTimeMs;

		record.hasDecodeQueueInsertTimeMs = true;
		record.decodeQueueInsertTimeMs    = decodeQueueInsertTimeMs;

		record.hasDecodeQueueExtractTimeMs = true;
		record.decodeQueueExtractTimeMs    = decodeQueueExtractTimeMs;

		record.hasDecodeStartMs = true;
		record.decodeStartMs    = decodeStartMs;

		record.hasDecodeFinishMs = true;
		record.decodeFinishMs    = decodeFinishMs;

		record.hasnow = true;
		record.now    = now;

		record.hasrender_time = true;
		record.render_time    = render_time;

		record.hasmax_wait = true;
		record.max_wait    = max_wait;

		// ========================================================
		// DROP: decodeStartMs == 0
		// LATE: DROP은 아니지만 max_wait <= threshold
		// NORMAL: 나머지
		// ========================================================
		const bool dropped = record.hasDecodeStartMs && record.decodeStartMs <= 0.0;

		const bool late = !dropped && record.hasmax_wait && record.max_wait <= KnnLateThresholdMs;

		record.hasFrameOutcome = true;

		if (dropped)
		{
			record.deadlineMiss = true;
			record.frameOutcome = RTC::FrameOutcome::PRE_DECODE_DROP;
		}
		else if (late)
		{
			record.deadlineMiss = true;
			record.frameOutcome = RTC::FrameOutcome::LATE;
		}
		else
		{
			record.deadlineMiss = false;
			record.frameOutcome = RTC::FrameOutcome::NORMAL;
		}

		// === 첫 프레임이면 기준 프레임으로 설정 ===
		// 기준 frame은 실제 decodeStart가 존재하는 frame으로만 설정.
		// DROP frame(decodeStartMs == 0)은 reference로 사용하지 않는다.
		if (!this->hasReferenceFrame)
		{
			if (decodeStartMs > 0.0)
			{
				this->hasReferenceFrame      = true;
				this->referenceFrameId       = frameId;
				this->referenceReceiveTimeMs = receiveTimeMs;
				this->referenceDecodeStartMs = decodeStartMs;

				record.hasDesiredReceiveTimeMs = true;
				record.desiredReceiveTimeMs    = receiveTimeMs;

				record.hasDesiredDecodeStartMs = true;
				record.desiredDecodeStartMs    = decodeStartMs;

				if (!record.hasReceiveSlackMs)
				{
					record.hasReceiveSlackMs = true;
					record.receiveSlackMs    = 0.0;

					this->receiveSlackSumMs += record.receiveSlackMs;
					this->receiveSlackCount += 1;

					if (record.receiveSlackMs < this->receiveSlackMinMs)
					{
						this->receiveSlackMinMs = record.receiveSlackMs;
					}

					if (record.receiveSlackMs > this->receiveSlackMaxMs)
					{
						this->receiveSlackMaxMs = record.receiveSlackMs;
					}
				}
			}

			// 아직 정상 decode frame이 한 번도 없었다면
			// desiredDecodeStart를 계산할 수 없으므로 이 frame은
			// kNN training sample에는 들어가지 않게 된다.
			UpdateDerivedTimingMetrics(record);

			return true;
		}

		// === 후속 프레임이면 RTP timestamp 차이 기반으로 desired time 계산 ===
		// int64_t deltaRtp = static_cast<int64_t>(frameId) - static_cast<int64_t>(this->referenceFrameId);
		// double deltaMs   = static_cast<double>(deltaRtp) * 1000.0 / 90000.0;

		const int32_t deltaRtp = static_cast<int32_t>(frameId - this->referenceFrameId);
		const double deltaMs   = static_cast<double>(deltaRtp) * 1000.0 / 90000.0;

		record.hasDesiredReceiveTimeMs = true;
		record.desiredReceiveTimeMs    = this->referenceReceiveTimeMs + deltaMs;

		record.hasDesiredDecodeStartMs = true;
		record.desiredDecodeStartMs    = this->referenceDecodeStartMs + deltaMs;

		const double receiveSlackMs =
		  record.desiredReceiveTimeMs - static_cast<double>(record.receiveTimeMs);

		if (!record.hasReceiveSlackMs)
		{
			this->receiveSlackSumMs += receiveSlackMs;
			this->receiveSlackCount += 1;

			if (receiveSlackMs < this->receiveSlackMinMs)
			{
				this->receiveSlackMinMs = receiveSlackMs;
			}
			if (receiveSlackMs > this->receiveSlackMaxMs)
			{
				this->receiveSlackMaxMs = receiveSlackMs;
			}
		}

		record.hasReceiveSlackMs = true;
		record.receiveSlackMs    = receiveSlackMs;

		// === 새 파생 지표 계산 ===
		UpdateDerivedTimingMetrics(record);

		return true;
	}

	bool FrameRecordTable::AttachPacketReceiveTimes(
	  uint32_t frameId, const std::vector<PacketReceiveInfo>& packetReceiveTimes)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it != this->completedRecords.end())
		{
			it->second.packetReceiveTimes    = packetReceiveTimes;
			it->second.hasPacketReceiveTimes = !packetReceiveTimes.empty();
			return true;
		}

		auto inProgressIt = this->inProgressFrames.find(frameId);
		if (inProgressIt != this->inProgressFrames.end())
		{
			inProgressIt->second.packetReceiveTimes    = packetReceiveTimes;
			inProgressIt->second.hasPacketReceiveTimes = !packetReceiveTimes.empty();
			return true;
		}

		return false;
	}

	std::optional<FrameRecord> FrameRecordTable::GetCompletedRecord(uint32_t frameId) const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it == this->completedRecords.end())
		{
			return std::nullopt;
		}

		return it->second;
	}

	void FrameRecordTable::RemoveOldCompletedRecords(uint64_t minLastPacketSentAtMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		std::vector<uint32_t> newOrder;
		newOrder.reserve(this->completedOrder.size());

		for (auto frameId : this->completedOrder)
		{
			auto it = this->completedRecords.find(frameId);
			if (it == this->completedRecords.end())
			{
				continue;
			}

			if (it->second.lastPacketSentAtMs < minLastPacketSentAtMs)
			{
				this->completedRecords.erase(it);
			}
			else
			{
				newOrder.push_back(frameId);
			}
		}

		this->completedOrder.swap(newOrder);
	}

	size_t FrameRecordTable::GetCompletedCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->completedRecords.size();
	}

	double FrameRecordTable::GetAverageSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->slackCount == 0)
		{
			return 0.0;
		}

		return this->slackSumMs / static_cast<double>(this->slackCount);
	}

	uint64_t FrameRecordTable::GetSlackCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->slackCount;
	}

	double FrameRecordTable::GetMinSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->slackCount == 0)
		{
			return 0.0;
		}

		return this->slackMinMs;
	}

	double FrameRecordTable::GetMaxSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->slackCount == 0)
		{
			return 0.0;
		}

		return this->slackMaxMs;
	}

	bool FrameRecordTable::AttachPredictedSlack(uint32_t frameId, double predictedSlackMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);
		if (it == this->completedRecords.end())
		{
			return false;
		}

		it->second.hasPredictedSlack = true;
		it->second.predictedSlackMs  = predictedSlackMs;

		return true;
	}

	uint64_t FrameRecordTable::GetReceiveSlackCount() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->receiveSlackCount;
	}

	double FrameRecordTable::GetAverageReceiveSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->receiveSlackCount == 0)
		{
			return 0.0;
		}

		return this->receiveSlackSumMs / static_cast<double>(this->receiveSlackCount);
	}

	double FrameRecordTable::GetMinReceiveSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->receiveSlackCount == 0)
		{
			return 0.0;
		}

		return this->receiveSlackMinMs;
	}

	double FrameRecordTable::GetMaxReceiveSlackMs() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (this->receiveSlackCount == 0)
		{
			return 0.0;
		}

		return this->receiveSlackMaxMs;
	}

	void RTC::FrameRecordTable::AttachFecRedundancy(uint32_t frameId, uint8_t protectionFactor)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		const double redundancyPercent = static_cast<double>(protectionFactor) * 100.0 / 255.0;

		// 1. 이미 completed 된 frame이면 바로 저장.
		auto completedIt = this->completedRecords.find(frameId);

		if (completedIt != this->completedRecords.end())
		{
			completedIt->second.hasFecRedundancy     = true;
			completedIt->second.fecProtectionFactor  = protectionFactor;
			completedIt->second.fecRedundancyPercent = redundancyPercent;

			return;
		}

		// 2. 아직 전송 중인 frame이면 builder에 저장.
		auto progressIt = this->inProgressFrames.find(frameId);

		if (progressIt != this->inProgressFrames.end())
		{
			progressIt->second.hasFecRedundancy     = true;
			progressIt->second.fecProtectionFactor  = protectionFactor;
			progressIt->second.fecRedundancyPercent = redundancyPercent;

			return;
		}

		// 3. pacing 때문에 아직 첫 packet도 실제 send되지 않은 경우.
		// 나중에 OnPacketSent()에서 가져간다.
		this->pendingFecInfoByFrameId[frameId] = { protectionFactor, redundancyPercent };
	}

	void FrameRecordTable::AttachIngressFrameSizes(
	  uint32_t frameId, uint32_t logicalFrameId, const LogicalFrameSizeSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto merge = [&](auto& target)
		{
			target.hasLogicalFrameId = true;
			target.logicalFrameId    = logicalFrameId;

			for (size_t i = 0; i < SimulcastLayerCount; ++i)
			{
				if (!snapshot.available[i])
				{
					continue;
				}

				target.hasLayerEncodedFrameSize[i]   = true;
				target.layerEncodedFrameSizeBytes[i] = snapshot.sizeBytes[i];
			}
		};

		// 이미 완료된 경우도 갱신 가능.
		auto completedIt = this->completedRecords.find(frameId);

		if (completedIt != this->completedRecords.end())
		{
			auto& record = completedIt->second;

			// 최신 ingress layer size 반영.
			merge(record);
			// logicalFrameId가 timing telemetry보다 늦게 붙은 경우에도
			// logical slack을 다시 계산할 수 있도록 갱신.
			UpdateDerivedTimingMetrics(record);

			return;
		}

		// 실제 송신 중.
		auto progressIt = this->inProgressFrames.find(frameId);

		if (progressIt != this->inProgressFrames.end())
		{
			merge(progressIt->second);
			return;
		}

		// 아직 pacer 등으로 실제 send가 시작되지 않음.
		auto& pending = this->pendingIngressFrameSizesByFrameId[frameId];

		pending.logicalFrameId = logicalFrameId;

		for (size_t i = 0; i < SimulcastLayerCount; ++i)
		{
			if (snapshot.available[i])
			{
				pending.snapshot.available[i] = true;
				pending.snapshot.sizeBytes[i] = snapshot.sizeBytes[i];
			}
		}
	}

	void FrameRecordTable::AttachVp8PictureIdTrace(
	  uint32_t frameId,
	  bool hasIncomingPictureId,
	  uint16_t incomingPictureId,
	  bool hasOutgoingPictureId,
	  uint16_t outgoingPictureId,
	  bool hasTl0PictureIndex,
	  bool pictureIdSyncApplied,
	  bool pictureIdRewriteApplied)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto merge = [&](auto& target)
		{
			target.hasVp8PictureIdTrace = true;

			if (hasIncomingPictureId)
			{
				target.hasIncomingPictureId = true;
				target.incomingPictureId    = incomingPictureId;
			}

			if (hasOutgoingPictureId)
			{
				target.hasOutgoingPictureId = true;
				target.outgoingPictureId    = outgoingPictureId;
			}

			// 같은 frame의 여러 RTP packet이 들어올 수 있으므로 OR merge.
			target.vp8HasTl0PictureIndex = target.vp8HasTl0PictureIndex || hasTl0PictureIndex;

			target.pictureIdSyncApplied = target.pictureIdSyncApplied || pictureIdSyncApplied;

			target.pictureIdRewriteApplied = target.pictureIdRewriteApplied || pictureIdRewriteApplied;
		};

		// 1. 이미 완료된 frame.
		auto completedIt = this->completedRecords.find(frameId);

		if (completedIt != this->completedRecords.end())
		{
			merge(completedIt->second);
			return;
		}

		// 2. 현재 전송 중인 frame.
		auto progressIt = this->inProgressFrames.find(frameId);

		if (progressIt != this->inProgressFrames.end())
		{
			merge(progressIt->second);
			return;
		}

		// 3. 아직 OnPacketSent()가 호출되기 전.
		auto& pending = this->pendingVp8PictureIdTraceByFrameId[frameId];

		merge(pending);
	}

	void FrameRecordTable::AttachSlackActionDecision(
	  uint32_t frameId, const RTC::SlackActionDecision& decision)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		// 1. 이미 completed record라면 바로 저장.
		auto completedIt = this->completedRecords.find(frameId);

		if (completedIt != this->completedRecords.end())
		{
			completedIt->second.hasSlackActionDecision = true;
			completedIt->second.slackActionDecision    = decision;

			return;
		}

		// 2. 현재 생성 중인 FrameBuilder가 있으면 거기에 저장.
		auto progressIt = this->inProgressFrames.find(frameId);

		if (progressIt != this->inProgressFrames.end())
		{
			progressIt->second.hasSlackActionDecision = true;
			progressIt->second.slackActionDecision    = decision;

			return;
		}

		// 3. 아직 첫 packet 실제 전송 전이라 Builder가 없으면 pending으로 저장.
		this->pendingSlackActionDecisions[frameId] = decision;
	}

	bool FrameRecordTable::TakeKnnTrainingSample(uint32_t frameId, RTC::SlackSample& sample)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		auto it = this->completedRecords.find(frameId);

		if (it == this->completedRecords.end())
		{
			return false;
		}

		auto& record = it->second;

		// ========================================================
		// kNN continuous label은 decodeSlackLogical.
		//
		// decodeSlackNominal에는 mediasoup spatial-layer switching의
		// RTP timestamp rewrite(+33ms 등)가 포함될 수 있으므로
		// KNN training target으로 사용하지 않는다.
		//
		// logicalFrameId 또는 nominal slack을 얻지 못해
		// logical slack 계산이 불가능한 frame은 학습에서 제외한다.
		// ========================================================
		if (!record.hasDecodeSlackLogicalMs)
		{
			return false;
		}
		// if (!record.hasDecodeSlackNominalMs)
		// {
		// 	return false;
		// }

		// Decision-time feature가 없는 frame은 학습 불가.
		if (!record.hasSlackActionDecision)
		{
			return false;
		}

		if (record.knnSampleAdded)
		{
			return false;
		}

		// ========================================================
		// 기존 실제 action == decision action 검증이 있다면
		// 이 부분은 그대로 유지.
		// ========================================================

		const auto& decision = record.slackActionDecision;

		// Pause frames remain available for FrameRecord/CSV analysis but
		// must never become SlackPredictor training samples.
		if (decision.skipKnnTrainingSample)
		{
			return false;
		}

		// ========================================================
		// 실제 전송된 spatial layer 확인.
		// ========================================================
		if (record.currentSpatialLayer < 0 || record.currentSpatialLayer >= static_cast<int16_t>(SimulcastLayerCount))
		{
			return false;
		}

		const size_t actualLayer = static_cast<size_t>(record.currentSpatialLayer);

		// ========================================================
		// FEC / Pacing은 decision과 실제 적용 action이 같아야 학습.
		// ========================================================
		const bool fecMatches =
		  record.hasFecRedundancy && record.fecProtectionFactor == decision.fecProtectionFactor;

		const bool pacingMatches = record.pacingEnabled == decision.pacingEnabled;

		if (!fecMatches || !pacingMatches)
		{
			return false;
		}

		sample.frameId = record.frameId;

		// ========================================================
		// 실제 전송 layer에 해당하는 decision-time feature 사용.
		//
		// KNN decision에서는 동일 FEC/Pacing에 대해
		// L0/L1/L2 feature를 모두 저장해두었으므로,
		// model-selected layer가 아니라 actual layer의 X_i를 사용.
		// ========================================================
		if (decision.hasActionFeatureBySpatialLayer[actualLayer])
		{
			sample.feature = decision.actionFeatureBySpatialLayer[actualLayer];
		}
		else
		{
			// Cold-start 등에서 per-layer feature가 없는 경우를 위한 fallback.
			// 이 경우 decision layer와 실제 layer가 같을 때만 사용.
			if (record.currentSpatialLayer != static_cast<int16_t>(decision.spatialLayer))
			{
				return false;
			}

			sample.feature = decision.feature;
		}

		// yeon: historical sample의 실제 action identity.
		// decision.spatialLayer는 CAP이므로 사용하지 않는다.
		// 실제 viewer로 전송된 action을 기록한다.
		sample.action.spatialLayer        = static_cast<uint8_t>(actualLayer);
		sample.action.fecProtectionFactor = record.fecProtectionFactor;
		sample.action.pacingEnabled       = record.pacingEnabled;

		// Y_i = decodeSlackLogical
		sample.decodeSlackMs = record.decodeSlackLogicalMs;
		//sample.decodeSlackMs = record.decodeSlackNominalMs;
		
		// 아래 LATE / DROP 판정은 record에서 저장했으므로 그대로 가져와서 사용
		if (!record.hasFrameOutcome)
		{
			return false;
		}

		sample.deadlineMiss = record.deadlineMiss;
		sample.outcome      = record.frameOutcome;

		record.knnSampleAdded = true;

		return true;
	}

} // namespace RTC