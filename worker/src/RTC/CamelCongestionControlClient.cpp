#define MS_CLASS "RTC::CamelCongestionControlClient"

#include "RTC/CamelCongestionControlClient.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "RTC/Consumer.hpp"
#include "RTC/RTCP/FeedbackRtpTransport.hpp"
#include "RTC/RTCP/ReceiverReport.hpp"
#include "RTC/RtpPacket.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace RTC
{
	CamelCongestionControlClient::CamelCongestionControlClient(
	  Listener* listener,
	  uint32_t initialAvailableBitrate,
	  uint32_t maxOutgoingBitrate,
	  uint32_t minOutgoingBitrate)
	  : listener(listener)
	{
		MS_TRACE();

		this->bitrates.availableBitrate = initialAvailableBitrate;
		this->bitrates.maxBitrate       = maxOutgoingBitrate;
		this->bitrates.minBitrate       = minOutgoingBitrate;

		// yeon: initial Camel burst length.
		this->bitrates.burstLengthBytes = InitialBurstLengthBytes;
		MaybeClampAvailableBitrate();
	}

	// Camel client는 packet이 나갈 때마다 이 packet이 어떤 frame에 속하는지 기록
	void CamelCongestionControlClient::InsertPacket(
	  const webrtc::RtpPacketSendInfo& packetInfo,
	  RTC::RtpPacket* packet,
	  RTC::Consumer* consumer,
	  int64_t nowMs)
	{
		MS_TRACE();

		// TODO:
		// 여기서 transport-wide seq -> frameId mapping을 저장할 예정.
		// 현재는 stub이라서 아무 동작 없음.

		// 다음 단계에서 여기서 transport-wide seq -> frame 정보 매핑을 저장한다.
		//
		// 필요한 값:
		// - packetInfo.transport_sequence_number
		// - packetInfo.rtp_sequence_number
		// - packetInfo.ssrc
		// - packetInfo.length
		// - packet->GetTimestamp()
		// - packet->GetMarker()
		// - consumer id / layer 정보
		// - nowMs
		(void)consumer;

		if (!packet)
		{
			return;
		}

		const auto wideSeq = static_cast<uint16_t>(packetInfo.transport_sequence_number);

		FrameKey frameKey;
		frameKey.ssrc         = packet->GetSsrc();
		frameKey.rtpTimestamp = packet->GetTimestamp();

		auto& currentFrameBytes = this->frameBytesSoFar[frameKey];

		PacketRecord record;
		record.wideSeq            = wideSeq; // 시퀸스 넘버
		record.ssrc               = packet->GetSsrc();
		record.rtpTimestamp       = packet->GetTimestamp();
		record.rtpSequenceNumber  = packet->GetSequenceNumber();
		record.sizeBytes          = packetInfo.length;
		record.offsetInFrameBytes = currentFrameBytes;
		record.marker             = packet->HasMarker();
		record.insertTimeMs       = nowMs;
		record.sentTimeMs         = 0;
		record.feedbackSeen       = false;
		record.received           = false;
		record.recvTimeMs         = 0;
		record.inFlightCounted    = false;

		this->packetRecords[wideSeq] = record;

		auto& frame = this->frameStates[frameKey];

		if (frame.packetCount == 0u)
		{
			frame.key          = frameKey;
			frame.firstWideSeq = wideSeq;
		}

		frame.lastWideSeq = wideSeq;
		frame.packetCount++;
		frame.frameSizeBytes += record.sizeBytes;
		frame.markerSeen   = frame.markerSeen || record.marker;
		frame.lastUpdateMs = nowMs;

		currentFrameBytes += record.sizeBytes;

		// RTP marker bit이면 해당 RTP timestamp frame은 끝났다고 보고
		// 다음 frame offset 계산을 위해 누적 값을 제거한다.
		if (record.marker)
		{
			this->frameBytesSoFar.erase(frameKey);
		}

		PruneOldRecords(nowMs);
	}

	void CamelCongestionControlClient::PacketSent(const webrtc::RtpPacketSendInfo& packetInfo, int64_t nowMs)
	{
		MS_TRACE();

		// TODO:
		// packet 실제 send time 저장 예정.
		const auto wideSeq = static_cast<uint16_t>(packetInfo.transport_sequence_number);

		auto packetIt = this->packetRecords.find(wideSeq);

		if (packetIt == this->packetRecords.end())
		{
			return;
		}

		auto& record      = packetIt->second;
		record.sentTimeMs = nowMs;

		if (!record.inFlightCounted)
		{
			this->outstandingBytes += record.sizeBytes;
			record.inFlightCounted = true;
		}

		FrameKey frameKey;
		frameKey.ssrc         = record.ssrc;
		frameKey.rtpTimestamp = record.rtpTimestamp;

		auto frameIt = this->frameStates.find(frameKey);

		if (frameIt == this->frameStates.end())
		{
			return;
		}

		auto& frame = frameIt->second;

		if (frame.firstSentTimeMs == 0 || nowMs < frame.firstSentTimeMs)
		{
			frame.firstSentTimeMs = nowMs;

			// 이 frame의 첫 packet이 실제로 나간 시점의 inflight.
			frame.inflightBytesAtFirstSent = this->outstandingBytes;
		}

		if (nowMs > frame.lastSentTimeMs)
		{
			frame.lastSentTimeMs = nowMs;
		}

		frame.lastUpdateMs = nowMs;
	}

	void CamelCongestionControlClient::AddBurstLossSample(
	  const PacketRecord& record, bool lost, int64_t nowMs)
	{
		// frame 안에서 이 packet이 몇 번째 2KB 구간에 속하는지 계산.
		// offset 0~2047     -> 2KB interval
		// offset 2048~4095  -> 4KB interval
		// offset 4096~6143  -> 6KB interval
		// BurstIntervalBytes = 2048
		const size_t intervalIndex = (record.offsetInFrameBytes / BurstIntervalBytes) + 1u;

		BurstLossSample sample;
		sample.sampleTimeMs       = nowMs;
		sample.intervalUpperBytes = intervalIndex * BurstIntervalBytes;
		sample.lost               = lost;

		this->burstLossSamples.emplace_back(sample);
	}

	void CamelCongestionControlClient::PruneBurstLossSamples(int64_t nowMs)
	{
		while (!this->burstLossSamples.empty())
		{
			const auto& sample = this->burstLossSamples.front();

			if (nowMs - sample.sampleTimeMs > EstimateWindowMs)
			{
				this->burstLossSamples.pop_front();
			}
			else
			{
				break;
			}
		}
	}

	/*
	TWCC packet result 수신
	→ transport-wide seq로 packetRecords 조회
	→ 해당 packet이 속한 frameState 업데이트
	→ frame feedback이 완료되면 B_i 계산 시도
	*/
	void CamelCongestionControlClient::ReceiveRtcpTransportFeedback(
	  const RTC::RTCP::FeedbackRtpTransportPacket* feedback)
	{
		MS_TRACE();

		if (!feedback)
		{
			return;
		}

		const auto nowMs = DepLibUV::GetTimeMsInt64();

		const size_t expectedPackets = feedback->GetPacketStatusCount();

		if (expectedPackets == 0u)
		{
			return;
		}

		size_t lostPackets = 0u;
		std::vector<FrameKey> candidateFrames;

		for (const auto& result : feedback->GetPacketResults())
		{
			auto packetIt = this->packetRecords.find(result.sequenceNumber);

			if (packetIt == this->packetRecords.end())
			{
				continue;
			}

			auto& record = packetIt->second;

			// 중복 feedback 방어.
			if (record.feedbackSeen)
			{
				continue;
			}

			record.feedbackSeen = true;
			record.received     = result.received;

			// 해당 패킷이 inflight로 체크된 경우 피드백을 받았으므로 inflight에서 빼야 함
			if (record.inFlightCounted)
			{
				if (this->outstandingBytes >= record.sizeBytes)
				{
					this->outstandingBytes -= record.sizeBytes;
				}
				else
				{
					this->outstandingBytes = 0u;
				}

				record.inFlightCounted = false;
			}

			const bool lost = !result.received;

			// Bursting Length Controller용 packet loss sample.
			// FeedbackRtpTransportPacket::GetPacketResults()는 packet별 received 여부와 receivedAtMs를
			// 채워주므로, 여기서 loss sample을 만들 수 있음
			// 피드백이 온 패킷들은 packet별 received 여부와 receivedAtMs를 알 수 있으므로 도착한 모든
			// 패킷들에 대해 샘플에 추가
			AddBurstLossSample(record, lost, nowMs);

			if (result.received)
			{
				record.recvTimeUs  = result.receivedAtUs;
				record.hasRecvTime = true;
				// MS_WARN_TAG(
				//   bwe,
				//   "camel recv time set [wideSeq:%" PRIu16 ", delta250Us:%" PRIi16 ", recvTimeUs:%" PRIi64 "]",
				//   result.sequenceNumber,
				//   result.delta,
				//   record.recvTimeUs);
			}
			else
			{
				record.recvTimeUs  = 0;
				record.hasRecvTime = false;
				lostPackets++;
			}

			FrameKey frameKey;
			frameKey.ssrc         = record.ssrc;
			frameKey.rtpTimestamp = record.rtpTimestamp;

			auto frameIt = this->frameStates.find(frameKey);

			if (frameIt == this->frameStates.end())
			{
				continue;
			}

			auto& frame = frameIt->second;

			frame.feedbackedPackets++;
			frame.lastUpdateMs = nowMs;

			if (result.received)
			{
				frame.receivedPackets++;

				if (frame.firstRecvTimeMs == 0 || record.recvTimeMs < frame.firstRecvTimeMs)
				{
					frame.firstRecvTimeMs = record.recvTimeMs;
				}

				if (record.recvTimeMs > frame.lastRecvTimeMs)
				{
					frame.lastRecvTimeMs = record.recvTimeMs;
				}
			}
			else
			{
				frame.lostPackets++;
			}

			candidateFrames.emplace_back(frameKey);
		}

		this->bitrates.packetLoss = static_cast<double>(lostPackets) / expectedPackets;

		for (const auto& frameKey : candidateFrames)
		{
			MaybeFinalizeFrame(frameKey, nowMs);
		}

		UpdateBurstingLengthController(nowMs);

		PruneOldRecords(nowMs);
		PruneFrameSamples(nowMs);

		// PruneBurstLossSamples(nowMs);

		// TODO:
		// 다음 단계에서 여기서 transportSeq 기반으로 packet receive time을 읽고,
		// frame-level B, D, BDP, gamma, burstLength를 계산할 예정.
	}

	void CamelCongestionControlClient::ReceiveRtcpReceiverReport(
	  RTC::RTCP::ReceiverReportPacket* packet, float rtt, int64_t nowMs)
	{
		MS_TRACE();
		// MS_ERROR_STD("--Camel--");
		(void)packet;
		(void)nowMs;

		if (rtt <= 0.0f)
		{
			return;
		}

		this->bitrates.latestRttMs = static_cast<double>(rtt);

		// 현재 단계에서는 frame first-packet RTT를 아직 못 구하므로,
		// RR 기반 RTT를 Camel delay D의 근사값으로 저장한다.
		if (this->bitrates.minFrameDelayMs <= 0.0 || this->bitrates.latestRttMs < this->bitrates.minFrameDelayMs)
		{
			this->bitrates.minFrameDelayMs = this->bitrates.latestRttMs;
		}
	}

	void CamelCongestionControlClient::SetDesiredBitrate(uint32_t desiredBitrate, bool force)
	{
		MS_TRACE();

		(void)force;

		this->bitrates.desiredBitrate = desiredBitrate;

		// Stub 단계에서는 available bitrate를 바꾸지 않음.
		// 실제 Camel estimator가 들어오면 gamma * avg(B)를 availableBitrate로 설정.
	}

	void CamelCongestionControlClient::SetMaxOutgoingBitrate(uint32_t maxBitrate)
	{
		MS_TRACE();

		this->bitrates.maxBitrate = maxBitrate;

		MaybeClampAvailableBitrate();
	}

	void CamelCongestionControlClient::SetMinOutgoingBitrate(uint32_t minBitrate)
	{
		MS_TRACE();

		this->bitrates.minBitrate = minBitrate;

		MaybeClampAvailableBitrate();
	}

	void CamelCongestionControlClient::MaybeClampAvailableBitrate()
	{
		if (this->bitrates.maxBitrate > 0u)
		{
			this->bitrates.availableBitrate =
			  std::min(this->bitrates.availableBitrate, this->bitrates.maxBitrate);
		}

		if (this->bitrates.minBitrate > 0u)
		{
			this->bitrates.availableBitrate =
			  std::max(this->bitrates.availableBitrate, this->bitrates.minBitrate);
		}
	}

	// 단순한 버전: 단순히 첫번째 샘플과 마지막 샘플 값 차이를 구함
	void CamelCongestionControlClient::UpdateCongestionDetector()
	{
		if (this->frameSamples.size() < 2u)
		{
			this->bitrates.congested                    = false;
			this->bitrates.delayInflightGradientMsPerKb = 0.0;
			this->bitrates.inflightBytes                = static_cast<double>(this->outstandingBytes);
			return;
		}

		const auto& first = this->frameSamples.front();
		const auto& last  = this->frameSamples.back();

		const double deltaDelayMs       = last.delayMs - first.delayMs;
		const double deltaInflightBytes = last.inflightBytes - first.inflightBytes;

		this->bitrates.inflightBytes = last.inflightBytes;

		if (!std::isfinite(deltaDelayMs) || !std::isfinite(deltaInflightBytes) || deltaInflightBytes <= 0.0)
		{
			this->bitrates.congested                    = false;
			this->bitrates.delayInflightGradientMsPerKb = 0.0;

			// 논문 설명에 따르면 congestion이 아니면 gamma를 1로 복귀.
			this->bitrates.gamma = 1.0;

			return;
		}

		const double deltaInflightKb = deltaInflightBytes / 1024.0;

		if (deltaInflightKb <= 0.0)
		{
			this->bitrates.congested                    = false;
			this->bitrates.delayInflightGradientMsPerKb = 0.0;
			this->bitrates.gamma                        = 1.0;

			return;
		}

		const double gradientMsPerKb = deltaDelayMs / deltaInflightKb;

		this->bitrates.delayInflightGradientMsPerKb = gradientMsPerKb;

		const bool congested = gradientMsPerKb > CongestionGradientThresholdMsPerKb;

		this->bitrates.congested = congested;

		if (congested)
		{
			this->bitrates.gamma *= GammaDecreaseFactor;

			if (this->bitrates.gamma < MinGamma)
			{
				this->bitrates.gamma = MinGamma;
			}
		}
		else
		{
			this->bitrates.gamma = 1.0;
		}
	}

	void CamelCongestionControlClient::UpdateBurstingLengthController(int64_t nowMs)
	{
		PruneBurstLossSamples(nowMs);

		if (this->burstLossSamples.empty())
		{
			return;
		}

		size_t baselineTotal = 0u;
		size_t baselineLost  = 0u;

		size_t currentTotal = 0u;
		size_t currentLost  = 0u;

		const size_t currentBurstLength = this->bitrates.burstLengthBytes > 0u
		                                    ? this->bitrates.burstLengthBytes
		                                    : InitialBurstLengthBytes;

		for (const auto& sample : this->burstLossSamples)
		{
			// L0: 첫 2KB interval의 loss rate.
			if (sample.intervalUpperBytes <= BurstIntervalBytes)
			{
				baselineTotal++;

				if (sample.lost)
				{
					baselineLost++;
				}
			}

			// Li: 현재 burst length 근처, 즉 currentBurstLength 이하의 tail interval.
			// 예: burstLength=12KB이면 10~12KB 주변 packet들의 loss를 봄.
			if (sample.intervalUpperBytes > currentBurstLength - BurstIntervalBytes && sample.intervalUpperBytes <= currentBurstLength)
			{
				currentTotal++;

				if (sample.lost)
				{
					currentLost++;
				}
			}
		}

		if (baselineTotal == 0u || currentTotal == 0u)
		{
			return;
		}

		const double baselineLossRate =
		  static_cast<double>(baselineLost) / static_cast<double>(baselineTotal);

		const double currentLossRate =
		  static_cast<double>(currentLost) / static_cast<double>(currentTotal);

		this->bitrates.baselineLossRate = baselineLossRate;
		this->bitrates.burstLossRate    = currentLossRate;

		const bool tailLossTooHigh = currentLossRate > baselineLossRate + BurstLossExtraThreshold;

		if (tailLossTooHigh)
		{
			if (currentBurstLength > MinBurstLengthBytes)
			{
				this->bitrates.burstLengthBytes =
				  static_cast<uint32_t>(currentBurstLength - BurstIntervalBytes);
			}
			else
			{
				this->bitrates.burstLengthBytes = MinBurstLengthBytes;

				// 매우 얕은 buffer로 판단.
				// 실제 GCC fallback 연결은 Transport 쪽에서 처리해야 한다.
				if (currentLossRate > ShallowBufferLossThreshold)
				{
					this->bitrates.fallbackToGcc = true;
				}
			}
		}
		else
		{
			this->bitrates.fallbackToGcc = false;

			if (currentBurstLength < MaxBurstLengthBytes)
			{
				this->bitrates.burstLengthBytes =
				  static_cast<uint32_t>(currentBurstLength + BurstIntervalBytes);
			}
			else
			{
				this->bitrates.burstLengthBytes = MaxBurstLengthBytes;
			}
		}
	}

	// 피드백이 도착했으므로, 지표들을 계산하기 위한 과정
	void CamelCongestionControlClient::MaybeFinalizeFrame(const FrameKey& frameKey, int64_t nowMs)
	{
		auto frameIt = this->frameStates.find(frameKey);

		if (frameIt == this->frameStates.end())
		{
			return;
		}

		auto& frame = frameIt->second;

		// MS_WARN_TAG(
		//   bwe,
		//   "camel finalize check [ssrc:%" PRIu32 ", ts:%" PRIu32
		//   ", packetCount:%zu, feedbacked:%zu, received:%zu, lost:%zu"
		//   ", markerSeen:%d, finalized:%d]",
		//   frame.key.ssrc,
		//   frame.key.rtpTimestamp,
		//   frame.packetCount,
		//   frame.feedbackedPackets,
		//   frame.receivedPackets,
		//   frame.lostPackets,
		//   frame.markerSeen ? 1 : 0,
		//   frame.finalized ? 1 : 0);

		if (frame.finalized)
		{
			return;
		}

		// marker를 본 frame만 완료 대상으로 본다.
		if (!frame.markerSeen)
		{
			return;
		}

		// frame에 속한 packet들의 feedback이 모두 도착해야 계산한다.
		if (frame.feedbackedPackets < frame.packetCount)
		{
			return;
		}

		frame.finalized = true;

		// MS_WARN_TAG(
		//   bwe,
		//   "camel finalize passed [ssrc:%" PRIu32 ", ts:%" PRIu32
		//   ", packetCount:%zu, received:%zu, lost:%zu"
		//   ", latestRttMs:%.3f]",
		//   frame.key.ssrc,
		//   frame.key.rtpTimestamp,
		//   frame.packetCount,
		//   frame.receivedPackets,
		//   frame.lostPackets,
		//   this->bitrates.latestRttMs);

		// 이번 초기 구현에서는 loss가 있는 frame은 bandwidth sample에서 제외한다.
		// 이유: Camel 수식은 frame 내 n개 packet의 receiving time을 전제로 하므로,
		// lost packet이 섞이면 B_i가 왜곡될 수 있다.
		if (frame.lostPackets > 0u)
		{
			EraseFrameRecords(frameKey);
			return;
		}

		std::vector<const PacketRecord*> receivedPackets;

		size_t sameFrameRecords          = 0u;
		size_t sameFrameFeedbackSeen     = 0u;
		size_t sameFrameReceived         = 0u;
		size_t sameFrameRecvTimePositive = 0u;

		for (const auto& kv : this->packetRecords)
		{
			const auto& record = kv.second;

			if (record.ssrc == frameKey.ssrc && record.rtpTimestamp == frameKey.rtpTimestamp)
			{
				if (record.feedbackSeen && record.received && record.hasRecvTime)
				{
					receivedPackets.emplace_back(&record);
				}
			}
		}

		if (receivedPackets.size() < 2u)
		{
			// MS_WARN_TAG(
			//   bwe,
			//   "camel sample skipped: not enough received packet records "
			//   "[ssrc:%" PRIu32 ", ts:%" PRIu32
			//   ", frameReceived:%zu, collected:%zu, packetRecords:%zu"
			//   ", framePacketCount:%zu, lost:%zu]",
			//   frame.key.ssrc,
			//   frame.key.rtpTimestamp,
			//   frame.receivedPackets,
			//   receivedPackets.size(),
			//   this->packetRecords.size(),
			//   frame.packetCount,
			//   frame.lostPackets);
			EraseFrameRecords(frameKey);
			return;
		}

		std::sort(
		  receivedPackets.begin(),
		  receivedPackets.end(),
		  [](const PacketRecord* a, const PacketRecord* b) { return a->recvTimeUs < b->recvTimeUs; });

		const auto firstRecvTimeUs = receivedPackets.front()->recvTimeUs;
		const auto lastRecvTimeUs  = receivedPackets.back()->recvTimeUs;

		const auto recvSpanUs = lastRecvTimeUs - firstRecvTimeUs;

		// MS_WARN_TAG(
		//   bwe,
		//   "camel sample input [ssrc:%" PRIu32 ", ts:%" PRIu32
		//   ", frameSize:%zu"
		//   ", packetCount:%zu"
		//   ", receivedPackets:%zu"
		//   ", firstRecvUs:%" PRIi64 ", lastRecvUs:%" PRIi64 ", recvSpanUs:%" PRIi64 ", latestRttMs:%.3f]",
		//   frame.key.ssrc,
		//   frame.key.rtpTimestamp,
		//   frame.frameSizeBytes,
		//   frame.packetCount,
		//   receivedPackets.size(),
		//   firstRecvTimeUs,
		//   lastRecvTimeUs,
		//   recvSpanUs,
		//   this->bitrates.latestRttMs);

		if (recvSpanUs <= 0)
		{
			EraseFrameRecords(frameKey);
			return;
		}

		size_t bytesExceptFirst = 0u;

		for (size_t i = 1u; i < receivedPackets.size(); ++i)
		{
			bytesExceptFirst += receivedPackets[i]->sizeBytes;
		}

		if (bytesExceptFirst == 0u)
		{
			EraseFrameRecords(frameKey);
			return;
		}

		// B = sum(S_i, i=2..n) / (t_recv_n - t_recv_1)
		// bytes/ms → bps
		const double bandwidthBps =
		  //   (static_cast<double>(bytesExceptFirst) * 8.0 * 1000.0) / static_cast<double>(recvSpans);
		  (static_cast<double>(bytesExceptFirst) * 8.0 * 1000000.0) / static_cast<double>(recvSpanUs);

		if (!std::isfinite(bandwidthBps) || bandwidthBps <= 0.0)
		{
			EraseFrameRecords(frameKey);
			return;
		}

		// 현재 단계에서는 D_i를 frame first packet RTT가 아니라,
		// 최신 RR RTT로 근사한다.
		const double delayMs = this->bitrates.latestRttMs;

		FrameSample sample;
		sample.key             = frameKey;
		sample.bandwidthBps    = bandwidthBps;
		sample.delayMs         = delayMs;
		sample.frameSizeBytes  = frame.frameSizeBytes;
		sample.packetCount     = frame.packetCount;
		sample.firstRecvTimeUs = firstRecvTimeUs;
		sample.lastRecvTimeUs  = lastRecvTimeUs;
		sample.sampleTimeMs    = nowMs;
		sample.inflightBytes   = static_cast<double>(frame.inflightBytesAtFirstSent);

		if (delayMs > 0.0)
		{
			sample.bdpBytes = (bandwidthBps / 8.0) * (delayMs / 1000.0);
		}

		this->frameSamples.emplace_back(sample);

		RecomputeEstimates(nowMs);

		EraseFrameRecords(frameKey);
	}

	// 새 샘플이 들어옴 => 샘플 구간을 슬라이딩 하고, 필요한 지표를 계산
	// 구간에서 수집된 샘플 데이터를 토대로 avg b, min d 계산
	// 샘플 구간의 기준이 되는 nowMs는 ReceiveRtcpTransportFeedback가 호출된 시점을 기준으로 함
	// RtcpTransportFeedback를 수신하면 구간이 생기는데, RtcpTransportFeedback를 수신 시점 ~ 5초
	// 이전을 의미 각 구간마다 쌓인 패킷 정보가 있고, 그 패킷 정보(수신 시간, 패킷 사이즈, 패킷의
	// RTT(물론 아직 구현은 안됨))를 사용하여 필요한 샘플 데이터를 관리함 이 구간 내에서 관리되는 샘플
	// 데이터를 samples에 추가하고, 그 samples에서 필요한 지표를 계산 (b, d)
	void CamelCongestionControlClient::RecomputeEstimates(int64_t nowMs)
	{
		// 현재 기준,
		PruneFrameSamples(nowMs);

		if (this->frameSamples.empty())
		{
			return;
		}

		double sumBandwidthBps  = 0.0;
		size_t bandwidthSamples = 0u;

		double minDelayMs = std::numeric_limits<double>::max();

		for (const auto& sample : this->frameSamples)
		{
			if (sample.bandwidthBps > 0.0 && std::isfinite(sample.bandwidthBps))
			{
				sumBandwidthBps += sample.bandwidthBps;
				bandwidthSamples++;
			}

			if (sample.delayMs > 0.0 && sample.delayMs < minDelayMs)
			{
				minDelayMs = sample.delayMs;
			}
		}

		if (bandwidthSamples == 0u)
		{
			return;
		}

		const double avgBandwidthBps = sumBandwidthBps / static_cast<double>(bandwidthSamples);

		this->bitrates.avgFrameBandwidthBps = avgBandwidthBps;

		if (minDelayMs != std::numeric_limits<double>::max())
		{
			this->bitrates.minFrameDelayMs = minDelayMs;
		}
		else if (this->bitrates.latestRttMs > 0.0)
		{
			this->bitrates.minFrameDelayMs = this->bitrates.latestRttMs;
		}

		if (this->bitrates.minFrameDelayMs > 0.0)
		{
			this->bitrates.bdpBytes =
			  (this->bitrates.avgFrameBandwidthBps / 8.0) * (this->bitrates.minFrameDelayMs / 1000.0);
		}

		UpdateCongestionDetector();

		this->bitrates.cwndBytes = this->bitrates.gamma * this->bitrates.bdpBytes;

		const double camelBitrateBps = this->bitrates.gamma * this->bitrates.avgFrameBandwidthBps;

		if (std::isfinite(camelBitrateBps) && camelBitrateBps > 0.0)
		{
			this->bitrates.availableBitrate = static_cast<uint32_t>(camelBitrateBps);
			MaybeClampAvailableBitrate();
		}

		if (this->listener)
		{
			this->listener->OnCamelCongestionControlClientBitrates(this, this->bitrates);
		}
	}

	void CamelCongestionControlClient::PruneFrameSamples(int64_t nowMs)
	{
		while (!this->frameSamples.empty())
		{
			const auto& sample = this->frameSamples.front();

			if (nowMs - sample.sampleTimeMs > EstimateWindowMs)
			{
				this->frameSamples.pop_front();
			}
			else
			{
				break;
			}
		}
	}

	// 오래된 packet record를 제거하기
	void CamelCongestionControlClient::PruneOldRecords(int64_t nowMs)
	{
		for (auto it = this->packetRecords.begin(); it != this->packetRecords.end();)
		{
			if (nowMs - it->second.insertTimeMs > PacketRecordTtlMs)
			{
				it = this->packetRecords.erase(it);
			}
			else
			{
				++it;
			}
		}

		for (auto it = this->frameStates.begin(); it != this->frameStates.end();)
		{
			if (nowMs - it->second.lastUpdateMs > FrameRecordTtlMs)
			{
				it = this->frameStates.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void CamelCongestionControlClient::EraseFrameRecords(const FrameKey& frameKey)
	{
		for (auto it = this->packetRecords.begin(); it != this->packetRecords.end();)
		{
			const auto& record = it->second;

			if (record.ssrc == frameKey.ssrc && record.rtpTimestamp == frameKey.rtpTimestamp)
			{
				it = this->packetRecords.erase(it);
			}
			else
			{
				++it;
			}
		}

		this->frameStates.erase(frameKey);
		this->frameBytesSoFar.erase(frameKey);
	}
} // namespace RTC