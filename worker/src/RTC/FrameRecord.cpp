#include "RTC/FrameRecord.hpp"

namespace RTC
{
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

		auto& builder = this->inProgressFrames[frameId];

		if (!builder.initialized)
		{
			builder.transportId = transportId;
			builder.consumerId  = consumerId;
			builder.producerId  = producerId;

			builder.frameId             = frameId;
			builder.firstPacketSentAtMs = nowMs;
			builder.initialized         = true;

			// 첫 패킷 기준 초기 layer 정보 저장
			builder.SpatialLayer          = SpatialLayer;
			builder.currentSpatialLayer   = currentSpatialLayer;
			builder.targetSpatialLayer    = targetSpatialLayer;
			builder.preferredSpatialLayer = preferredSpatialLayer;

			builder.pacingEnabled = pacingEnabled;
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

		// === 첫 프레임이면 기준 프레임으로 설정 ===
		if (!this->hasReferenceFrame)
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

			UpdateDerivedTimingMetrics(record);
			return true;
		}

		// === 후속 프레임이면 RTP timestamp 차이 기반으로 desired time 계산 ===
		int64_t deltaRtp = static_cast<int64_t>(frameId) - static_cast<int64_t>(this->referenceFrameId);
		double deltaMs   = static_cast<double>(deltaRtp) * 1000.0 / 90000.0;

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

} // namespace RTC