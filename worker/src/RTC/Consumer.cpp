#define MS_CLASS "RTC::Consumer"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/Consumer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <inttypes.h>

static inline double NowEpochMs()
{
	using namespace std::chrono;
	return duration<double, std::milli>(system_clock::now().time_since_epoch()).count();
}
namespace
{
	double Percentile(std::vector<double> values, double q)
	{
		if (values.empty())
		{
			return 0.0;
		}

		std::sort(values.begin(), values.end());

		if (values.size() == 1)
		{
			return values.front();
		}

		const double pos  = q * static_cast<double>(values.size() - 1);
		const auto idx    = static_cast<size_t>(pos);
		const auto next   = std::min(idx + 1, values.size() - 1);
		const double frac = pos - static_cast<double>(idx);

		return values[idx] * (1.0 - frac) + values[next] * frac;
	}
} // namespace

namespace RTC
{
	/* Instance methods. */

	Consumer::Consumer(
	  RTC::Shared* shared,
	  const std::string& id,
	  const std::string& producerId,
	  Listener* listener,
	  const FBS::Transport::ConsumeRequest* data,
	  RTC::RtpParameters::Type type)
	  : id(id), producerId(producerId), shared(shared), listener(listener),
	    kind(RTC::Media::Kind(data->kind())), type(type)
	{
		MS_TRACE();

		// This may throw.
		this->rtpParameters = RTC::RtpParameters(data->rtpParameters());

		if (this->rtpParameters.encodings.empty())
		{
			MS_THROW_TYPE_ERROR("empty rtpParameters.encodings");
		}

		// All encodings must have SSRCs.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			if (encoding.ssrc == 0)
			{
				MS_THROW_TYPE_ERROR("invalid encoding in rtpParameters (missing ssrc)");
			}
			else if (encoding.hasRtx && encoding.rtx.ssrc == 0)
			{
				MS_THROW_TYPE_ERROR("invalid encoding in rtpParameters (missing rtx.ssrc)");
			}
			else if (encoding.hasFlexFec && encoding.flexfec.ssrc == 0) // yeon: fec
			{
				MS_THROW_TYPE_ERROR(
				  "invalid encoding in rtpParameters "
				  "(missing flexfec.ssrc)");
			}
		}

		if (data->consumableRtpEncodings()->size() == 0)
		{
			MS_THROW_TYPE_ERROR("empty consumableRtpEncodings");
		}

		this->consumableRtpEncodings.reserve(data->consumableRtpEncodings()->size());

		for (size_t i{ 0 }; i < data->consumableRtpEncodings()->size(); ++i)
		{
			const auto* entry = data->consumableRtpEncodings()->Get(i);

			// This may throw due the constructor of RTC::RtpEncodingParameters.
			this->consumableRtpEncodings.emplace_back(entry);

			// Verify that it has ssrc field.
			auto& encoding = this->consumableRtpEncodings[i];

			if (encoding.ssrc == 0u)
			{
				MS_THROW_TYPE_ERROR("wrong encoding in consumableRtpEncodings (missing ssrc)");
			}
		}

		// Fill RTP header extension ids and their mapped values.
		// This may throw.
		for (auto& exten : this->rtpParameters.headerExtensions)
		{
			if (exten.id == 0u)
			{
				MS_THROW_TYPE_ERROR("RTP extension id cannot be 0");
			}

			if (this->rtpHeaderExtensionIds.ssrcAudioLevel == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::SSRC_AUDIO_LEVEL)
			{
				this->rtpHeaderExtensionIds.ssrcAudioLevel = exten.id;
			}

			if (this->rtpHeaderExtensionIds.videoOrientation == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::VIDEO_ORIENTATION)
			{
				this->rtpHeaderExtensionIds.videoOrientation = exten.id;
			}

			if (this->rtpHeaderExtensionIds.playoutDelay == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::PLAYOUT_DELAY)
			{
				this->rtpHeaderExtensionIds.playoutDelay = exten.id;
			}

			if (this->rtpHeaderExtensionIds.absSendTime == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME)
			{
				this->rtpHeaderExtensionIds.absSendTime = exten.id;
			}

			if (this->rtpHeaderExtensionIds.transportWideCc01 == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01)
			{
				this->rtpHeaderExtensionIds.transportWideCc01 = exten.id;
			}

			if (this->rtpHeaderExtensionIds.mid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::MID)
			{
				this->rtpHeaderExtensionIds.mid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rrid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::REPAIRED_RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rrid = exten.id;
			}
		}

		// paused is set to false by default.
		this->paused = data->paused();

		// Fill supported codec payload types.
		for (auto& codec : this->rtpParameters.codecs)
		{
			if (codec.mimeType.IsMediaCodec())
			{
				this->supportedCodecPayloadTypes[codec.payloadType] = true;
			}
		}

		// Fill media SSRCs vector.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			this->mediaSsrcs.push_back(encoding.ssrc);
		}

		// Fill RTX SSRCs vector.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			if (encoding.hasRtx)
			{
				this->rtxSsrcs.push_back(encoding.rtx.ssrc);
			}
		}

		// yeon: fec
		// Log FlexFEC repair SSRCs.
		for (auto& encoding : this->rtpParameters.encodings)
		{
			if (encoding.hasFlexFec)
			{
				MS_WARN_TAG(
				  rtp,
				  "[FlexFEC] Consumer parsed repair SSRC "
				  "[consumerId:%s, mediaSsrc:%" PRIu32 ", flexfecSsrc:%" PRIu32 "]",
				  this->id.c_str(),
				  encoding.ssrc,
				  encoding.flexfec.ssrc);
			}
		}

		// Set the RTCP report generation interval.
		if (this->kind == RTC::Media::Kind::AUDIO)
		{
			this->maxRtcpInterval = RTC::RTCP::MaxAudioIntervalMs;
		}
		else
		{
			this->maxRtcpInterval = RTC::RTCP::MaxVideoIntervalMs;
		}
	}

	Consumer::~Consumer()
	{
		MS_TRACE();
	}

	flatbuffers::Offset<FBS::Consumer::BaseConsumerDump> Consumer::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Add rtpParameters.
		auto rtpParameters = this->rtpParameters.FillBuffer(builder);

		// Add consumableRtpEncodings.
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> consumableRtpEncodings;
		consumableRtpEncodings.reserve(this->consumableRtpEncodings.size());

		for (const auto& encoding : this->consumableRtpEncodings)
		{
			consumableRtpEncodings.emplace_back(encoding.FillBuffer(builder));
		}

		// Add supportedCodecPayloadTypes.
		std::vector<uint8_t> supportedCodecPayloadTypes;

		for (auto i = 0; i < 128; ++i)
		{
			if (this->supportedCodecPayloadTypes[i])
			{
				supportedCodecPayloadTypes.push_back(i);
			}
		}

		// Add traceEventTypes.
		std::vector<FBS::Consumer::TraceEventType> traceEventTypes;

		if (this->traceEventTypes.rtp)
		{
			traceEventTypes.emplace_back(FBS::Consumer::TraceEventType::RTP);
		}
		if (this->traceEventTypes.keyframe)
		{
			traceEventTypes.emplace_back(FBS::Consumer::TraceEventType::KEYFRAME);
		}
		if (this->traceEventTypes.nack)
		{
			traceEventTypes.emplace_back(FBS::Consumer::TraceEventType::NACK);
		}
		if (this->traceEventTypes.pli)
		{
			traceEventTypes.emplace_back(FBS::Consumer::TraceEventType::PLI);
		}
		if (this->traceEventTypes.fir)
		{
			traceEventTypes.emplace_back(FBS::Consumer::TraceEventType::FIR);
		}

		return FBS::Consumer::CreateBaseConsumerDumpDirect(
		  builder,
		  this->id.c_str(),
		  RTC::RtpParameters::TypeToFbs(this->type),
		  this->producerId.c_str(),
		  this->kind == RTC::Media::Kind::AUDIO ? FBS::RtpParameters::MediaKind::AUDIO
		                                        : FBS::RtpParameters::MediaKind::VIDEO,
		  rtpParameters,
		  &consumableRtpEncodings,
		  &supportedCodecPayloadTypes,
		  &traceEventTypes,
		  this->paused,
		  this->producerPaused,
		  this->priority);
	}

	void Consumer::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::CONSUMER_GET_STATS:
			{
				auto responseOffset = FillBufferStats(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Consumer_GetStatsResponse, responseOffset);

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_PAUSE:
			{
				if (this->paused)
				{
					request->Accept();

					break;
				}

				const bool wasActive = IsActive();

				this->paused = true;

				MS_DEBUG_DEV("Consumer paused [consumerId:%s]", this->id.c_str());

				if (wasActive)
				{
					UserOnPaused();
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_RESUME:
			{
				if (!this->paused)
				{
					request->Accept();

					break;
				}

				this->paused = false;

				MS_DEBUG_DEV("Consumer resumed [consumerId:%s]", this->id.c_str());

				if (IsActive())
				{
					UserOnResumed();
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_SET_PRIORITY:
			{
				const auto* body = request->data->body_as<FBS::Consumer::SetPriorityRequest>();

				if (body->priority() < 1u)
				{
					MS_THROW_TYPE_ERROR("wrong priority (must be higher than 0)");
				}

				this->priority = body->priority();

				auto responseOffset =
				  FBS::Consumer::CreateSetPriorityResponse(request->GetBufferBuilder(), this->priority);

				request->Accept(FBS::Response::Body::Consumer_SetPriorityResponse, responseOffset);

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_ENABLE_TRACE_EVENT:
			{
				const auto* body = request->data->body_as<FBS::Consumer::EnableTraceEventRequest>();

				// Reset traceEventTypes.
				struct TraceEventTypes newTraceEventTypes;

				for (const auto& type : *body->events())
				{
					switch (type)
					{
						case FBS::Consumer::TraceEventType::KEYFRAME:
						{
							newTraceEventTypes.keyframe = true;

							break;
						}
						case FBS::Consumer::TraceEventType::FIR:
						{
							newTraceEventTypes.fir = true;

							break;
						}
						case FBS::Consumer::TraceEventType::NACK:
						{
							newTraceEventTypes.nack = true;

							break;
						}
						case FBS::Consumer::TraceEventType::PLI:
						{
							newTraceEventTypes.pli = true;

							break;
						}
						case FBS::Consumer::TraceEventType::RTP:
						{
							newTraceEventTypes.rtp = true;

							break;
						}
					}
				}

				this->traceEventTypes = newTraceEventTypes;

				request->Accept();

				break;
			}

				// case Channel::ChannelRequest::Method::CONSUMER_GET_SYNC_CLOCK:
				// {
				// 	const auto nowMs = this->GetSyncClockMs();
				// 	//const auto* body = request->data->body_as<FBS::Consumer::SetRecvDeadlineRequest>();

				// 	auto responseOffset =
				// 	  FBS::Consumer::CreateGetSyncClockResponse(
				// 	    request->GetBufferBuilder(), nowMs);

				// 	request->Accept(
				// 	  FBS::Response::Body::Consumer_GetSyncClockResponse,
				// 	  responseOffset);

				// 	break;
				// }

			default:
			{
				MS_THROW_ERROR("unknown method '%s'", request->methodCStr);
			}
		}
	}

	void Consumer::TransportConnected()
	{
		MS_TRACE();

		if (this->transportConnected)
		{
			return;
		}

		this->transportConnected = true;

		MS_DEBUG_DEV("Transport connected [consumerId:%s]", this->id.c_str());

		UserOnTransportConnected();
	}

	void Consumer::TransportDisconnected()
	{
		MS_TRACE();

		if (!this->transportConnected)
		{
			return;
		}

		this->transportConnected = false;

		MS_DEBUG_DEV("Transport disconnected [consumerId:%s]", this->id.c_str());

		UserOnTransportDisconnected();
	}

	void Consumer::ProducerPaused()
	{
		MS_TRACE();

		if (this->producerPaused)
		{
			return;
		}

		const bool wasActive = IsActive();

		this->producerPaused = true;

		MS_DEBUG_DEV("Producer paused [consumerId:%s]", this->id.c_str());

		if (wasActive)
		{
			UserOnPaused();
		}

		this->shared->channelNotifier->Emit(this->id, FBS::Notification::Event::CONSUMER_PRODUCER_PAUSE);
	}

	void Consumer::ProducerResumed()
	{
		MS_TRACE();

		if (!this->producerPaused)
		{
			return;
		}

		this->producerPaused = false;

		MS_DEBUG_DEV("Producer resumed [consumerId:%s]", this->id.c_str());

		if (IsActive())
		{
			UserOnResumed();
		}

		this->shared->channelNotifier->Emit(this->id, FBS::Notification::Event::CONSUMER_PRODUCER_RESUME);
	}

	void Consumer::ProducerRtpStreamScores(const std::vector<uint8_t>* scores)
	{
		MS_TRACE();

		// This is gonna be a constant pointer.
		this->producerRtpStreamScores = scores;
	}

	// The caller (Router) is supposed to proceed with the deletion of this Consumer
	// right after calling this method. Otherwise ugly things may happen.
	void Consumer::ProducerClosed()
	{
		MS_TRACE();

		this->producerClosed = true;

		MS_DEBUG_DEV("Producer closed [consumerId:%s]", this->id.c_str());

		this->shared->channelNotifier->Emit(this->id, FBS::Notification::Event::CONSUMER_PRODUCER_CLOSE);

		this->listener->OnConsumerProducerClosed(this);
	}

	void Consumer::EmitTraceEventRtpAndKeyFrameTypes(RTC::RtpPacket* packet, bool isRtx) const
	{
		MS_TRACE();

		if (this->traceEventTypes.keyframe && packet->IsKeyFrame())
		{
			auto rtpPacketDump = packet->FillBuffer(this->shared->channelNotifier->GetBufferBuilder());
			auto traceInfo     = FBS::Consumer::CreateKeyFrameTraceInfo(
			  this->shared->channelNotifier->GetBufferBuilder(), rtpPacketDump, isRtx);

			auto notification = FBS::Consumer::CreateTraceNotification(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  FBS::Consumer::TraceEventType::KEYFRAME,
			  DepLibUV::GetTimeMs(),
			  FBS::Common::TraceDirection::DIRECTION_OUT,
			  FBS::Consumer::TraceInfo::KeyFrameTraceInfo,
			  traceInfo.Union());

			EmitTraceEvent(notification);
		}
		else if (this->traceEventTypes.rtp)
		{
			auto rtpPacketDump = packet->FillBuffer(this->shared->channelNotifier->GetBufferBuilder());
			auto traceInfo     = FBS::Consumer::CreateRtpTraceInfo(
			  this->shared->channelNotifier->GetBufferBuilder(), rtpPacketDump, isRtx);

			auto notification = FBS::Consumer::CreateTraceNotification(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  FBS::Consumer::TraceEventType::RTP,
			  DepLibUV::GetTimeMs(),
			  FBS::Common::TraceDirection::DIRECTION_OUT,
			  FBS::Consumer::TraceInfo::RtpTraceInfo,
			  traceInfo.Union());

			EmitTraceEvent(notification);
		}
	}

	void Consumer::EmitTraceEventPliType(uint32_t ssrc) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.pli)
		{
			return;
		}

		auto traceInfo =
		  FBS::Consumer::CreatePliTraceInfo(this->shared->channelNotifier->GetBufferBuilder(), ssrc);

		auto notification = FBS::Consumer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Consumer::TraceEventType::PLI,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_IN,
		  FBS::Consumer::TraceInfo::PliTraceInfo,
		  traceInfo.Union());

		EmitTraceEvent(notification);
	}

	void Consumer::EmitTraceEventFirType(uint32_t ssrc) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.fir)
		{
			return;
		}

		auto traceInfo =
		  FBS::Consumer::CreateFirTraceInfo(this->shared->channelNotifier->GetBufferBuilder(), ssrc);

		auto notification = FBS::Consumer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Consumer::TraceEventType::FIR,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_IN,
		  FBS::Consumer::TraceInfo::FirTraceInfo,
		  traceInfo.Union());

		EmitTraceEvent(notification);
	}

	void Consumer::EmitTraceEventNackType() const
	{
		MS_TRACE();

		if (!this->traceEventTypes.nack)
		{
			return;
		}

		auto notification = FBS::Consumer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Consumer::TraceEventType::NACK,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_IN);

		EmitTraceEvent(notification);
	}

	void Consumer::EmitTraceEvent(flatbuffers::Offset<FBS::Consumer::TraceNotification>& notification) const
	{
		MS_TRACE();

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::CONSUMER_TRACE,
		  FBS::Notification::Body::Consumer_TraceNotification,
		  notification);
	}

	// yun
	// void Consumer::SetRecvDeadline(
	//   const std::string& producerId,
	//   uint32_t rtpTimestamp,
	//   const std::string& latestDecodeTimeNtp,
	//   double oneWayDelay)
	// {
	// 	this->recvDeadlineInfo.producerId          = producerId;
	// 	this->recvDeadlineInfo.rtpTimestamp        = rtpTimestamp;
	// 	this->recvDeadlineInfo.latestDecodeTimeNtp = latestDecodeTimeNtp;
	// 	this->recvDeadlineInfo.oneWayDelay         = oneWayDelay;

	// 	MS_ERROR_STD(
	// 	  "recv-deadline updated [consumerId:%s, producerId:%s, rtpTimestamp:%u, ntp:%s, delay:%f]",
	// 	  this->id.c_str(),
	// 	  producerId.c_str(),
	// 	  rtpTimestamp,
	// 	  latestDecodeTimeNtp.c_str(),
	// 	  oneWayDelay);
	// }

	// const RecvDeadlineInfo& Consumer::GetRecvDeadlineInfo() const
	// {
	// 	return this->recvDeadlineInfo;
	// }

	// double Consumer::GetSyncClockMs() const
	// {
	// 	return NowEpochMs();
	// }

	bool Consumer::UpdateDecodeSlackLayerCap(double decodeSlackNominalMs, int64_t nowMs)
	{
		MS_TRACE();

		const int8_t currentSpatialLayer = GetCurrentSpatialLayer();

		if (currentSpatialLayer < 0)
		{
			return false;
		}

		if (!std::isfinite(decodeSlackNominalMs))
		{
			return false;
		}

		if (decodeSlackNominalMs < -10000.0 || decodeSlackNominalMs > 100000.0)
		{
			return false;
		}

		static constexpr int64_t BaselineWindowMs{ 10000 };
		static constexpr int64_t DecisionWindowMs{ 1000 };
		static constexpr int64_t HoldDownMs{ 3000 };

		static constexpr size_t MinBaselineSamples{ 20u };
		static constexpr size_t MinDecisionSamples{ 5u };

		// 절대값 기준이 아니라 baseline 대비 비율 기준.
		static constexpr double BadSlackRatio{ 0.65 };
		static constexpr double SevereSlackRatio{ 0.45 };

		static constexpr size_t BadCountThreshold{ 3u };
		static constexpr size_t SevereCountThreshold{ 2u };

		static constexpr double BadFrameRatioThreshold{ 0.15 };
		static constexpr double SevereFrameRatioThreshold{ 0.08 };

		this->decodeSlackSamples.push_back({ nowMs, decodeSlackNominalMs, currentSpatialLayer });

		while (!this->decodeSlackSamples.empty() &&
		       nowMs - this->decodeSlackSamples.front().timeMs > BaselineWindowMs)
		{
			this->decodeSlackSamples.pop_front();
		}

		std::vector<double> baselineValues;
		std::vector<double> decisionValues;

		baselineValues.reserve(this->decodeSlackSamples.size());
		decisionValues.reserve(this->decodeSlackSamples.size());

		// 우선 현재 spatial layer의 최근 slack만으로 baseline을 만든다.
		for (const auto& sample : this->decodeSlackSamples)
		{
			if (sample.spatialLayer != currentSpatialLayer)
			{
				continue;
			}

			baselineValues.emplace_back(sample.slackMs);

			if (nowMs - sample.timeMs <= DecisionWindowMs)
			{
				decisionValues.emplace_back(sample.slackMs);
			}
		}

		// 현재 layer sample이 너무 적으면, 초기 구간이라고 보고 전체 sample로 baseline만 대체한다.
		// 그래도 상수값은 사용하지 않는다.
		if (baselineValues.size() < MinBaselineSamples)
		{
			baselineValues.clear();

			for (const auto& sample : this->decodeSlackSamples)
			{
				baselineValues.emplace_back(sample.slackMs);
			}
		}

		if (baselineValues.size() < MinBaselineSamples)
		{
			return false;
		}

		if (decisionValues.size() < MinDecisionSamples)
		{
			return false;
		}

		// 최근 정상 slack 수준.
		// p80을 쓰는 이유:
		// - 평균은 bad frame이 섞이면 내려감
		// - min/p10은 문제 frame에 너무 민감함
		// - p80은 최근 window 안의 "정상적인 높은 slack"을 baseline으로 보기 좋음
		const double baselineSlackMs = Percentile(baselineValues, 0.80);

		if (!std::isfinite(baselineSlackMs) || baselineSlackMs <= 0.0)
		{
			return false;
		}

		this->decodeSlackBaselineMs = baselineSlackMs;

		const double badThreshold    = baselineSlackMs * BadSlackRatio;
		const double severeThreshold = baselineSlackMs * SevereSlackRatio;

		size_t badCount{ 0u };
		size_t severeCount{ 0u };

		for (const auto slackMs : decisionValues)
		{
			if (slackMs < badThreshold)
			{
				badCount++;
			}

			if (slackMs < severeThreshold)
			{
				severeCount++;
			}
		}

		const double badFrameRatio =
		  static_cast<double>(badCount) / static_cast<double>(decisionValues.size());

		const double severeFrameRatio =
		  static_cast<double>(severeCount) / static_cast<double>(decisionValues.size());

		const double shortP10SlackMs = Percentile(decisionValues, 0.10);

		int8_t newSlackMaxSpatialLayer{ this->slackMaxSpatialLayer };

		if (severeCount >= SevereCountThreshold || severeFrameRatio >= SevereFrameRatioThreshold || shortP10SlackMs < severeThreshold)
		{
			newSlackMaxSpatialLayer = 0;
			this->slackHoldUntilMs  = nowMs + HoldDownMs;
		}
		else if (badCount >= BadCountThreshold || badFrameRatio >= BadFrameRatioThreshold || shortP10SlackMs < badThreshold)
		{
			newSlackMaxSpatialLayer = std::max<int8_t>(0, currentSpatialLayer - 1);
			this->slackHoldUntilMs  = nowMs + HoldDownMs;
		}
		else if (nowMs < this->slackHoldUntilMs)
		{
			newSlackMaxSpatialLayer = this->slackMaxSpatialLayer;
		}
		else
		{
			newSlackMaxSpatialLayer = -1;
		}

		const bool changed = newSlackMaxSpatialLayer != this->slackMaxSpatialLayer;

		if (changed)
		{
			MS_WARN_TAG(
			  bwe,
			  "decode slack spatial cap changed "
			  "[old:%d, new:%d, currentSpatial:%d, slack:%.2f, baselineP80:%.2f, "
			  "badThreshold:%.2f, severeThreshold:%.2f, shortP10:%.2f, "
			  "badCount:%zu/%zu, severeCount:%zu/%zu, badRatio:%.3f, severeRatio:%.3f]",
			  static_cast<int>(this->slackMaxSpatialLayer),
			  static_cast<int>(newSlackMaxSpatialLayer),
			  static_cast<int>(currentSpatialLayer),
			  decodeSlackNominalMs,
			  baselineSlackMs,
			  badThreshold,
			  severeThreshold,
			  shortP10SlackMs,
			  badCount,
			  decisionValues.size(),
			  severeCount,
			  decisionValues.size(),
			  badFrameRatio,
			  severeFrameRatio);

			this->slackMaxSpatialLayer = newSlackMaxSpatialLayer;
		}

		return changed;
	}

} // namespace RTC
