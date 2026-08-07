#include "flatbuffers/stl_emulation.h"
#define MS_CLASS "RTC::Transport"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/Transport.hpp"
#ifdef MS_LIBURING_SUPPORTED
#include "DepLibUring.hpp"
#endif
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "FBS/transport.h"
#include "RTC/BweType.hpp"
#include "RTC/Consts.hpp"
#include "RTC/PipeConsumer.hpp"
#include "RTC/RTCP/FeedbackPs.hpp"
#include "RTC/RTCP/FeedbackPsAfb.hpp"
#include "RTC/RTCP/FeedbackPsRemb.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/RTCP/FeedbackRtpTransport.hpp"
#include "RTC/RTCP/XrDelaySinceLastRr.hpp"
#include "RTC/RtpDictionaries.hpp"
#include "RTC/SimpleConsumer.hpp"
#include "RTC/SimulcastConsumer.hpp"
#include "RTC/SvcConsumer.hpp"
#ifdef MS_RTC_LOGGER_RTP
#include "RTC/RtcLogger.hpp"
#endif
#include <libwebrtc/modules/rtp_rtcp/include/rtp_rtcp_defines.h> // webrtc::RtpPacketSendInfo
#include <iterator>                                              // std::ostream_iterator
#include <map>                                                   // std::multimap

#include <fstream>
#include <iomanip>
#include <unordered_map>

namespace RTC
{
	static const size_t DefaultSctpSendBufferSize{ 262144 }; // 2^18.
	static const size_t MaxSctpSendBufferSize{ 268435456 };  // 2^28.

	/* Instance methods. */

	Transport::Transport(
	  RTC::Shared* shared,
	  const std::string& id,
	  RTC::Transport::Listener* listener,
	  const FBS::Transport::Options* options)
	  : id(id), shared(shared), listener(listener),
	    recvRtpTransmission(/*ignorePaddingOnlyPackets*/ false),
	    sendRtpTransmission(/*ignorePaddingOnlyPackets*/ false),
	    recvRtxTransmission(/*ignorePaddingOnlyPackets*/ false, 1000u),
	    sendRtxTransmission(/*ignorePaddingOnlyPackets*/ false, 1000u),
	    sendProbationTransmission(/*ignorePaddingOnlyPackets*/ false, 100u)
	{
		MS_TRACE();

		if (options->direct())
		{
			this->direct = true;

			if (auto maxMessageSize = options->maxMessageSize(); maxMessageSize.has_value())
			{
				this->maxMessageSize = maxMessageSize.value();
			}
		}

		if (
		  auto initialAvailableOutgoingBitrate = options->initialAvailableOutgoingBitrate();
		  initialAvailableOutgoingBitrate.has_value())
		{
			this->initialAvailableOutgoingBitrate = initialAvailableOutgoingBitrate.value();
		}

		if (options->enableSctp())
		{
			if (this->direct)
			{
				MS_THROW_TYPE_ERROR("cannot enable SCTP in a direct Transport");
			}

			// numSctpStreams is mandatory.
			if (!flatbuffers::IsFieldPresent(options, FBS::Transport::Options::VT_NUMSCTPSTREAMS))
			{
				MS_THROW_TYPE_ERROR("numSctpStreams missing");
			}

			// maxSctpMessageSize is mandatory.
			if (!flatbuffers::IsFieldPresent(options, FBS::Transport::Options::VT_MAXSCTPMESSAGESIZE))
			{
				MS_THROW_TYPE_ERROR("maxSctpMessageSize missing");
			}

			this->maxMessageSize = options->maxSctpMessageSize();

			size_t sctpSendBufferSize;

			// sctpSendBufferSize is optional.
			if (flatbuffers::IsFieldPresent(options, FBS::Transport::Options::VT_SCTPSENDBUFFERSIZE))
			{
				if (options->sctpSendBufferSize() > MaxSctpSendBufferSize)
				{
					MS_THROW_TYPE_ERROR("wrong sctpSendBufferSize (maximum value exceeded)");
				}

				sctpSendBufferSize = options->sctpSendBufferSize();
			}
			else
			{
				sctpSendBufferSize = DefaultSctpSendBufferSize;
			}

			// This may throw.
			this->sctpAssociation = new RTC::SctpAssociation(
			  this,
			  options->numSctpStreams()->os(),
			  options->numSctpStreams()->mis(),
			  this->maxMessageSize,
			  sctpSendBufferSize,
			  options->isDataChannel());
		}

		// Create the RTCP timer.
		this->rtcpTimer = new TimerHandle(this);
	}

	Transport::~Transport()
	{
		MS_TRACE();

		// The destructor must delete and clear everything silently.

		// Delete all Producers.
		for (auto& kv : this->mapProducers)
		{
			auto* producer = kv.second;

			delete producer;
		}
		this->mapProducers.clear();

		// Delete all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			delete consumer;
		}
		this->mapConsumers.clear();
		this->mapSsrcConsumer.clear();
		this->mapRtxSsrcConsumer.clear();

		// Delete all DataProducers.
		for (auto& kv : this->mapDataProducers)
		{
			auto* dataProducer = kv.second;

			delete dataProducer;
		}
		this->mapDataProducers.clear();

		// Delete all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			delete dataConsumer;
		}
		this->mapDataConsumers.clear();

		// Delete SCTP association.
		delete this->sctpAssociation;
		this->sctpAssociation = nullptr;

		// Delete the RTCP timer.
		delete this->rtcpTimer;
		this->rtcpTimer = nullptr;
	}

	void Transport::CloseProducersAndConsumers()
	{
		MS_TRACE();

		// This method is called by the Router and must notify him about all Producers
		// and Consumers that we are gonna close.
		//
		// The caller is supposed to delete this Transport instance after calling
		// this method.

		// Close all Producers.
		for (auto& kv : this->mapProducers)
		{
			auto* producer = kv.second;

			// Notify the listener.
			this->listener->OnTransportProducerClosed(this, producer);

			delete producer;
		}
		this->mapProducers.clear();

		// Delete all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			// Notify the listener.
			this->listener->OnTransportConsumerClosed(this, consumer);

			delete consumer;
		}
		this->mapConsumers.clear();
		this->mapSsrcConsumer.clear();
		this->mapRtxSsrcConsumer.clear();

		// Delete all DataProducers.
		for (auto& kv : this->mapDataProducers)
		{
			auto* dataProducer = kv.second;

			// Notify the listener.
			this->listener->OnTransportDataProducerClosed(this, dataProducer);

			delete dataProducer;
		}
		this->mapDataProducers.clear();

		// Delete all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			// Notify the listener.
			this->listener->OnTransportDataConsumerClosed(this, dataConsumer);

			delete dataConsumer;
		}
		this->mapDataConsumers.clear();
	}

	void Transport::ListenServerClosed()
	{
		MS_TRACE();

		// Ask our parent Router to close/delete us.
		this->listener->OnTransportListenServerClosed(this);
	}

	flatbuffers::Offset<FBS::Transport::Dump> Transport::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Add producerIds.
		std::vector<flatbuffers::Offset<flatbuffers::String>> producerIds;

		for (const auto& kv : this->mapProducers)
		{
			const auto& producerId = kv.first;

			producerIds.emplace_back(builder.CreateString(producerId));
		}

		// Add consumerIds.
		std::vector<flatbuffers::Offset<flatbuffers::String>> consumerIds;

		for (const auto& kv : this->mapConsumers)
		{
			const auto& consumerId = kv.first;

			consumerIds.emplace_back(builder.CreateString(consumerId));
		}

		// Add mapSsrcConsumerId.
		std::vector<flatbuffers::Offset<FBS::Common::Uint32String>> mapSsrcConsumerId;

		for (const auto& kv : this->mapSsrcConsumer)
		{
			auto ssrc      = kv.first;
			auto* consumer = kv.second;

			mapSsrcConsumerId.emplace_back(
			  FBS::Common::CreateUint32StringDirect(builder, ssrc, consumer->id.c_str()));
		}

		// Add mapRtxSsrcConsumerId.
		std::vector<flatbuffers::Offset<FBS::Common::Uint32String>> mapRtxSsrcConsumerId;

		for (const auto& kv : this->mapRtxSsrcConsumer)
		{
			auto ssrc      = kv.first;
			auto* consumer = kv.second;

			mapRtxSsrcConsumerId.emplace_back(
			  FBS::Common::CreateUint32StringDirect(builder, ssrc, consumer->id.c_str()));
		}

		// Add dataProducerIds.
		std::vector<flatbuffers::Offset<flatbuffers::String>> dataProducerIds;

		for (const auto& kv : this->mapDataProducers)
		{
			const auto& dataProducerId = kv.first;

			dataProducerIds.emplace_back(builder.CreateString(dataProducerId));
		}

		// Add dataConsumerIds.
		std::vector<flatbuffers::Offset<flatbuffers::String>> dataConsumerIds;

		for (const auto& kv : this->mapDataConsumers)
		{
			const auto& dataConsumerId = kv.first;

			dataConsumerIds.emplace_back(builder.CreateString(dataConsumerId));
		}

		// Add headerExtensionIds.
		auto recvRtpHeaderExtensions = FBS::Transport::CreateRecvRtpHeaderExtensions(
		  builder,
		  this->recvRtpHeaderExtensionIds.mid != 0u
		    ? flatbuffers::Optional<uint8_t>(this->recvRtpHeaderExtensionIds.mid)
		    : flatbuffers::nullopt,
		  this->recvRtpHeaderExtensionIds.rid != 0u
		    ? flatbuffers::Optional<uint8_t>(this->recvRtpHeaderExtensionIds.rid)
		    : flatbuffers::nullopt,
		  this->recvRtpHeaderExtensionIds.rrid != 0u
		    ? flatbuffers::Optional<uint8_t>(this->recvRtpHeaderExtensionIds.rrid)
		    : flatbuffers::nullopt,
		  this->recvRtpHeaderExtensionIds.absSendTime != 0u
		    ? flatbuffers::Optional<uint8_t>(this->recvRtpHeaderExtensionIds.absSendTime)
		    : flatbuffers::nullopt,
		  this->recvRtpHeaderExtensionIds.transportWideCc01 != 0u
		    ? flatbuffers::Optional<uint8_t>(this->recvRtpHeaderExtensionIds.transportWideCc01)
		    : flatbuffers::nullopt);

		auto rtpListenerOffset = this->rtpListener.FillBuffer(builder);

		// Add sctpParameters.
		flatbuffers::Offset<FBS::SctpParameters::SctpParameters> sctpParameters;
		// Add sctpState.
		FBS::SctpAssociation::SctpState sctpState{ FBS::SctpAssociation::SctpState::NEW };
		// Add sctpListener.
		flatbuffers::Offset<FBS::Transport::SctpListener> sctpListener;

		if (this->sctpAssociation)
		{
			// Add sctpParameters.
			sctpParameters = this->sctpAssociation->FillBuffer(builder);

			switch (this->sctpAssociation->GetState())
			{
				case RTC::SctpAssociation::SctpState::NEW:
				{
					sctpState = FBS::SctpAssociation::SctpState::NEW;
					break;
				}

				case RTC::SctpAssociation::SctpState::CONNECTING:
				{
					sctpState = FBS::SctpAssociation::SctpState::CONNECTING;
					break;
				}

				case RTC::SctpAssociation::SctpState::CONNECTED:
				{
					sctpState = FBS::SctpAssociation::SctpState::CONNECTED;
					break;
				}

				case RTC::SctpAssociation::SctpState::FAILED:
				{
					sctpState = FBS::SctpAssociation::SctpState::FAILED;
					break;
				}

				case RTC::SctpAssociation::SctpState::CLOSED:
				{
					sctpState = FBS::SctpAssociation::SctpState::CLOSED;
					break;
				}
			}

			sctpListener = this->sctpListener.FillBuffer(builder);
		}

		// Add traceEventTypes.
		std::vector<FBS::Transport::TraceEventType> traceEventTypes;

		if (this->traceEventTypes.probation)
		{
			traceEventTypes.emplace_back(FBS::Transport::TraceEventType::PROBATION);
		}
		if (this->traceEventTypes.bwe)
		{
			traceEventTypes.emplace_back(FBS::Transport::TraceEventType::BWE);
		}

		return FBS::Transport::CreateDumpDirect(
		  builder,
		  this->id.c_str(),
		  this->direct,
		  &producerIds,
		  &consumerIds,
		  &mapSsrcConsumerId,
		  &mapRtxSsrcConsumerId,
		  &dataProducerIds,
		  &dataConsumerIds,
		  recvRtpHeaderExtensions,
		  rtpListenerOffset,
		  this->maxMessageSize,
		  sctpParameters,
		  this->sctpAssociation ? flatbuffers::Optional<FBS::SctpAssociation::SctpState>(sctpState)
		                        : flatbuffers::nullopt,
		  sctpListener,
		  &traceEventTypes);
	}

	flatbuffers::Offset<FBS::Transport::Stats> Transport::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		auto nowMs = DepLibUV::GetTimeMs();

		// Add sctpState.
		FBS::SctpAssociation::SctpState sctpState{ FBS::SctpAssociation::SctpState::NEW };

		if (this->sctpAssociation)
		{
			// Add sctpState.
			switch (this->sctpAssociation->GetState())
			{
				case RTC::SctpAssociation::SctpState::NEW:
				{
					sctpState = FBS::SctpAssociation::SctpState::NEW;
					break;
				}

				case RTC::SctpAssociation::SctpState::CONNECTING:
				{
					sctpState = FBS::SctpAssociation::SctpState::CONNECTING;
					break;
				}

				case RTC::SctpAssociation::SctpState::CONNECTED:
				{
					sctpState = FBS::SctpAssociation::SctpState::CONNECTED;
					break;
				}

				case RTC::SctpAssociation::SctpState::FAILED:
				{
					sctpState = FBS::SctpAssociation::SctpState::FAILED;
					break;
				}

				case RTC::SctpAssociation::SctpState::CLOSED:
				{
					sctpState = FBS::SctpAssociation::SctpState::CLOSED;
					break;
				}
			}
		}

		return FBS::Transport::CreateStatsDirect(
		  builder,
		  // transportId.
		  this->id.c_str(),
		  // timestamp.
		  nowMs,
		  // sctpState.
		  this->sctpAssociation ? flatbuffers::Optional<FBS::SctpAssociation::SctpState>(sctpState)
		                        : flatbuffers::nullopt,
		  // bytesReceived.
		  this->recvTransmission.GetBytes(),
		  // recvBitrate.
		  this->recvTransmission.GetRate(nowMs),
		  // bytesSent.
		  this->sendTransmission.GetBytes(),
		  // sendBitrate.
		  this->sendTransmission.GetRate(nowMs),
		  // rtpBytesReceived.
		  this->recvRtpTransmission.GetBytes(),
		  // rtpRecvBitrate.
		  this->recvRtpTransmission.GetBitrate(nowMs),
		  // rtpBytesSent.
		  this->sendRtpTransmission.GetBytes(),
		  // rtpSendBitrate.
		  this->sendRtpTransmission.GetBitrate(nowMs),
		  // rtxBytesReceived.
		  this->recvRtxTransmission.GetBytes(),
		  // rtxRecvBitrate.
		  this->recvRtxTransmission.GetBitrate(nowMs),
		  // rtxBytesSent.
		  this->sendRtxTransmission.GetBytes(),
		  // rtxSendBitrate.
		  this->sendRtxTransmission.GetBitrate(nowMs),
		  // probationBytesSent.
		  this->sendProbationTransmission.GetBytes(),
		  // probationSendBitrate.
		  this->sendProbationTransmission.GetBitrate(nowMs),
		  // availableOutgoingBitrate.
		  this->tccClient ? flatbuffers::Optional<uint32_t>(this->tccClient->GetAvailableBitrate())
		                  : flatbuffers::nullopt,
		  // availableIncomingBitrate.
		  this->tccServer ? flatbuffers::Optional<uint32_t>(this->tccServer->GetAvailableBitrate())
		                  : flatbuffers::nullopt,
		  // maxIncomingBitrate.
		  this->maxIncomingBitrate ? flatbuffers::Optional<uint32_t>(this->maxIncomingBitrate)
		                           : flatbuffers::nullopt,
		  // maxOutgoingBitrate.
		  this->maxOutgoingBitrate ? flatbuffers::Optional<uint32_t>(this->maxOutgoingBitrate)
		                           : flatbuffers::nullopt,
		  // minOutgoingBitrate.
		  this->minOutgoingBitrate ? flatbuffers::Optional<uint32_t>(this->minOutgoingBitrate)
		                           : flatbuffers::nullopt,
		  // rtpPacketLossReceived.
		  this->tccServer ? flatbuffers::Optional<double>(this->tccServer->GetPacketLoss())
		                  : flatbuffers::nullopt,
		  // rtpPacketLossSent.
		  this->tccClient ? flatbuffers::Optional<double>(this->tccClient->GetPacketLoss())
		                  : flatbuffers::nullopt);
	}

	void Transport::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::TRANSPORT_SET_MAX_INCOMING_BITRATE:
			{
				const auto* body = request->data->body_as<FBS::Transport::SetMaxIncomingBitrateRequest>();

				this->maxIncomingBitrate = body->maxIncomingBitrate();

				MS_DEBUG_TAG(bwe, "maximum incoming bitrate set to %" PRIu32, this->maxIncomingBitrate);

				request->Accept();

				if (this->tccServer)
				{
					this->tccServer->SetMaxIncomingBitrate(this->maxIncomingBitrate);
				}

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_SET_MAX_OUTGOING_BITRATE:
			{
				const auto* body = request->data->body_as<FBS::Transport::SetMaxOutgoingBitrateRequest>();
				const uint32_t bitrate = body->maxOutgoingBitrate();

				if (bitrate > 0u && bitrate < RTC::TransportCongestionControlMinOutgoingBitrate)
				{
					MS_THROW_TYPE_ERROR(
					  "bitrate must be >= %" PRIu32 " or 0 (unlimited)",
					  RTC::TransportCongestionControlMinOutgoingBitrate);
				}
				else if (bitrate > 0u && bitrate < this->minOutgoingBitrate)
				{
					MS_THROW_TYPE_ERROR(
					  "bitrate must be >= current min outgoing bitrate (%" PRIu32 ") or 0 (unlimited)",
					  this->minOutgoingBitrate);
				}

				// yeon: camel
				if (this->tccClient)
				{
					// NOTE: This may throw so don't update things before calling this
					// method.
					this->tccClient->SetMaxOutgoingBitrate(bitrate);

					if (this->camelClient)
					{
						this->camelClient->SetMaxOutgoingBitrate(bitrate);
					}
					this->maxOutgoingBitrate = bitrate;

					MS_DEBUG_TAG(bwe, "maximum outgoing bitrate set to %" PRIu32, this->maxOutgoingBitrate);

					ComputeOutgoingDesiredBitrate();
				}
				else
				{
					if (this->camelClient)
					{
						this->camelClient->SetMaxOutgoingBitrate(bitrate);
					}

					this->maxOutgoingBitrate = bitrate;
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_SET_MIN_OUTGOING_BITRATE:
			{
				const auto* body = request->data->body_as<FBS::Transport::SetMinOutgoingBitrateRequest>();
				const uint32_t bitrate = body->minOutgoingBitrate();

				if (bitrate > 0u && bitrate < RTC::TransportCongestionControlMinOutgoingBitrate)
				{
					MS_THROW_TYPE_ERROR(
					  "bitrate must be >= %" PRIu32 " or 0 (unlimited)",
					  RTC::TransportCongestionControlMinOutgoingBitrate);
				}
				else if (bitrate > 0u && this->maxOutgoingBitrate > 0 && bitrate > this->maxOutgoingBitrate)
				{
					MS_THROW_TYPE_ERROR(
					  "bitrate must be <= current max outgoing bitrate (%" PRIu32 ") or 0 (unlimited)",
					  this->maxOutgoingBitrate);
				}

				// yeon: camel
				if (this->tccClient)
				{
					// NOTE: This may throw so don't update things before calling this
					// method.
					this->tccClient->SetMinOutgoingBitrate(bitrate);
					if (this->camelClient)
					{
						this->camelClient->SetMinOutgoingBitrate(bitrate);
					}
					this->minOutgoingBitrate = bitrate;

					MS_DEBUG_TAG(bwe, "minimum outgoing bitrate set to %" PRIu32, this->minOutgoingBitrate);

					ComputeOutgoingDesiredBitrate();
				}
				else
				{
					if (this->camelClient)
					{
						this->camelClient->SetMinOutgoingBitrate(bitrate);
					}
					this->minOutgoingBitrate = bitrate;
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_PRODUCE:
			{
				const auto* body = request->data->body_as<FBS::Transport::ProduceRequest>();
				auto producerId  = body->producerId()->str();

				if (this->mapProducers.find(producerId) != this->mapProducers.end())
				{
					MS_THROW_ERROR("a Producer with same producerId already exists");
				}

				// This may throw.
				auto* producer = new RTC::Producer(this->shared, producerId, this, body);

				// Insert the Producer into the RtpListener.
				// This may throw. If so, delete the Producer and throw.
				try
				{
					this->rtpListener.AddProducer(producer);
				}
				catch (const MediaSoupError& error)
				{
					delete producer;

					throw;
				}

				// Notify the listener.
				// This may throw if a Producer with same id already exists.
				try
				{
					this->listener->OnTransportNewProducer(this, producer);
				}
				catch (const MediaSoupError& error)
				{
					this->rtpListener.RemoveProducer(producer);

					delete producer;

					throw;
				}

				// Insert into the map.
				this->mapProducers[producerId] = producer;

				MS_DEBUG_DEV("Producer created [producerId:%s]", producerId.c_str());

				// Take the transport related RTP header extensions of the Producer and
				// add them to the Transport.
				// NOTE: Producer::GetRtpHeaderExtensionIds() returns the original
				// header extension ids of the Producer (and not their mapped values).
				const auto& producerRtpHeaderExtensionIds = producer->GetRtpHeaderExtensionIds();

				if (producerRtpHeaderExtensionIds.mid != 0u)
				{
					this->recvRtpHeaderExtensionIds.mid = producerRtpHeaderExtensionIds.mid;
				}

				if (producerRtpHeaderExtensionIds.rid != 0u)
				{
					this->recvRtpHeaderExtensionIds.rid = producerRtpHeaderExtensionIds.rid;
				}

				if (producerRtpHeaderExtensionIds.rrid != 0u)
				{
					this->recvRtpHeaderExtensionIds.rrid = producerRtpHeaderExtensionIds.rrid;
				}

				if (producerRtpHeaderExtensionIds.absSendTime != 0u)
				{
					this->recvRtpHeaderExtensionIds.absSendTime = producerRtpHeaderExtensionIds.absSendTime;
				}

				if (producerRtpHeaderExtensionIds.transportWideCc01 != 0u)
				{
					this->recvRtpHeaderExtensionIds.transportWideCc01 =
					  producerRtpHeaderExtensionIds.transportWideCc01;
				}

				if (producerRtpHeaderExtensionIds.absCaptureTime != 0u)
				{
					this->recvRtpHeaderExtensionIds.absCaptureTime =
					  producerRtpHeaderExtensionIds.absCaptureTime;
				}

				// Create status response.
				auto responseOffset = FBS::Transport::CreateProduceResponse(
				  request->GetBufferBuilder(), FBS::RtpParameters::Type(producer->GetType()));

				request->Accept(FBS::Response::Body::Transport_ProduceResponse, responseOffset);

				// Check if TransportCongestionControlServer or REMB server must be
				// created.
				const auto& rtpHeaderExtensionIds = producer->GetRtpHeaderExtensionIds();
				const auto& codecs                = producer->GetRtpParameters().codecs;

				// Set TransportCongestionControlServer.
				if (!this->tccServer)
				{
					bool createTccServer{ false };
					RTC::BweType bweType;

					// Use transport-cc if:
					// - there is transport-wide-cc-01 RTP header extension, and
					// - there is "transport-cc" in codecs RTCP feedback.
					//
					// clang-format off
					if (
						rtpHeaderExtensionIds.transportWideCc01 != 0u &&
						std::any_of(
							codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
							{
								return std::any_of(
									codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RTC::RtcpFeedback& fb)
									{
										return fb.type == "transport-cc";
									});
							})
					)
					// clang-format on
					{
						MS_DEBUG_TAG(bwe, "enabling TransportCongestionControlServer with transport-cc");

						createTccServer = true;
						bweType         = RTC::BweType::TRANSPORT_CC;
					}
					// Use REMB if:
					// - there is abs-send-time RTP header extension, and
					// - there is "remb" in codecs RTCP feedback.
					//
					// clang-format off
					else if (
						rtpHeaderExtensionIds.absSendTime != 0u &&
						std::any_of(
							codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
							{
								return std::any_of(
									codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RTC::RtcpFeedback& fb)
									{
										return fb.type == "goog-remb";
									});
							})
					)
					// clang-format on
					{
						MS_DEBUG_TAG(bwe, "enabling TransportCongestionControlServer with REMB");

						createTccServer = true;
						bweType         = RTC::BweType::REMB;
					}

					if (createTccServer)
					{
						this->tccServer = std::make_shared<RTC::TransportCongestionControlServer>(
						  this, bweType, RTC::Consts::RtcpPacketMaxSize);

						if (this->maxIncomingBitrate != 0u)
						{
							this->tccServer->SetMaxIncomingBitrate(this->maxIncomingBitrate);
						}

						if (IsConnected())
						{
							this->tccServer->TransportConnected();
						}
					}
				}

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CONSUME:
			{
				const auto* body             = request->data->body_as<FBS::Transport::ConsumeRequest>();
				const std::string producerId = body->producerId()->str();
				const std::string consumerId = body->consumerId()->str();

				if (this->mapConsumers.find(consumerId) != this->mapConsumers.end())
				{
					MS_THROW_ERROR("a Consumer with same consumerId already exists");
				}

				auto type = RTC::RtpParameters::Type(body->type());

				RTC::Consumer* consumer{ nullptr };

				switch (type)
				{
					case RTC::RtpParameters::Type::SIMPLE:
					{
						// This may throw.
						consumer = new RTC::SimpleConsumer(this->shared, consumerId, producerId, this, body);

						break;
					}

					case RTC::RtpParameters::Type::SIMULCAST:
					{
						// This may throw.
						consumer = new RTC::SimulcastConsumer(this->shared, consumerId, producerId, this, body);

						break;
					}

					case RTC::RtpParameters::Type::SVC:
					{
						// This may throw.
						consumer = new RTC::SvcConsumer(this->shared, consumerId, producerId, this, body);

						break;
					}

					case RTC::RtpParameters::Type::PIPE:
					{
						// This may throw.
						consumer = new RTC::PipeConsumer(this->shared, consumerId, producerId, this, body);

						break;
					}
				}

				// Notify the listener.
				// This may throw if no Producer is found.
				try
				{
					this->listener->OnTransportNewConsumer(this, consumer, producerId);
				}
				catch (const MediaSoupError& error)
				{
					delete consumer;

					throw;
				}

				// Insert into the maps.
				this->mapConsumers[consumerId] = consumer;

				for (auto ssrc : consumer->GetMediaSsrcs())
				{
					this->mapSsrcConsumer[ssrc] = consumer;
				}

				for (auto ssrc : consumer->GetRtxSsrcs())
				{
					this->mapRtxSsrcConsumer[ssrc] = consumer;
				}

				MS_DEBUG_DEV(
				  "Consumer created [consumerId:%s, producerId:%s]", consumerId.c_str(), producerId.c_str());

				flatbuffers::Offset<FBS::Consumer::ConsumerLayers> preferredLayersOffset;
				auto preferredLayers = consumer->GetPreferredLayers();

				if (preferredLayers.spatial > -1 && preferredLayers.temporal > -1)
				{
					const flatbuffers::Optional<int16_t> preferredTemporalLayer{ preferredLayers.temporal };
					preferredLayersOffset = FBS::Consumer::CreateConsumerLayers(
					  request->GetBufferBuilder(), preferredLayers.spatial, preferredTemporalLayer);
				}

				auto scoreOffset    = consumer->FillBufferScore(request->GetBufferBuilder());
				auto responseOffset = FBS::Transport::CreateConsumeResponse(
				  request->GetBufferBuilder(),
				  consumer->IsPaused(),
				  consumer->IsProducerPaused(),
				  scoreOffset,
				  preferredLayersOffset);

				request->Accept(FBS::Response::Body::Transport_ConsumeResponse, responseOffset);

				// Check if Transport Congestion Control client must be created.
				const auto& rtpHeaderExtensionIds = consumer->GetRtpHeaderExtensionIds();
				const auto& codecs                = consumer->GetRtpParameters().codecs;

				// Set TransportCongestionControlClient.
				if (!this->tccClient)
				{
					bool createTccClient{ false };
					RTC::BweType bweType;

					// Use transport-cc if:
					// - it's a video Consumer, and
					// - there is transport-wide-cc-01 RTP header extension, and
					// - there is "transport-cc" in codecs RTCP feedback.
					//
					// clang-format off
						if (
								consumer->GetKind() == RTC::Media::Kind::VIDEO &&
								rtpHeaderExtensionIds.transportWideCc01 != 0u &&
								std::any_of(
									codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
									{
									return std::any_of(
											codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RTC::RtcpFeedback& fb)
											{
											return fb.type == "transport-cc";
											});
									})
							 )
					// clang-format on
					{
						MS_DEBUG_TAG(bwe, "enabling TransportCongestionControlClient with transport-cc");

						createTccClient = true;
						bweType         = RTC::BweType::TRANSPORT_CC;
					}
					// Use REMB if:
					// - it's a video Consumer, and
					// - there is abs-send-time RTP header extension, and
					// - there is "remb" in codecs RTCP feedback.
					//
					// clang-format off
						else if (
								consumer->GetKind() == RTC::Media::Kind::VIDEO &&
								rtpHeaderExtensionIds.absSendTime != 0u &&
								std::any_of(
									codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
									{
									return std::any_of(
											codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RTC::RtcpFeedback& fb)
											{
											return fb.type == "goog-remb";
											});
									})
								)
					// clang-format on
					{
						MS_DEBUG_TAG(bwe, "enabling TransportCongestionControlClient with REMB");

						createTccClient = true;
						bweType         = RTC::BweType::REMB;
					}

					if (createTccClient)
					{
						// Tell all the Consumers that we are gonna manage their bitrate.
						for (auto& kv : this->mapConsumers)
						{
							auto* consumer = kv.second;

							consumer->SetExternallyManagedBitrate();
						};

						this->tccClient = std::make_shared<RTC::TransportCongestionControlClient>(
						  this,
						  bweType,
						  this->initialAvailableOutgoingBitrate,
						  this->maxOutgoingBitrate,
						  this->minOutgoingBitrate);

						// Camel은 패킷별 도착 정보를 제공하는 TWCC 환경에서만 동작 가능.
						if (bweType == RTC::BweType::TRANSPORT_CC)
						{
							this->camelClient = std::make_shared<RTC::CamelCongestionControlClient>(
							  this,
							  this->initialAvailableOutgoingBitrate,
							  this->maxOutgoingBitrate,
							  this->minOutgoingBitrate);
						}

						if (IsConnected())
						{
							this->tccClient->TransportConnected();
						}
					}
				}

				// If applicable, tell the new Consumer that we are gonna manage its
				// bitrate.
				if (this->tccClient)
				{
					consumer->SetExternallyManagedBitrate();
				}

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
				// Create SenderBandwidthEstimator if:
				// - not already created,
				// - it's a video Consumer, and
				// - there is transport-wide-cc-01 RTP header extension, and
				// - there is "transport-cc" in codecs RTCP feedback.
				//
				// clang-format off
					if (
							!this->senderBwe &&
							consumer->GetKind() == RTC::Media::Kind::VIDEO &&
							rtpHeaderExtensionIds.transportWideCc01 != 0u &&
							std::any_of(
								codecs.begin(), codecs.end(), [](const RTC::RtpCodecParameters& codec)
								{
								return std::any_of(
										codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(), [](const RTC::RtcpFeedback& fb)
										{
										return fb.type == "transport-cc";
										});
								})
						 )
				// clang-format on
				{
					MS_DEBUG_TAG(bwe, "enabling SenderBandwidthEstimator");

					// Tell all the Consumers that we are gonna manage their bitrate.
					for (auto& kv : this->mapConsumers)
					{
						auto* consumer = kv.second;

						consumer->SetExternallyManagedBitrate();
					};

					this->senderBwe = std::make_shared<RTC::SenderBandwidthEstimator>(
					  this, this->initialAvailableOutgoingBitrate);

					if (IsConnected())
					{
						this->senderBwe->TransportConnected();
					}
				}

				// If applicable, tell the new Consumer that we are gonna manage its
				// bitrate.
				if (this->senderBwe)
				{
					consumer->SetExternallyManagedBitrate();
				}
#endif

				if (IsConnected())
				{
					consumer->TransportConnected();
				}

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_PRODUCE_DATA:
			{
				// Early check. The Transport must support SCTP or be direct.
				if (!this->sctpAssociation && !this->direct)
				{
					MS_THROW_ERROR("SCTP not enabled and not a direct Transport");
				}

				const auto* body = request->data->body_as<FBS::Transport::ProduceDataRequest>();

				auto dataProducerId = body->dataProducerId()->str();

				// This may throw.
				CheckNoDataProducer(dataProducerId);

				// This may throw.
				auto* dataProducer =
				  new RTC::DataProducer(this->shared, dataProducerId, this->maxMessageSize, this, body);

				// Verify the type of the DataProducer.
				switch (dataProducer->GetType())
				{
					case RTC::DataProducer::Type::SCTP:
					{
						if (!this->sctpAssociation)
						{
							delete dataProducer;

							MS_THROW_TYPE_ERROR(
							  "cannot create a DataProducer of type 'sctp', SCTP not enabled in this Transport");
							;
						}

						break;
					}

					case RTC::DataProducer::Type::DIRECT:
					{
						if (!this->direct)
						{
							delete dataProducer;

							MS_THROW_TYPE_ERROR(
							  "cannot create a DataProducer of type 'direct', not a direct Transport");
							;
						}

						break;
					}
				}

				if (dataProducer->GetType() == RTC::DataProducer::Type::SCTP)
				{
					// Insert the DataProducer into the SctpListener.
					// This may throw. If so, delete the DataProducer and throw.
					try
					{
						this->sctpListener.AddDataProducer(dataProducer);
					}
					catch (const MediaSoupError& error)
					{
						delete dataProducer;

						throw;
					}
				}

				// Notify the listener.
				// This may throw if a DataProducer with same id already exists.
				try
				{
					this->listener->OnTransportNewDataProducer(this, dataProducer);
				}
				catch (const MediaSoupError& error)
				{
					if (dataProducer->GetType() == RTC::DataProducer::Type::SCTP)
					{
						this->sctpListener.RemoveDataProducer(dataProducer);
					}

					delete dataProducer;

					throw;
				}

				// Insert into the map.
				this->mapDataProducers[dataProducerId] = dataProducer;

				MS_DEBUG_DEV("DataProducer created [dataProducerId:%s]", dataProducerId.c_str());

				auto dumpOffset = dataProducer->FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::DataProducer_DumpResponse, dumpOffset);

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CONSUME_DATA:
			{
				// Early check. The Transport must support SCTP or be direct.
				if (!this->sctpAssociation && !this->direct)
				{
					MS_THROW_ERROR("SCTP not enabled and not a direct Transport");
				}

				const auto* body = request->data->body_as<FBS::Transport::ConsumeDataRequest>();

				auto dataProducerId = body->dataProducerId()->str();
				auto dataConsumerId = body->dataConsumerId()->str();

				// This may throw.
				CheckNoDataConsumer(dataConsumerId);

				// This may throw.
				auto* dataConsumer = new RTC::DataConsumer(
				  this->shared,
				  dataConsumerId,
				  dataProducerId,
				  this->sctpAssociation,
				  this,
				  body,
				  this->maxMessageSize);

				// Verify the type of the DataConsumer.
				switch (dataConsumer->GetType())
				{
					case RTC::DataConsumer::Type::SCTP:
					{
						if (!this->sctpAssociation)
						{
							delete dataConsumer;

							MS_THROW_TYPE_ERROR(
							  "cannot create a DataConsumer of type 'sctp', SCTP not enabled in this Transport");
							;
						}

						break;
					}

					case RTC::DataConsumer::Type::DIRECT:
					{
						if (!this->direct)
						{
							delete dataConsumer;

							MS_THROW_TYPE_ERROR(
							  "cannot create a DataConsumer of type 'direct', not a direct Transport");
							;
						}

						break;
					}
				}

				// Notify the listener.
				// This may throw if no DataProducer is found.
				try
				{
					this->listener->OnTransportNewDataConsumer(this, dataConsumer, dataProducerId);
				}
				catch (const MediaSoupError& error)
				{
					delete dataConsumer;

					throw;
				}

				// Insert into the maps.
				this->mapDataConsumers[dataConsumerId] = dataConsumer;

				MS_DEBUG_DEV(
				  "DataConsumer created [dataConsumerId:%s, dataProducerId:%s]",
				  dataConsumerId.c_str(),
				  dataProducerId.c_str());

				auto dumpOffset = dataConsumer->FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::DataConsumer_DumpResponse, dumpOffset);

				if (IsConnected())
				{
					dataConsumer->TransportConnected();
				}

				if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
				{
					if (this->sctpAssociation->GetState() == RTC::SctpAssociation::SctpState::CONNECTED)
					{
						dataConsumer->SctpAssociationConnected();
					}

					// Tell to the SCTP association.
					this->sctpAssociation->HandleDataConsumer(dataConsumer);
				}

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_ENABLE_TRACE_EVENT:
			{
				const auto* body = request->data->body_as<FBS::Transport::EnableTraceEventRequest>();

				// Reset traceEventTypes.
				struct TraceEventTypes newTraceEventTypes;

				for (const auto& type : *body->events())
				{
					switch (type)
					{
						case FBS::Transport::TraceEventType::PROBATION:
						{
							newTraceEventTypes.probation = true;

							break;
						}

						case FBS::Transport::TraceEventType::BWE:
						{
							newTraceEventTypes.bwe = true;

							break;
						}
					}
				}

				this->traceEventTypes = newTraceEventTypes;

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CLOSE_PRODUCER:
			{
				const auto* body = request->data->body_as<FBS::Transport::CloseProducerRequest>();

				// This may throw.
				RTC::Producer* producer = GetProducerById(body->producerId()->str());

				// Remove it from the RtpListener.
				this->rtpListener.RemoveProducer(producer);

				// Remove it from the map.
				this->mapProducers.erase(producer->id);

				// Tell the child class to clear associated SSRCs.
				for (const auto& kv : producer->GetRtpStreams())
				{
					auto* rtpStream = kv.first;

					RecvStreamClosed(rtpStream->GetSsrc());

					if (rtpStream->HasRtx())
					{
						RecvStreamClosed(rtpStream->GetRtxSsrc());
					}
				}

				// Notify the listener.
				this->listener->OnTransportProducerClosed(this, producer);

				MS_DEBUG_DEV("Producer closed [producerId:%s]", producer->id.c_str());

				// Delete it.
				delete producer;

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CLOSE_CONSUMER:
			{
				const auto* body = request->data->body_as<FBS::Transport::CloseConsumerRequest>();

				// This may throw.
				RTC::Consumer* consumer = GetConsumerById(body->consumerId()->str());

				// Remove it from the maps.
				this->mapConsumers.erase(consumer->id);

				for (auto ssrc : consumer->GetMediaSsrcs())
				{
					this->mapSsrcConsumer.erase(ssrc);

					// Tell the child class to clear associated SSRCs.
					SendStreamClosed(ssrc);
				}

				for (auto ssrc : consumer->GetRtxSsrcs())
				{
					this->mapRtxSsrcConsumer.erase(ssrc);

					// Tell the child class to clear associated SSRCs.
					SendStreamClosed(ssrc);
				}

				// Notify the listener.
				this->listener->OnTransportConsumerClosed(this, consumer);

				MS_DEBUG_DEV("Consumer closed [consumerId:%s]", consumer->id.c_str());

				// Delete it.
				delete consumer;

				request->Accept();

				// This may be the latest active Consumer with BWE. If so we have to stop
				// probation.
				if (this->tccClient)
				{
					ComputeOutgoingDesiredBitrate(/*forceBitrate*/ true);
				}

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CLOSE_DATAPRODUCER:
			{
				const auto* body = request->data->body_as<FBS::Transport::CloseDataProducerRequest>();

				// This may throw.
				RTC::DataProducer* dataProducer = GetDataProducerById(body->dataProducerId()->str());

				if (dataProducer->GetType() == RTC::DataProducer::Type::SCTP)
				{
					// Remove it from the SctpListener.
					this->sctpListener.RemoveDataProducer(dataProducer);
				}

				// Remove it from the map.
				this->mapDataProducers.erase(dataProducer->id);

				// Notify the listener.
				this->listener->OnTransportDataProducerClosed(this, dataProducer);

				MS_DEBUG_DEV("DataProducer closed [dataProducerId:%s]", dataProducer->id.c_str());

				if (dataProducer->GetType() == RTC::DataProducer::Type::SCTP)
				{
					// Tell the SctpAssociation so it can reset the SCTP stream.
					this->sctpAssociation->DataProducerClosed(dataProducer);
				}

				// Delete it.
				delete dataProducer;

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_CLOSE_DATACONSUMER:
			{
				const auto* body = request->data->body_as<FBS::Transport::CloseDataConsumerRequest>();

				// This may throw.
				RTC::DataConsumer* dataConsumer = GetDataConsumerById(body->dataConsumerId()->str());

				// Remove it from the maps.
				this->mapDataConsumers.erase(dataConsumer->id);

				// Notify the listener.
				this->listener->OnTransportDataConsumerClosed(this, dataConsumer);

				MS_DEBUG_DEV("DataConsumer closed [dataConsumerId:%s]", dataConsumer->id.c_str());

				if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
				{
					// Tell the SctpAssociation so it can reset the SCTP stream.
					this->sctpAssociation->DataConsumerClosed(dataConsumer);
				}

				// Delete it.
				delete dataConsumer;

				request->Accept();

				break;
			}

			default:
			{
				MS_THROW_ERROR("unknown method '%s'", request->methodCStr);
			}
		}

		return;

		switch (request->method)
		{
			default:
			{
				MS_ERROR("unknown method");
			}
		}
	}

	void Transport::HandleNotification(Channel::ChannelNotification* notification)
	{
		MS_TRACE();

		switch (notification->event)
		{
			default:
			{
				MS_ERROR("unknown event '%s'", notification->eventCStr);
			}
		}
	}

	void Transport::Destroying()
	{
		MS_TRACE();

		this->destroying = true;
	}

	void Transport::Connected()
	{
		MS_TRACE();

		// Tell all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			consumer->TransportConnected();
		}

		// Tell all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			dataConsumer->TransportConnected();
		}

		// Tell the SctpAssociation.
		if (this->sctpAssociation)
		{
			this->sctpAssociation->TransportConnected();
		}

		// Start the RTCP timer.
		this->rtcpTimer->Start(static_cast<uint64_t>(RTC::RTCP::MaxVideoIntervalMs / 2));

		// Tell the TransportCongestionControlClient.
		if (this->tccClient)
		{
			this->tccClient->TransportConnected();
		}

		// Tell the TransportCongestionControlServer.
		if (this->tccServer)
		{
			this->tccServer->TransportConnected();
		}

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
		// Tell the SenderBandwidthEstimator.
		if (this->senderBwe)
		{
			this->senderBwe->TransportConnected();
		}
#endif
	}

	void Transport::Disconnected()
	{
		MS_TRACE();

		// Tell all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			consumer->TransportDisconnected();
		}

		// Tell all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			dataConsumer->TransportDisconnected();
		}

		// Stop the RTCP timer.
		this->rtcpTimer->Stop();

		// Tell the TransportCongestionControlClient.
		if (this->tccClient)
		{
			this->tccClient->TransportDisconnected();
		}

		// Tell the TransportCongestionControlServer.
		if (this->tccServer)
		{
			this->tccServer->TransportDisconnected();
		}

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
		// Tell the SenderBandwidthEstimator.
		if (this->senderBwe)
		{
			this->senderBwe->TransportDisconnected();
		}
#endif
	}

	void Transport::ReceiveRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.recvTransportId = this->id;
#endif

		// Apply the Transport RTP header extension ids so the RTP listener can use them.
		packet->SetMidExtensionId(this->recvRtpHeaderExtensionIds.mid);
		packet->SetRidExtensionId(this->recvRtpHeaderExtensionIds.rid);
		packet->SetRepairedRidExtensionId(this->recvRtpHeaderExtensionIds.rrid);
		packet->SetAbsSendTimeExtensionId(this->recvRtpHeaderExtensionIds.absSendTime);
		packet->SetTransportWideCc01ExtensionId(this->recvRtpHeaderExtensionIds.transportWideCc01);
		packet->SetAbsCaptureTimeExtensionId(this->recvRtpHeaderExtensionIds.absCaptureTime);

		auto nowMs = DepLibUV::GetTimeMs();

		// Feed the TransportCongestionControlServer.
		if (this->tccServer)
		{
			this->tccServer->IncomingPacket(nowMs, packet);
		}

		// Get the associated Producer.
		RTC::Producer* producer = this->rtpListener.GetProducer(packet);

		if (!producer)
		{
#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Discarded(RtcLogger::RtpPacket::DiscardReason::PRODUCER_NOT_FOUND);
#endif

			MS_WARN_TAG(
			  rtp,
			  "no suitable Producer for received RTP packet [ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]",
			  packet->GetSsrc(),
			  packet->GetPayloadType());

			// Tell the child class to remove this SSRC.
			RecvStreamClosed(packet->GetSsrc());

			delete packet;

			return;
		}

		// MS_DEBUG_DEV(
		//   "RTP packet received [ssrc:%" PRIu32 ", payloadType:%" PRIu8 ", producerId:%s]",
		//   packet->GetSsrc(),
		//   packet->GetPayloadType(),
		//   producer->id.c_str());

		// Pass the RTP packet to the corresponding Producer.
		auto result = producer->ReceiveRtpPacket(packet);

		switch (result)
		{
			case RTC::Producer::ReceiveRtpPacketResult::MEDIA:
			{
				this->recvRtpTransmission.Update(packet);
				break;
			}

			case RTC::Producer::ReceiveRtpPacketResult::RETRANSMISSION:
			{
				this->recvRtxTransmission.Update(packet);
				break;
			}

			case RTC::Producer::ReceiveRtpPacketResult::DISCARDED:
			{
				// Tell the child class to remove this SSRC.
				RecvStreamClosed(packet->GetSsrc());
				break;
			}

			default:;
		}

		delete packet;
	}

	void Transport::ReceiveRtcpPacket(RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		// Handle each RTCP packet.
		while (packet)
		{
			HandleRtcpPacket(packet);

			auto* previousPacket = packet;

			packet = packet->GetNext();

			delete previousPacket;
		}
	}

	void Transport::ReceiveSctpData(const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (!this->sctpAssociation)
		{
			MS_DEBUG_TAG(sctp, "ignoring SCTP packet (SCTP not enabled)");

			return;
		}

		// Pass it to the SctpAssociation.
		this->sctpAssociation->ProcessSctpData(data, len);
	}

	void Transport::CheckNoDataProducer(const std::string& dataProducerId) const
	{
		if (this->mapDataProducers.find(dataProducerId) != this->mapDataProducers.end())
		{
			MS_THROW_ERROR("a DataProducer with same dataProducerId already exists");
		}
	}

	void Transport::CheckNoDataConsumer(const std::string& dataConsumerId) const
	{
		MS_TRACE();

		if (this->mapDataConsumers.find(dataConsumerId) != this->mapDataConsumers.end())
		{
			MS_THROW_ERROR("a DataConsumer with same dataConsumerId already exists");
		}
	}

	RTC::Producer* Transport::GetProducerById(const std::string& producerId) const
	{
		MS_TRACE();

		auto it = this->mapProducers.find(producerId);

		if (it == this->mapProducers.end())
		{
			MS_THROW_ERROR("Producer not found");
		}

		return it->second;
	}

	RTC::Consumer* Transport::GetConsumerById(const std::string& consumerId) const
	{
		MS_TRACE();

		auto it = this->mapConsumers.find(consumerId);

		if (it == this->mapConsumers.end())
		{
			MS_THROW_ERROR("Consumer not found");
		}

		return it->second;
	}

	inline RTC::Consumer* Transport::GetConsumerByMediaSsrc(uint32_t ssrc) const
	{
		MS_TRACE();

		auto mapSsrcConsumerIt = this->mapSsrcConsumer.find(ssrc);

		if (mapSsrcConsumerIt == this->mapSsrcConsumer.end())
		{
			return nullptr;
		}

		auto* consumer = mapSsrcConsumerIt->second;

		return consumer;
	}

	inline RTC::Consumer* Transport::GetConsumerByRtxSsrc(uint32_t ssrc) const
	{
		MS_TRACE();

		auto mapRtxSsrcConsumerIt = this->mapRtxSsrcConsumer.find(ssrc);

		if (mapRtxSsrcConsumerIt == this->mapRtxSsrcConsumer.end())
		{
			return nullptr;
		}

		auto* consumer = mapRtxSsrcConsumerIt->second;

		return consumer;
	}

	RTC::DataProducer* Transport::GetDataProducerById(const std::string& dataProducerId) const
	{
		MS_TRACE();

		auto it = this->mapDataProducers.find(dataProducerId);

		if (it == this->mapDataProducers.end())
		{
			MS_THROW_ERROR("DataProducer not found");
		}

		return it->second;
	}

	RTC::DataConsumer* Transport::GetDataConsumerById(const std::string& dataConsumerId) const
	{
		MS_TRACE();

		auto it = this->mapDataConsumers.find(dataConsumerId);

		if (it == this->mapDataConsumers.end())
		{
			MS_THROW_ERROR("DataConsumer not found");
		}

		return it->second;
	}

	void Transport::HandleRtcpPacket(RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		switch (packet->GetType())
		{
			case RTC::RTCP::Type::RR:
			{
				auto* rr = static_cast<RTC::RTCP::ReceiverReportPacket*>(packet);

				for (auto it = rr->Begin(); it != rr->End(); ++it)
				{
					auto& report   = *it;
					auto* consumer = GetConsumerByMediaSsrc(report->GetSsrc());

					if (!consumer)
					{
						// Special case for the RTP probator.
						if (report->GetSsrc() == RTC::RtpProbationSsrc)
						{
							continue;
						}

						// Special case for (unused) RTCP-RR from the RTX stream.
						if (GetConsumerByRtxSsrc(report->GetSsrc()) != nullptr)
						{
							continue;
						}

						MS_DEBUG_TAG(
						  rtcp,
						  "no Consumer found for received Receiver Report [ssrc:%" PRIu32 "]",
						  report->GetSsrc());

						continue;
					}

					consumer->ReceiveRtcpReceiverReport(report);
				}

				if ((this->tccClient || this->camelClient) && !this->mapConsumers.empty())
				{
					float rtt = 0;

					// Retrieve the RTT from the first active consumer.
					for (auto& kv : this->mapConsumers)
					{
						auto* consumer = kv.second;

						if (consumer->IsActive())
						{
							rtt = consumer->GetRtt();

							break;
						}
					}
					// yeon: 이 받은 RTT값을 WebRTCTransport에 전달
					OnRttUpdated(rtt);
					// this->tccClient->ReceiveRtcpReceiverReport(rr, rtt, DepLibUV::GetTimeMsInt64());

					// yeon: camel
					if (this->tccClient)
					{
						this->tccClient->ReceiveRtcpReceiverReport(rr, rtt, DepLibUV::GetTimeMsInt64());
					}
					if (this->camelClient)
					{
						this->camelClient->ReceiveRtcpReceiverReport(rr, rtt, DepLibUV::GetTimeMsInt64());
					}
				}

				break;
			}

			case RTC::RTCP::Type::PSFB:
			{
				auto* feedback = static_cast<RTC::RTCP::FeedbackPsPacket*>(packet);

				switch (feedback->GetMessageType())
				{
					case RTC::RTCP::FeedbackPs::MessageType::PLI:
					{
						auto* consumer = GetConsumerByMediaSsrc(feedback->GetMediaSsrc());

						if (feedback->GetMediaSsrc() == RTC::RtpProbationSsrc)
						{
							break;
						}
						else if (!consumer)
						{
							MS_DEBUG_TAG(
							  rtcp,
							  "no Consumer found for received PLI Feedback packet "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							  feedback->GetSenderSsrc(),
							  feedback->GetMediaSsrc());

							break;
						}

						MS_DEBUG_TAG(
						  rtcp,
						  "PLI received, requesting key frame for Consumer "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  feedback->GetSenderSsrc(),
						  feedback->GetMediaSsrc());

						consumer->ReceiveKeyFrameRequest(
						  RTC::RTCP::FeedbackPs::MessageType::PLI, feedback->GetMediaSsrc());

						break;
					}

					case RTC::RTCP::FeedbackPs::MessageType::FIR:
					{
						// Must iterate FIR items.
						auto* fir = static_cast<RTC::RTCP::FeedbackPsFirPacket*>(packet);

						for (auto it = fir->Begin(); it != fir->End(); ++it)
						{
							auto& item     = *it;
							auto* consumer = GetConsumerByMediaSsrc(item->GetSsrc());

							if (item->GetSsrc() == RTC::RtpProbationSsrc)
							{
								continue;
							}
							else if (!consumer)
							{
								MS_DEBUG_TAG(
								  rtcp,
								  "no Consumer found for received FIR Feedback packet "
								  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 ", item ssrc:%" PRIu32 "]",
								  feedback->GetSenderSsrc(),
								  feedback->GetMediaSsrc(),
								  item->GetSsrc());

								continue;
							}

							MS_DEBUG_TAG(
							  rtcp,
							  "FIR received, requesting key frame for Consumer "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 ", item ssrc:%" PRIu32 "]",
							  feedback->GetSenderSsrc(),
							  feedback->GetMediaSsrc(),
							  item->GetSsrc());

							consumer->ReceiveKeyFrameRequest(feedback->GetMessageType(), item->GetSsrc());
						}

						break;
					}

					case RTC::RTCP::FeedbackPs::MessageType::AFB:
					{
						auto* afb = static_cast<RTC::RTCP::FeedbackPsAfbPacket*>(feedback);

						// Store REMB info.
						if (afb->GetApplication() == RTC::RTCP::FeedbackPsAfbPacket::Application::REMB)
						{
							auto* remb = static_cast<RTC::RTCP::FeedbackPsRembPacket*>(afb);

							// Pass it to the TCC client.
							// clang-format off
							if (
								this->tccClient &&
								this->tccClient->GetBweType() == RTC::BweType::REMB
							)
							// clang-format on
							{
								this->tccClient->ReceiveEstimatedBitrate(remb->GetBitrate());
							}

							break;
						}
						else
						{
							MS_DEBUG_TAG(
							  rtcp,
							  "ignoring unsupported %s Feedback PS AFB packet "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							  RTC::RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
							  feedback->GetSenderSsrc(),
							  feedback->GetMediaSsrc());

							break;
						}
					}

					default:
					{
						MS_DEBUG_TAG(
						  rtcp,
						  "ignoring unsupported %s Feedback packet "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  RTC::RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
						  feedback->GetSenderSsrc(),
						  feedback->GetMediaSsrc());
					}
				}

				break;
			}

			case RTC::RTCP::Type::RTPFB:
			{
				auto* feedback = static_cast<RTC::RTCP::FeedbackRtpPacket*>(packet);
				auto* consumer = GetConsumerByMediaSsrc(feedback->GetMediaSsrc());

				// If no Consumer is found and this is not a Transport Feedback for the
				// probation SSRC or any Consumer RTX SSRC, ignore it.
				//
				// clang-format off
				if (
					!consumer &&
					feedback->GetMessageType() != RTC::RTCP::FeedbackRtp::MessageType::TCC &&
					(
						feedback->GetMediaSsrc() != RTC::RtpProbationSsrc ||
						!GetConsumerByRtxSsrc(feedback->GetMediaSsrc())
					)
				)
				// clang-format on
				{
					MS_DEBUG_TAG(
					  rtcp,
					  "no Consumer found for received Feedback packet "
					  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
					  feedback->GetSenderSsrc(),
					  feedback->GetMediaSsrc());

					break;
				}

				switch (feedback->GetMessageType())
				{
					case RTC::RTCP::FeedbackRtp::MessageType::NACK:
					{
						if (!consumer)
						{
							MS_DEBUG_TAG(
							  rtcp,
							  "no Consumer found for received NACK Feedback packet "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							  feedback->GetSenderSsrc(),
							  feedback->GetMediaSsrc());

							break;
						}

						auto* nackPacket = static_cast<RTC::RTCP::FeedbackRtpNackPacket*>(packet);

						consumer->ReceiveNack(nackPacket);

						break;
					}

					case RTC::RTCP::FeedbackRtp::MessageType::TCC:
					{
						auto* feedback = static_cast<RTC::RTCP::FeedbackRtpTransportPacket*>(packet);

						if (this->tccClient)
						{
							this->tccClient->ReceiveRtcpTransportFeedback(feedback);
						}

						// yeon: camel
						if (this->camelClient)
						{
							this->camelClient->ReceiveRtcpTransportFeedback(feedback);
						}

						WriteCamelMetricsCsv();

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
						// Pass it to the SenderBandwidthEstimator client.
						if (this->senderBwe)
						{
							this->senderBwe->ReceiveRtcpTransportFeedback(feedback);
						}
#endif

						break;
					}

					default:
					{
						MS_DEBUG_TAG(
						  rtcp,
						  "ignoring unsupported %s Feedback packet "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  RTC::RTCP::FeedbackRtpPacket::MessageType2String(feedback->GetMessageType()).c_str(),
						  feedback->GetSenderSsrc(),
						  feedback->GetMediaSsrc());
					}
				}

				break;
			}

			case RTC::RTCP::Type::SR:
			{
				auto* sr = static_cast<RTC::RTCP::SenderReportPacket*>(packet);

				// Even if Sender Report packet can only contains one report.
				for (auto it = sr->Begin(); it != sr->End(); ++it)
				{
					auto& report   = *it;
					auto* producer = this->rtpListener.GetProducer(report->GetSsrc());

					if (!producer)
					{
						MS_DEBUG_TAG(
						  rtcp,
						  "no Producer found for received Sender Report [ssrc:%" PRIu32 "]",
						  report->GetSsrc());

						continue;
					}

					producer->ReceiveRtcpSenderReport(report);
				}

				break;
			}

			case RTC::RTCP::Type::SDES:
			{
				// According to RFC 3550 section 6.1 "a CNAME item MUST be included in
				// each compound RTCP packet". So this is true even for compound
				// packets sent by endpoints that are not sending any RTP stream to us
				// (thus chunks in such a SDES will have an SSCR does not match with
				// any Producer created in this Transport).
				// Therefore, and given that we do nothing with SDES, just ignore them.

				break;
			}

			case RTC::RTCP::Type::BYE:
			{
				MS_DEBUG_TAG(rtcp, "ignoring received RTCP BYE");

				break;
			}

			case RTC::RTCP::Type::XR:
			{
				auto* xr = static_cast<RTC::RTCP::ExtendedReportPacket*>(packet);

				for (auto it = xr->Begin(); it != xr->End(); ++it)
				{
					auto& report = *it;

					switch (report->GetType())
					{
						case RTC::RTCP::ExtendedReportBlock::Type::DLRR:
						{
							auto* dlrr = static_cast<RTC::RTCP::DelaySinceLastRr*>(report);

							for (auto it2 = dlrr->Begin(); it2 != dlrr->End(); ++it2)
							{
								auto& ssrcInfo = *it2;

								// SSRC should be filled in the sub-block.
								if (ssrcInfo->GetSsrc() == 0)
								{
									ssrcInfo->SetSsrc(xr->GetSsrc());
								}

								auto* producer = this->rtpListener.GetProducer(ssrcInfo->GetSsrc());

								if (!producer)
								{
									MS_DEBUG_TAG(
									  rtcp,
									  "no Producer found for received Sender Extended Report [ssrc:%" PRIu32 "]",
									  ssrcInfo->GetSsrc());

									continue;
								}

								producer->ReceiveRtcpXrDelaySinceLastRr(ssrcInfo);
							}

							break;
						}

						case RTC::RTCP::ExtendedReportBlock::Type::RRT:
						{
							auto* rrt = static_cast<RTC::RTCP::ReceiverReferenceTime*>(report);

							for (auto& kv : this->mapConsumers)
							{
								auto* consumer = kv.second;

								consumer->ReceiveRtcpXrReceiverReferenceTime(rrt);
							}

							break;
						}

						default:;
					}
				}

				break;
			}

			default:
			{
				MS_DEBUG_TAG(
				  rtcp,
				  "unhandled RTCP type received [type:%" PRIu8 "]",
				  static_cast<uint8_t>(packet->GetType()));
			}
		}
	}

	void Transport::SendRtcp(uint64_t nowMs)
	{
		MS_TRACE();

		std::unique_ptr<RTC::RTCP::CompoundPacket> packet{ new RTC::RTCP::CompoundPacket() };

#ifdef MS_LIBURING_SUPPORTED
		if (DepLibUring::IsEnabled())
		{
			// Activate liburing usage.
			DepLibUring::SetActive();
		}
#endif

		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;
			auto rtcpAdded = consumer->GetRtcp(packet.get(), nowMs);

			// RTCP data couldn't be added because the Compound packet is full.
			// Send the RTCP compound packet and request for RTCP again.
			if (!rtcpAdded)
			{
				SendRtcpCompoundPacket(packet.get());

				// Create a new compount packet.
				packet.reset(new RTC::RTCP::CompoundPacket());

				// Retrieve the RTCP again.
				consumer->GetRtcp(packet.get(), nowMs);
			}
		}

		for (auto& kv : this->mapProducers)
		{
			auto* producer = kv.second;
			auto rtcpAdded = producer->GetRtcp(packet.get(), nowMs);

			// RTCP data couldn't be added because the Compound packet is full.
			// Send the RTCP compound packet and request for RTCP again.
			if (!rtcpAdded)
			{
				SendRtcpCompoundPacket(packet.get());

				// Create a new compount packet.
				packet.reset(new RTC::RTCP::CompoundPacket());

				// Retrieve the RTCP again.
				producer->GetRtcp(packet.get(), nowMs);
			}
		}

		// Send the RTCP compound packet if there is any sender or receiver report.
		if (packet->GetReceiverReportCount() > 0u || packet->GetSenderReportCount() > 0u)
		{
			SendRtcpCompoundPacket(packet.get());
		}

#ifdef MS_LIBURING_SUPPORTED
		if (DepLibUring::IsEnabled())
		{
			// Submit all prepared submission entries.
			DepLibUring::Submit();
		}
#endif
	}

	// yeon: camel
	// 여기만 수정하면 bitrate를 원하는 cc로 선택 가능
	uint32_t Transport::GetSelectedAvailableOutgoingBitrate() const
	{
		switch (this->outgoingBweAlgorithm)
		{
			case OutgoingBweAlgorithm::Gcc:
			{
				return this->tccClient ? this->tccClient->GetAvailableBitrate() : 0u;
			}

			case OutgoingBweAlgorithm::Camel:
			{
				if (this->camelClient)
				{
					const uint32_t bitrate = this->camelClient->GetAvailableBitrate();

					if (bitrate > 0u)
					{
						return bitrate;
					}
				}

				// Camel 초기 샘플이 아직 없으면 GCC 사용.
				return this->tccClient ? this->tccClient->GetAvailableBitrate() : 0u;
			}
		}

		return 0u;
	}

	// 요게 일반 버전
	void Transport::DistributeAvailableOutgoingBitrate()
	{
		MS_TRACE();

		MS_ASSERT(this->tccClient, "no TransportCongestionClient");

		std::multimap<uint8_t, RTC::Consumer*> multimapPriorityConsumer;

		// Fill the map with Consumers and their priority (if > 0).
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;
			auto priority  = consumer->GetBitratePriority();

			if (priority > 0u)
			{
				multimapPriorityConsumer.emplace(priority, consumer);
			}
		}

		// Nobody wants bitrate. Exit.
		if (multimapPriorityConsumer.empty())
		{
			return;
		}

		bool baseAllocation = true;
		// yeon: camel
		// uint32_t availableBitrate = this->tccClient->GetAvailableBitrate();
		uint32_t availableBitrate = GetSelectedAvailableOutgoingBitrate();

		// this->tccClient->RescheduleNextAvailableBitrateEvent();
		if (this->tccClient)
		{
			this->tccClient->RescheduleNextAvailableBitrateEvent();
		}

		MS_DEBUG_DEV("before layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

		// Redistribute the available bitrate by allowing Consumers to increase
		// layer by layer. Initially try to spread the bitrate across all
		// consumers. Then allocate the excess bitrate to Consumers starting
		// with the highest priorty.
		while (availableBitrate > 0u)
		{
			auto previousAvailableBitrate = availableBitrate;

			for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
			{
				auto priority  = it->first;
				auto* consumer = it->second;
				auto bweType   = this->tccClient->GetBweType();

				// NOLINTNEXTLINE(bugprone-too-small-loop-variable)
				for (uint8_t i{ 1u }; i <= (baseAllocation ? 1u : priority); ++i)
				{
					uint32_t usedBitrate{ 0u };
					const bool considerLoss = (bweType == RTC::BweType::REMB);

					usedBitrate = consumer->IncreaseLayer(availableBitrate, considerLoss);

					MS_ASSERT(usedBitrate <= availableBitrate, "Consumer used more layer bitrate than given");

					availableBitrate -= usedBitrate;

					// Exit the loop fast if used bitrate is 0.
					if (usedBitrate == 0u)
					{
						break;
					}
				}
			}

			// If no Consumer used bitrate, exit the loop.
			if (availableBitrate == previousAvailableBitrate)
			{
				break;
			}

			baseAllocation = false;
		}

		MS_DEBUG_DEV("after layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

		// Finally instruct Consumers to apply their computed layers.
		for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
		{
			auto* consumer = it->second;

			consumer->ApplyLayers();
		}
	}

	// 요게 2에서 바로 0으로 내리는 버전
	// void Transport::DistributeAvailableOutgoingBitrate()
	// {
	// 	MS_TRACE();

	// 	MS_ASSERT(this->tccClient, "no TransportCongestionClient");

	// 	std::multimap<uint8_t, RTC::Consumer*> multimapPriorityConsumer;

	// 	// Fill the map with Consumers and their priority (if > 0).
	// 	for (auto& kv : this->mapConsumers)
	// 	{
	// 		auto* consumer = kv.second;
	// 		auto priority  = consumer->GetBitratePriority();

	// 		if (priority > 0u)
	// 		{
	// 			multimapPriorityConsumer.emplace(priority, consumer);
	// 		}
	// 	}

	// 	// Nobody wants bitrate. Exit.
	// 	if (multimapPriorityConsumer.empty())
	// 	{
	// 		return;
	// 	}

	// 	// yeon: QoS layer policy.
	// 	// 기존 bitrate allocation이 돌기 전의 target/current spatial layer를 저장한다.
	// 	// audio/simple consumer는 GetQosTargetSpatialLayer()가 -1을 반환하므로 자동 제외된다.
	// 	std::unordered_map<RTC::Consumer*, int16_t> previousSpatialLayerByConsumer;

	// 	for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 	{
	// 		auto* consumer = it->second;

	// 		if (!consumer)
	// 		{
	// 			continue;
	// 		}

	// 		int16_t previousSpatialLayer = consumer->GetQosTargetSpatialLayer();

	// 		if (previousSpatialLayer < 0)
	// 		{
	// 			previousSpatialLayer = consumer->GetQosCurrentSpatialLayer();
	// 		}

	// 		if (previousSpatialLayer >= 0)
	// 		{
	// 			previousSpatialLayerByConsumer[consumer] = previousSpatialLayer;
	// 		}
	// 	}

	// 	bool baseAllocation = true;

	// 	// yeon: camel
	// 	// uint32_t availableBitrate = this->tccClient->GetAvailableBitrate();
	// 	uint32_t availableBitrate = GetSelectedAvailableOutgoingBitrate();

	// 	// this->tccClient->RescheduleNextAvailableBitrateEvent();
	// 	if (this->tccClient)
	// 	{
	// 		this->tccClient->RescheduleNextAvailableBitrateEvent();
	// 	}

	// 	MS_DEBUG_DEV("before layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

	// 	// Redistribute the available bitrate by allowing Consumers to increase
	// 	// layer by layer. Initially try to spread the bitrate across all
	// 	// consumers. Then allocate the excess bitrate to Consumers starting
	// 	// with the highest priority.
	// 	while (availableBitrate > 0u)
	// 	{
	// 		auto previousAvailableBitrate = availableBitrate;

	// 		for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 		{
	// 			auto priority  = it->first;
	// 			auto* consumer = it->second;
	// 			auto bweType   = this->tccClient->GetBweType();

	// 			// NOLINTNEXTLINE(bugprone-too-small-loop-variable)
	// 			for (uint8_t i{ 1u }; i <= (baseAllocation ? 1u : priority); ++i)
	// 			{
	// 				uint32_t usedBitrate{ 0u };
	// 				const bool considerLoss = (bweType == RTC::BweType::REMB);

	// 				usedBitrate = consumer->IncreaseLayer(availableBitrate, considerLoss);

	// 				MS_ASSERT(usedBitrate <= availableBitrate, "Consumer used more layer bitrate than given");

	// 				availableBitrate -= usedBitrate;

	// 				// Exit the loop fast if used bitrate is 0.
	// 				if (usedBitrate == 0u)
	// 				{
	// 					break;
	// 				}
	// 			}
	// 		}

	// 		// If no Consumer used bitrate, exit the loop.
	// 		if (availableBitrate == previousAvailableBitrate)
	// 		{
	// 			break;
	// 		}

	// 		baseAllocation = false;
	// 	}

	// 	MS_DEBUG_DEV("after layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

	// 	// yeon: QoS layer policy override.
	// 	//
	// 	// 기존 allocator가 계산한 provisional target을 보고,
	// 	// 1) 이전 layer가 2였고 이번 allocator 결과가 1이면, 1이 아니라 0으로 바로 내림.
	// 	// 2) 이전 layer가 0이면, 이번 allocator 결과가 1/2여도 0 유지.
	// 	//
	// 	// 여기서는 ApplyLayers()를 호출하지 않는다.
	// 	// 기존 ApplyLayers() 호출 직전에 provisionalTargetLayers만 수정한다.
	// 	for (auto& kv : previousSpatialLayerByConsumer)
	// 	{
	// 		auto* consumer                     = kv.first;
	// 		const int16_t previousSpatialLayer = kv.second;

	// 		if (!consumer)
	// 		{
	// 			continue;
	// 		}

	// 		const int16_t normalProvisionalSpatialLayer = consumer->GetQosProvisionalTargetSpatialLayer();

	// 		if (normalProvisionalSpatialLayer < 0)
	// 		{
	// 			continue;
	// 		}

	// 		int16_t forcedSpatialLayer{ -1 };

	// 		// 이전에 이미 spatial 0이었다면 계속 0 유지.
	// 		if (previousSpatialLayer == 0 && normalProvisionalSpatialLayer > 0)
	// 		{
	// 			forcedSpatialLayer = 0;
	// 		}
	// 		// 기존 allocator가 2 -> 1로 내리려고 하면 2 -> 0으로 강제.
	// 		else if (previousSpatialLayer == 2 && normalProvisionalSpatialLayer == 1)
	// 		{
	// 			forcedSpatialLayer = 0;
	// 		}

	// 		if (forcedSpatialLayer >= 0)
	// 		{
	// 			MS_WARN_TAG(
	// 			  bwe,
	// 			  "QoS layer policy overrides provisional spatial layer "
	// 			  "[previous:%" PRIi16 ", normal:%" PRIi16 ", forced:%" PRIi16 "]",
	// 			  previousSpatialLayer,
	// 			  normalProvisionalSpatialLayer,
	// 			  forcedSpatialLayer);

	// 			consumer->ForceQosProvisionalSpatialLayer(forcedSpatialLayer);
	// 		}
	// 	}

	// 	// Finally instruct Consumers to apply their computed layers.
	// 	// 기존 루프 그대로 유지.
	// 	for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 	{
	// 		auto* consumer = it->second;

	// 		consumer->ApplyLayers();
	// 	}
	// }

	// void Transport::DistributeAvailableOutgoingBitrate()
	// {
	// 	MS_TRACE();

	// 	MS_ASSERT(this->tccClient, "no TransportCongestionClient");

	// 	// yeon: 1000 frames at 30fps ~= 33.33s.
	// 	static constexpr int64_t QosLayerZeroHoldMs{ 33333 };

	// 	const int64_t nowMs = DepLibUV::GetTimeMsInt64();

	// 	std::multimap<uint8_t, RTC::Consumer*> multimapPriorityConsumer;

	// 	// Fill the map with Consumers and their priority (if > 0).
	// 	for (auto& kv : this->mapConsumers)
	// 	{
	// 		auto* consumer = kv.second;
	// 		auto priority  = consumer->GetBitratePriority();

	// 		if (priority > 0u)
	// 		{
	// 			multimapPriorityConsumer.emplace(priority, consumer);
	// 		}
	// 	}

	// 	// Nobody wants bitrate. Exit.
	// 	if (multimapPriorityConsumer.empty())
	// 	{
	// 		return;
	// 	}

	// 	// yeon: QoS layer policy.
	// 	//
	// 	// 기존 bitrate allocation이 돌기 전에,
	// 	// 이전에 결정되어 있던 target/current spatial layer를 저장한다.
	// 	//
	// 	// audio/simple consumer는 GetQosTargetSpatialLayer() / GetQosCurrentSpatialLayer()
	// 	// 가 -1을 반환하므로 자동으로 제외된다.
	// 	std::unordered_map<RTC::Consumer*, int16_t> previousSpatialLayerByConsumer;

	// 	for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 	{
	// 		auto* consumer = it->second;

	// 		if (!consumer)
	// 		{
	// 			continue;
	// 		}

	// 		int16_t previousSpatialLayer = consumer->GetQosTargetSpatialLayer();

	// 		if (previousSpatialLayer < 0)
	// 		{
	// 			previousSpatialLayer = consumer->GetQosCurrentSpatialLayer();
	// 		}

	// 		if (previousSpatialLayer >= 0)
	// 		{
	// 			previousSpatialLayerByConsumer[consumer] = previousSpatialLayer;
	// 		}
	// 	}

	// 	bool baseAllocation = true;

	// 	// yeon: camel
	// 	// uint32_t availableBitrate = this->tccClient->GetAvailableBitrate();
	// 	uint32_t availableBitrate = GetSelectedAvailableOutgoingBitrate();

	// 	// this->tccClient->RescheduleNextAvailableBitrateEvent();
	// 	if (this->tccClient)
	// 	{
	// 		this->tccClient->RescheduleNextAvailableBitrateEvent();
	// 	}

	// 	MS_DEBUG_DEV("before layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

	// 	// Redistribute the available bitrate by allowing Consumers to increase
	// 	// layer by layer. Initially try to spread the bitrate across all
	// 	// consumers. Then allocate the excess bitrate to Consumers starting
	// 	// with the highest priority.
	// 	while (availableBitrate > 0u)
	// 	{
	// 		auto previousAvailableBitrate = availableBitrate;

	// 		for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 		{
	// 			auto priority  = it->first;
	// 			auto* consumer = it->second;
	// 			auto bweType   = this->tccClient->GetBweType();

	// 			// NOLINTNEXTLINE(bugprone-too-small-loop-variable)
	// 			for (uint8_t i{ 1u }; i <= (baseAllocation ? 1u : priority); ++i)
	// 			{
	// 				uint32_t usedBitrate{ 0u };
	// 				const bool considerLoss = (bweType == RTC::BweType::REMB);

	// 				usedBitrate = consumer->IncreaseLayer(availableBitrate, considerLoss);

	// 				MS_ASSERT(usedBitrate <= availableBitrate, "Consumer used more layer bitrate than given");

	// 				availableBitrate -= usedBitrate;

	// 				// Exit the loop fast if used bitrate is 0.
	// 				if (usedBitrate == 0u)
	// 				{
	// 					break;
	// 				}
	// 			}
	// 		}

	// 		// If no Consumer used bitrate, exit the loop.
	// 		if (availableBitrate == previousAvailableBitrate)
	// 		{
	// 			break;
	// 		}

	// 		baseAllocation = false;
	// 	}

	// 	MS_DEBUG_DEV("after layer-by-layer iterations [availableBitrate:%" PRIu32 "]", availableBitrate);

	// 	// yeon: QoS layer policy override.
	// 	//
	// 	// 요구한 정책:
	// 	//
	// 	// 1. 기존 allocator가 2 -> 1로 내리려고 하면,
	// 	//    1이 아니라 0으로 바로 내린다.
	// 	//
	// 	// 2. 그 순간부터 33초 동안 layer 0을 유지한다.
	// 	//    33초는 30fps 기준 약 1000 frames.
	// 	//
	// 	// 3. 33초가 지나면 allocator 결과와 상관없이 layer 1로 강제 상승한다.
	// 	//
	// 	// 4. 여기서는 ApplyLayers()를 호출하지 않는다.
	// 	//    기존 ApplyLayers() 호출 직전에 provisionalTargetLayers만 수정한다.
	// 	for (auto& kv : previousSpatialLayerByConsumer)
	// 	{
	// 		auto* consumer                     = kv.first;
	// 		const int16_t previousSpatialLayer = kv.second;

	// 		if (!consumer)
	// 		{
	// 			continue;
	// 		}

	// 		const int16_t normalProvisionalSpatialLayer = consumer->GetQosProvisionalTargetSpatialLayer();

	// 		if (normalProvisionalSpatialLayer < 0)
	// 		{
	// 			continue;
	// 		}

	// 		const bool holdActive     = consumer->IsQosLayerZeroHoldActive();
	// 		const int64_t holdStartMs = consumer->GetQosLayerZeroHoldStartMs();

	// 		const int64_t holdElapsedMs = holdActive && holdStartMs > 0 ? nowMs - holdStartMs : 0;

	// 		const bool holdExpired = holdActive && holdElapsedMs >= QosLayerZeroHoldMs;

	// 		int16_t forcedSpatialLayer{ -1 };

	// 		// Case 1:
	// 		// 기존 allocator가 2 -> 1로 내리려고 하면,
	// 		// 1이 아니라 0으로 강제하고 hold 시작.
	// 		if (!holdActive && previousSpatialLayer == 2 && normalProvisionalSpatialLayer == 1)
	// 		{
	// 			forcedSpatialLayer = 0;
	// 			consumer->StartQosLayerZeroHold(nowMs);
	// 		}
	// 		// Case 2:
	// 		// hold 중이고 아직 33초가 안 지났으면 무조건 0 유지.
	// 		else if (holdActive && !holdExpired)
	// 		{
	// 			forcedSpatialLayer = 0;
	// 		}
	// 		// Case 3:
	// 		// hold 시작 후 33초가 지났으면 allocator 결과와 상관없이 1로 강제 상승.
	// 		else if (holdActive && holdExpired)
	// 		{
	// 			forcedSpatialLayer = 1;
	// 			// consumer->StopQosLayerZeroHold();
	// 		}

	// 		if (forcedSpatialLayer >= 0)
	// 		{
	// 			// MS_WARN_TAG(
	// 			//   bwe,
	// 			//   "QoS layer policy overrides provisional spatial layer "
	// 			//   "[previous:%" PRIi16 ", normal:%" PRIi16 ", forced:%" PRIi16
	// 			//   ", holdActive:%d, holdExpired:%d, holdElapsedMs:%" PRIi64 "/%" PRIi64 "]",
	// 			//   previousSpatialLayer,
	// 			//   normalProvisionalSpatialLayer,
	// 			//   forcedSpatialLayer,
	// 			//   static_cast<int>(holdActive),
	// 			//   static_cast<int>(holdExpired),
	// 			//   holdElapsedMs,
	// 			//   QosLayerZeroHoldMs);

	// 			consumer->ForceQosProvisionalSpatialLayer(forcedSpatialLayer);
	// 		}
	// 	}

	// 	// Finally instruct Consumers to apply their computed layers.
	// 	for (auto it = multimapPriorityConsumer.rbegin(); it != multimapPriorityConsumer.rend(); ++it)
	// 	{
	// 		auto* consumer = it->second;

	// 		consumer->ApplyLayers();
	// 	}
	// }

	void Transport::ComputeOutgoingDesiredBitrate(bool forceBitrate)
	{
		MS_TRACE();

		MS_ASSERT(this->tccClient, "no TransportCongestionClient");

		uint32_t totalDesiredBitrate{ 0u };

		for (auto& kv : this->mapConsumers)
		{
			auto* consumer      = kv.second;
			auto desiredBitrate = consumer->GetDesiredBitrate();

			totalDesiredBitrate += desiredBitrate;
		}

		MS_DEBUG_DEV("total desired bitrate: %" PRIu32, totalDesiredBitrate);

		// yeon: camel
		// this->tccClient->SetDesiredBitrate(totalDesiredBitrate, forceBitrate);

		if (this->tccClient)
		{
			this->tccClient->SetDesiredBitrate(totalDesiredBitrate, forceBitrate);
		}

		if (this->camelClient)
		{
			this->camelClient->SetDesiredBitrate(totalDesiredBitrate, forceBitrate);
		}
	}

	inline void Transport::EmitTraceEventProbationType(RTC::RtpPacket* /*packet*/) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.probation)
		{
			return;
		}

		// TODO: Missing trace info (RTP packet dump).
		auto notification = FBS::Transport::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Transport::TraceEventType::PROBATION,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_OUT);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_TRACE,
		  FBS::Notification::Body::Transport_TraceNotification,
		  notification);
	}

	inline void Transport::EmitTraceEventBweType(
	  RTC::TransportCongestionControlClient::Bitrates& bitrates) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.bwe)
		{
			return;
		}

		auto traceInfo = FBS::Transport::CreateBweTraceInfo(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  this->tccClient->GetBweType() == RTC::BweType::TRANSPORT_CC
		    ? FBS::Transport::BweType::TRANSPORT_CC
		    : FBS::Transport::BweType::REMB,
		  bitrates.desiredBitrate,
		  bitrates.effectiveDesiredBitrate,
		  bitrates.minBitrate,
		  bitrates.maxBitrate,
		  bitrates.startBitrate,
		  bitrates.maxPaddingBitrate,
		  bitrates.availableBitrate);

		auto notification = FBS::Transport::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Transport::TraceEventType::BWE,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_OUT,
		  FBS::Transport::TraceInfo::BweTraceInfo,
		  traceInfo.Union());

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_TRACE,
		  FBS::Notification::Body::Transport_TraceNotification,
		  notification);
	}

	inline void Transport::OnProducerPaused(RTC::Producer* producer)
	{
		MS_TRACE();

		this->listener->OnTransportProducerPaused(this, producer);
	}

	inline void Transport::OnProducerResumed(RTC::Producer* producer)
	{
		MS_TRACE();

		this->listener->OnTransportProducerResumed(this, producer);
	}

	inline void Transport::OnProducerNewRtpStream(
	  RTC::Producer* producer, RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc)
	{
		MS_TRACE();

		this->listener->OnTransportProducerNewRtpStream(this, producer, rtpStream, mappedSsrc);
	}

	inline void Transport::OnProducerRtpStreamScore(
	  RTC::Producer* producer, RTC::RtpStreamRecv* rtpStream, uint8_t score, uint8_t previousScore)
	{
		MS_TRACE();

		this->listener->OnTransportProducerRtpStreamScore(this, producer, rtpStream, score, previousScore);
	}

	inline void Transport::OnProducerRtcpSenderReport(
	  RTC::Producer* producer, RTC::RtpStreamRecv* rtpStream, bool first)
	{
		MS_TRACE();

		this->listener->OnTransportProducerRtcpSenderReport(this, producer, rtpStream, first);
	}

	inline void Transport::OnProducerRtpPacketReceived(RTC::Producer* producer, RTC::RtpPacket* packet)
	{
		MS_TRACE();
		// MS_ERROR_STD();
		this->listener->OnTransportProducerRtpPacketReceived(this, producer, packet);
	}

	inline void Transport::OnProducerSendRtcpPacket(RTC::Producer* /*producer*/, RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		SendRtcpPacket(packet);
	}

	inline void Transport::OnProducerNeedWorstRemoteFractionLost(
	  RTC::Producer* producer, uint32_t mappedSsrc, uint8_t& worstRemoteFractionLost)
	{
		MS_TRACE();

		this->listener->OnTransportNeedWorstRemoteFractionLost(
		  this, producer, mappedSsrc, worstRemoteFractionLost);
	}

	inline void Transport::OnConsumerSendRtpPacket(RTC::Consumer* consumer, RTC::RtpPacket* packet)
	{
		MS_TRACE();
		// MS_ERROR_STD();

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.sendTransportId = this->id;
		packet->logger.Sent();
#endif

		// Update abs-send-time if present.
		packet->UpdateAbsSendTime(DepLibUV::GetTimeMs());

		// Update transport wide sequence number if present.
		// clang-format off
		if (
			this->tccClient &&
			this->tccClient->GetBweType() == RTC::BweType::TRANSPORT_CC &&
			packet->UpdateTransportWideCc01(this->transportWideCcSeq + 1)
		)
		// clang-format on
		{
			this->transportWideCcSeq++;

			webrtc::RtpPacketSendInfo packetInfo;

			packetInfo.ssrc                      = packet->GetSsrc();
			packetInfo.transport_sequence_number = this->transportWideCcSeq;
			packetInfo.has_rtp_sequence_number   = true;
			packetInfo.rtp_sequence_number       = packet->GetSequenceNumber();
			packetInfo.length                    = packet->GetSize();
			packetInfo.pacing_info               = this->tccClient->GetPacingInfo();

			// Indicate the pacer (and prober) that a packet is to be sent.
			this->tccClient->InsertPacket(packetInfo);

			if (this->camelClient)
			{
				this->camelClient->InsertPacket(packetInfo, packet, consumer, DepLibUV::GetTimeMsInt64());
			}

			// 2. 여기서 최종 상태의 원본 RTP를 FEC encoder에 입력.
			// 매 패킷마다 호출하지만, 매번 FEC가 생성되는 것은 아님.
			// std::vector<std::unique_ptr<RTC::RtpPacket>> fecPackets;

			// if (consumer->IsFecEnabled())
			// {
			// 	consumer->AddPacketToFecEncoder(packet);
			// 	fecPackets = consumer->TakeGeneratedFecPackets();
			// }

			// When using WebRtcServer, the lifecycle of a RTC::UdpSocket maybe longer
			// than WebRtcTransport so there is a chance for the send callback to be
			// invoked *after* the WebRtcTransport has been closed (freed). To avoid
			// invalid memory access we need to use weak_ptr. Same applies in other
			// send callbacks.
			const std::weak_ptr<RTC::TransportCongestionControlClient> tccClientWeakPtr(this->tccClient);
			const std::weak_ptr<RTC::CamelCongestionControlClient> camelClientWeakPtr(this->camelClient);

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
			std::weak_ptr<RTC::SenderBandwidthEstimator> senderBweWeakPtr(this->senderBwe);
			RTC::SenderBandwidthEstimator::SentInfo sentInfo;

			sentInfo.wideSeq     = this->transportWideCcSeq;
			sentInfo.size        = packet->GetSize();
			sentInfo.sendingAtMs = DepLibUV::GetTimeMs();

			auto* cb = new onSendCallback(
			  [tccClientWeakPtr, packetInfo, senderBweWeakPtr, sentInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto senderBwe = senderBweWeakPtr.lock();

					  if (senderBwe)
					  {
						  sentInfo.sentAtMs = DepLibUV::GetTimeMs();
						  senderBwe->RtpPacketSent(sentInfo);
					  }
				  }
			  });

			SendRtpPacket(consumer, packet, cb);
#else
			const auto* cb = new onSendCallback(
			  //[tccClientWeakPtr, packetInfo](bool sent)
			  // yeon: camel
			  [tccClientWeakPtr, camelClientWeakPtr, packetInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto camelClient = camelClientWeakPtr.lock();

					  if (camelClient)
					  {
						  camelClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }
				  }
			  });

			SendRtpPacket(consumer, packet, cb);
#endif
		}
		else
		{
			SendRtpPacket(consumer, packet);
		}

		this->sendRtpTransmission.Update(packet);

		// 4. 보호 그룹이 완성된 경우에만 FEC RTP 전송.
		// for (auto& fecPacket : fecPackets)
		// {
		// 	OnConsumerSendFecPacket(consumer, fecPacket.get());
		// }
	}

	inline void Transport::OnConsumerRetransmitRtpPacket(RTC::Consumer* consumer, RTC::RtpPacket* packet)
	{
		MS_TRACE();

		// Update abs-send-time if present.
		packet->UpdateAbsSendTime(DepLibUV::GetTimeMs());

		// Update transport wide sequence number if present.
		// clang-format off
		if (
			this->tccClient &&
			this->tccClient->GetBweType() == RTC::BweType::TRANSPORT_CC &&
			packet->UpdateTransportWideCc01(this->transportWideCcSeq + 1)
		)
		// clang-format on
		{
			// MS_ERROR_STD("no2");
			this->transportWideCcSeq++;

			webrtc::RtpPacketSendInfo packetInfo;

			packetInfo.ssrc                      = packet->GetSsrc();
			packetInfo.transport_sequence_number = this->transportWideCcSeq;
			packetInfo.has_rtp_sequence_number   = true;
			packetInfo.rtp_sequence_number       = packet->GetSequenceNumber();
			packetInfo.length                    = packet->GetSize();
			packetInfo.pacing_info               = this->tccClient->GetPacingInfo();

			// Indicate the pacer (and prober) that a packet is to be sent.
			this->tccClient->InsertPacket(packetInfo);

			const std::weak_ptr<RTC::TransportCongestionControlClient> tccClientWeakPtr(this->tccClient);
			const std::weak_ptr<RTC::CamelCongestionControlClient> camelClientWeakPtr(this->camelClient);

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
			std::weak_ptr<RTC::SenderBandwidthEstimator> senderBweWeakPtr = this->senderBwe;
			RTC::SenderBandwidthEstimator::SentInfo sentInfo;

			sentInfo.wideSeq     = this->transportWideCcSeq;
			sentInfo.size        = packet->GetSize();
			sentInfo.sendingAtMs = DepLibUV::GetTimeMs();

			auto* cb = new onSendCallback(
			  [tccClientWeakPtr, packetInfo, senderBweWeakPtr, sentInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto senderBwe = senderBweWeakPtr.lock();

					  if (senderBwe)
					  {
						  sentInfo.sentAtMs = DepLibUV::GetTimeMs();
						  senderBwe->RtpPacketSent(sentInfo);
					  }
				  }
			  });

			SendRtpPacket(consumer, packet, cb);
#else
			const auto* cb = new onSendCallback(
			  [tccClientWeakPtr, camelClientWeakPtr, packetInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto camelClient = camelClientWeakPtr.lock();

					  if (camelClient)
					  {
						  camelClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }
				  }
			  });

			SendRtpPacket(consumer, packet, cb);
#endif
		}
		else
		{
			SendRtpPacket(consumer, packet);
		}

		this->sendRtxTransmission.Update(packet);
	}

	inline void Transport::OnConsumerKeyFrameRequested(RTC::Consumer* consumer, uint32_t mappedSsrc)
	{
		MS_TRACE();

		if (!IsConnected())
		{
			MS_WARN_TAG(rtcp, "ignoring key rame request (transport not connected)");

			return;
		}

		this->listener->OnTransportConsumerKeyFrameRequested(this, consumer, mappedSsrc);
	}

	inline void Transport::OnConsumerNeedBitrateChange(RTC::Consumer* /*consumer*/)
	{
		MS_TRACE();

		MS_ASSERT(this->tccClient, "no TransportCongestionClient");

		DistributeAvailableOutgoingBitrate();
		ComputeOutgoingDesiredBitrate();
	}

	inline void Transport::OnConsumerNeedZeroBitrate(RTC::Consumer* /*consumer*/)
	{
		MS_TRACE();

		MS_ASSERT(this->tccClient, "no TransportCongestionClient");

		DistributeAvailableOutgoingBitrate();

		// This may be the latest active Consumer with BWE. If so we have to stop probation.
		ComputeOutgoingDesiredBitrate(/*forceBitrate*/ true);
	}

	inline void Transport::OnConsumerProducerClosed(RTC::Consumer* consumer)
	{
		MS_TRACE();

		// Remove it from the maps.
		this->mapConsumers.erase(consumer->id);

		for (auto ssrc : consumer->GetMediaSsrcs())
		{
			this->mapSsrcConsumer.erase(ssrc);

			// Tell the child class to clear associated SSRCs.
			SendStreamClosed(ssrc);
		}

		for (auto ssrc : consumer->GetRtxSsrcs())
		{
			this->mapRtxSsrcConsumer.erase(ssrc);

			// Tell the child class to clear associated SSRCs.
			SendStreamClosed(ssrc);
		}

		// Notify the listener.
		this->listener->OnTransportConsumerProducerClosed(this, consumer);

		// Delete it.
		delete consumer;

		// This may be the latest active Consumer with BWE. If so we have to stop probation.
		if (this->tccClient)
		{
			ComputeOutgoingDesiredBitrate(/*forceBitrate*/ true);
		}
	}

	inline void Transport::OnDataProducerMessageReceived(
	  RTC::DataProducer* dataProducer,
	  const uint8_t* msg,
	  size_t len,
	  uint32_t ppid,
	  std::vector<uint16_t>& subchannels,
	  std::optional<uint16_t> requiredSubchannel)
	{
		MS_TRACE();

		this->listener->OnTransportDataProducerMessageReceived(
		  this, dataProducer, msg, len, ppid, subchannels, requiredSubchannel);
	}

	inline void Transport::OnDataProducerPaused(RTC::DataProducer* dataProducer)
	{
		MS_TRACE();

		this->listener->OnTransportDataProducerPaused(this, dataProducer);
	}

	inline void Transport::OnDataProducerResumed(RTC::DataProducer* dataProducer)
	{
		MS_TRACE();

		this->listener->OnTransportDataProducerResumed(this, dataProducer);
	}

	inline void Transport::OnDataConsumerSendMessage(
	  RTC::DataConsumer* dataConsumer, const uint8_t* msg, size_t len, uint32_t ppid, onQueuedCallback* cb)
	{
		MS_TRACE();

		SendMessage(dataConsumer, msg, len, ppid, cb);
	}

	inline void Transport::OnDataConsumerDataProducerClosed(RTC::DataConsumer* dataConsumer)
	{
		MS_TRACE();

		// Remove it from the maps.
		this->mapDataConsumers.erase(dataConsumer->id);

		// Notify the listener.
		this->listener->OnTransportDataConsumerDataProducerClosed(this, dataConsumer);

		if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
		{
			// Tell the SctpAssociation so it can reset the SCTP stream.
			this->sctpAssociation->DataConsumerClosed(dataConsumer);
		}

		// Delete it.
		delete dataConsumer;
	}

	inline void Transport::OnSctpAssociationConnecting(RTC::SctpAssociation* /*sctpAssociation*/)
	{
		MS_TRACE();

		// Notify the Node Transport.
		auto sctpStateChangeOffset = FBS::Transport::CreateSctpStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::SctpAssociation::SctpState::CONNECTING);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_SCTP_STATE_CHANGE,
		  FBS::Notification::Body::Transport_SctpStateChangeNotification,
		  sctpStateChangeOffset);
	}

	inline void Transport::OnSctpAssociationConnected(RTC::SctpAssociation* /*sctpAssociation*/)
	{
		MS_TRACE();

		// Tell all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
			{
				dataConsumer->SctpAssociationConnected();
			}
		}

		// Notify the Node Transport.
		auto sctpStateChangeOffset = FBS::Transport::CreateSctpStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::SctpAssociation::SctpState::CONNECTED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_SCTP_STATE_CHANGE,
		  FBS::Notification::Body::Transport_SctpStateChangeNotification,
		  sctpStateChangeOffset);
	}

	inline void Transport::OnSctpAssociationFailed(RTC::SctpAssociation* /*sctpAssociation*/)
	{
		MS_TRACE();

		// Tell all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
			{
				dataConsumer->SctpAssociationClosed();
			}
		}

		// Notify the Node Transport.
		auto sctpStateChangeOffset = FBS::Transport::CreateSctpStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::SctpAssociation::SctpState::FAILED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_SCTP_STATE_CHANGE,
		  FBS::Notification::Body::Transport_SctpStateChangeNotification,
		  sctpStateChangeOffset);
	}

	inline void Transport::OnSctpAssociationClosed(RTC::SctpAssociation* /*sctpAssociation*/)
	{
		MS_TRACE();

		// Tell all DataConsumers.
		for (auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
			{
				dataConsumer->SctpAssociationClosed();
			}
		}

		// Notify the Node Transport.
		auto sctpStateChangeOffset = FBS::Transport::CreateSctpStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::SctpAssociation::SctpState::CLOSED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::TRANSPORT_SCTP_STATE_CHANGE,
		  FBS::Notification::Body::Transport_SctpStateChangeNotification,
		  sctpStateChangeOffset);
	}

	inline void Transport::OnSctpAssociationSendData(
	  RTC::SctpAssociation* /*sctpAssociation*/, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("OnSctpAssociationSendData");
		//  Ignore if destroying.
		//  NOTE: This is because when the child class (i.e. WebRtcTransport) is deleted,
		//  its destructor is called first and then the parent Transport's destructor,
		//  and we would end here calling SendSctpData() which is an abstract method.
		if (this->destroying)
		{
			return;
		}

		if (this->sctpAssociation)
		{
			SendSctpData(data, len);
		}
	}

	inline void Transport::OnSctpAssociationMessageReceived(
	  RTC::SctpAssociation* /*sctpAssociation*/,
	  uint16_t streamId,
	  const uint8_t* msg,
	  size_t len,
	  uint32_t ppid)
	{
		MS_TRACE();
		// MS_ERROR_STD("OnSctpAssociationMessageReceived");
		RTC::DataProducer* dataProducer = this->sctpListener.GetDataProducer(streamId);

		// 1. 구/신버전 정상 DataProducer 경로.
		// if (LooksLikeFrameTelemetryPayload(msg, len, ppid))
		// {
		// 	// MS_WARN_TAG(
		// 	//   sctp,
		// 	//   "[TELEMETRY] frame telemetry payload captured [streamId:%" PRIu16 ", ppid:%" PRIu32
		// 	//   ", len:%zu, hasDataProducer:%d]",
		// 	//   streamId,
		// 	//   ppid,
		// 	//   len,
		// 	//   dataProducer ? 1 : 0);

		// 	OnSlack(msg, len);

		// 	// telemetry를 Node.js app 쪽 DataProducer event로도 넘길 필요가 없다면 return.
		// 	// CSV/수집만 목적이면 return이 맞습니다.
		// 	return;
		// }

		// 2. 구버전 raw/unregistered SCTP 경로.
		if (!dataProducer)
		{
			OnSlack(msg, len);
			// MS_WARN_TAG(
			//   sctp, "no suitable DataProducer for received SCTP message [streamId:%" PRIu16 "]", streamId);

			return;
		}

		// Pass the SCTP message to the corresponding DataProducer.
		try
		{
			static std::vector<uint16_t> emptySubchannels;

			dataProducer->ReceiveMessage(
			  msg, len, ppid, emptySubchannels, /*requiredSubchannel*/ std::nullopt);
		}
		catch (std::exception& error)
		{
			MS_WARN_TAG(
			  sctp,
			  "DataProducer::ReceiveMessage() failed for received SCTP message [streamId:%" PRIu16 "]: %s",
			  streamId,
			  error.what());
		}
	}

	inline void Transport::OnSctpAssociationBufferedAmount(
	  RTC::SctpAssociation* /*sctpAssociation*/, uint32_t bufferedAmount)
	{
		MS_TRACE();

		for (const auto& kv : this->mapDataConsumers)
		{
			auto* dataConsumer = kv.second;

			if (dataConsumer->GetType() == RTC::DataConsumer::Type::SCTP)
			{
				dataConsumer->SctpAssociationBufferedAmount(bufferedAmount);
			}
		}
	}

	void Transport::ApplySelectedOutgoingBitrate(
	  RTC::TransportCongestionControlClient::Bitrates& bitrates)
	{
		DistributeAvailableOutgoingBitrate();
		ComputeOutgoingDesiredBitrate();

		// GCC Bitrates contains all fields required by the existing BWE trace event.
		EmitTraceEventBweType(bitrates);

		OnAvailableBitrateChanged(GetSelectedAvailableOutgoingBitrate());
	}

	void Transport::ApplySelectedOutgoingBitrate(
	  RTC::CamelCongestionControlClient::Bitrates& /*bitrates*/)
	{
		DistributeAvailableOutgoingBitrate();
		ComputeOutgoingDesiredBitrate();

		// EmitTraceEventBweType() currently accepts GCC Bitrates only.
		OnAvailableBitrateChanged(GetSelectedAvailableOutgoingBitrate());
	}

	inline void Transport::OnTransportCongestionControlClientBitrates(
	  RTC::TransportCongestionControlClient* /*tccClient*/,
	  RTC::TransportCongestionControlClient::Bitrates& bitrates)
	{
		MS_TRACE();
		MS_DEBUG_DEV("outgoing available bitrate:%" PRIu32, bitrates.availableBitrate);

		if (this->outgoingBweAlgorithm == OutgoingBweAlgorithm::Gcc)
		{
			ApplySelectedOutgoingBitrate(bitrates);
		}
	}

	// yeon:
	inline void Transport::OnCamelCongestionControlClientBitrates(
	  RTC::CamelCongestionControlClient* /*camelClient*/,
	  RTC::CamelCongestionControlClient::Bitrates& bitrates)
	{
		MS_TRACE();
		MS_DEBUG_DEV("camel outgoing available bitrate:%" PRIu32, bitrates.availableBitrate);

		if (this->outgoingBweAlgorithm == OutgoingBweAlgorithm::Camel)
		{
			ApplySelectedOutgoingBitrate(bitrates);
		}
	}

	inline void Transport::OnPacketLossCheck(double Loss)
	{
		MS_TRACE();
		OnPacketLossDetected(Loss);
	}

	inline void Transport::OnTransportCongestionControlClientSendRtpPacket(
	  RTC::TransportCongestionControlClient* /*tccClient*/,
	  RTC::RtpPacket* packet,
	  const webrtc::PacedPacketInfo& pacingInfo)
	{
		MS_TRACE();
		// MS_ERROR_STD();
		//  Update abs-send-time if present.
		packet->UpdateAbsSendTime(DepLibUV::GetTimeMs());

		// Update transport wide sequence number if present.
		// clang-format off
		if (
			this->tccClient->GetBweType() == RTC::BweType::TRANSPORT_CC &&
			packet->UpdateTransportWideCc01(this->transportWideCcSeq + 1)
		)
		// clang-format on
		{
			// MS_ERROR_STD("no3");
			this->transportWideCcSeq++;

			// May emit 'trace' event.
			EmitTraceEventProbationType(packet);

			webrtc::RtpPacketSendInfo packetInfo;

			packetInfo.ssrc                      = packet->GetSsrc();
			packetInfo.transport_sequence_number = this->transportWideCcSeq;
			packetInfo.has_rtp_sequence_number   = true;
			packetInfo.rtp_sequence_number       = packet->GetSequenceNumber();
			packetInfo.length                    = packet->GetSize();
			packetInfo.pacing_info               = pacingInfo;

			// Indicate the pacer (and prober) that a packet is to be sent.
			this->tccClient->InsertPacket(packetInfo);

			const std::weak_ptr<RTC::TransportCongestionControlClient> tccClientWeakPtr(this->tccClient);
			const std::weak_ptr<RTC::CamelCongestionControlClient> camelClientWeakPtr(this->camelClient);

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
			std::weak_ptr<RTC::SenderBandwidthEstimator> senderBweWeakPtr = this->senderBwe;
			RTC::SenderBandwidthEstimator::SentInfo sentInfo;

			sentInfo.wideSeq     = this->transportWideCcSeq;
			sentInfo.size        = packet->GetSize();
			sentInfo.isProbation = true;
			sentInfo.sendingAtMs = DepLibUV::GetTimeMs();

			auto* cb = new onSendCallback(
			  [tccClientWeakPtr, packetInfo, senderBweWeakPtr, sentInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto senderBwe = senderBweWeakPtr.lock();

					  if (senderBwe)
					  {
						  sentInfo.sentAtMs = DepLibUV::GetTimeMs();
						  senderBwe->RtpPacketSent(sentInfo);
					  }
				  }
			  });

			SendRtpPacket(nullptr, packet, cb);
#else
			const auto* cb = new onSendCallback(
			  [tccClientWeakPtr, camelClientWeakPtr, packetInfo](bool sent)
			  {
				  if (sent)
				  {
					  auto tccClient = tccClientWeakPtr.lock();

					  if (tccClient)
					  {
						  tccClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }

					  auto camelClient = camelClientWeakPtr.lock();

					  if (camelClient)
					  {
						  camelClient->PacketSent(packetInfo, DepLibUV::GetTimeMsInt64());
					  }
				  }
			  });

			SendRtpPacket(nullptr, packet, cb);
#endif
		}
		else
		{
			// May emit 'trace' event.
			EmitTraceEventProbationType(packet);

			SendRtpPacket(nullptr, packet);
		}

		this->sendProbationTransmission.Update(packet);

		MS_DEBUG_DEV(
		  "probation sent [seq:%" PRIu16 ", wideSeq:%" PRIu16 ", size:%zu, bitrate:%" PRIu32 "]",
		  packet->GetSequenceNumber(),
		  this->transportWideCcSeq,
		  packet->GetSize(),
		  this->sendProbationTransmission.GetBitrate(DepLibUV::GetTimeMs()));
	}

	inline void Transport::OnTransportCongestionControlServerSendRtcpPacket(
	  RTC::TransportCongestionControlServer* /*tccServer*/, RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		packet->Serialize(RTC::RTCP::Buffer);

		SendRtcpPacket(packet);
	}

#ifdef ENABLE_RTC_SENDER_BANDWIDTH_ESTIMATOR
	inline void Transport::OnSenderBandwidthEstimatorAvailableBitrate(
	  RTC::SenderBandwidthEstimator* /*senderBwe*/,
	  uint32_t availableBitrate,
	  uint32_t previousAvailableBitrate)
	{
		MS_TRACE();

		MS_DEBUG_DEV(
		  "outgoing available bitrate [now:%" PRIu32 ", before:%" PRIu32 "]",
		  availableBitrate,
		  previousAvailableBitrate);

		// TODO: Uncomment once just SenderBandwidthEstimator is used.
		// DistributeAvailableOutgoingBitrate();
		// ComputeOutgoingDesiredBitrate();
	}
#endif

	inline void Transport::OnTimer(TimerHandle* timer)
	{
		MS_TRACE();

		// RTCP timer.
		if (timer == this->rtcpTimer)
		{
			auto interval        = static_cast<uint64_t>(RTC::RTCP::MaxVideoIntervalMs);
			const uint64_t nowMs = DepLibUV::GetTimeMs();

			SendRtcp(nowMs);

			/*
			 * The interval between RTCP packets is varied randomly over the range
			 * [1.0, 1.5] times the calculated interval to avoid unintended
			 * synchronization of all participants.
			 */
			interval *= static_cast<float>(Utils::Crypto::GetRandomUInt(10, 15)) / 10;

			this->rtcpTimer->Start(interval);
		}
	}

	void RTC::Transport::WriteCamelMetricsCsv()
	{
		static constexpr const char* CsvPath =
		  "/home/n2sl/yeon/qos/network/log/frame/csv/camel_metrics.csv";

		const auto nowMs = DepLibUV::GetTimeMsInt64();

		uint32_t gccAvailableBitrateBps = 0u;

		if (this->tccClient)
		{
			gccAvailableBitrateBps = this->tccClient->GetAvailableBitrate();
		}

		uint32_t camelAvailableBitrateBps = 0u;
		double camelAvgFrameBandwidthBps  = 0.0;
		double camelGamma                 = 1.0;
		double camelBdpBytes              = 0.0;
		double camelCwndBytes             = 0.0;
		uint32_t camelBurstLengthBytes    = 0u;

		if (this->camelClient)
		{
			const auto& camelBitrates = this->camelClient->GetBitrates();

			camelAvailableBitrateBps  = camelBitrates.availableBitrate;
			camelAvgFrameBandwidthBps = camelBitrates.avgFrameBandwidthBps;
			camelGamma                = camelBitrates.gamma;
			camelBdpBytes             = camelBitrates.bdpBytes;
			camelCwndBytes            = camelBitrates.cwndBytes;
			camelBurstLengthBytes     = camelBitrates.burstLengthBytes;
		}

		const uint32_t aceBucketSizeBytes = GetAceBucketSizeBytes();

		static bool headerChecked = false;
		static bool headerWritten = false;

		if (!headerChecked)
		{
			std::ifstream in(CsvPath);

			headerWritten = in.good() && in.peek() != std::ifstream::traits_type::eof();

			headerChecked = true;
		}

		std::ofstream out(CsvPath, std::ios::app);

		if (!out.is_open())
		{
			MS_WARN_TAG(bwe, "failed to open Camel metrics CSV [path:%s]", CsvPath);
			return;
		}

		if (!headerWritten)
		{
			out << "timestampMs,"
			    << "gccAvailableBitrateBps,"
			    << "camelAvailableBitrateBps,"
			    << "camelAvgFrameBandwidthBps,"
			    << "camelGamma,"
			    << "camelBdpBytes,"
			    << "camelCwndBytes,"
			    << "camelBurstLengthBytes,"
			    << "aceBucketSizeBytes"
			    << "\n";

			headerWritten = true;
		}

		out << nowMs << "," << gccAvailableBitrateBps << "," << camelAvailableBitrateBps << ","
		    << std::fixed << std::setprecision(3) << camelAvgFrameBandwidthBps << "," << camelGamma
		    << "," << camelBdpBytes << "," << camelCwndBytes << "," << camelBurstLengthBytes << ","
		    << aceBucketSizeBytes << "\n";
	}

	uint32_t Transport::GetCamelBurstLengthBytes() const
	{
		if (!this->camelClient)
		{
			return 0u;
		}

		return this->camelClient->GetBurstLengthBytes();
	}

	uint32_t Transport::GetGccAvailableOutgoingBitrate() const
	{
		if (!this->tccClient)
		{
			return 0u;
		}

		return this->tccClient->GetAvailableBitrate();
	}

	uint32_t Transport::GetCamelAvailableOutgoingBitrate() const
	{
		if (!this->camelClient)
		{
			return 0u;
		}

		return this->camelClient->GetAvailableBitrate();
	}

} // namespace RTC
