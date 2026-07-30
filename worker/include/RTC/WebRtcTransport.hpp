#ifndef MS_RTC_WEBRTC_TRANSPORT_HPP
#define MS_RTC_WEBRTC_TRANSPORT_HPP

#include "RTC/DtlsTransport.hpp"
#include "RTC/IceCandidate.hpp"
#include "RTC/IceServer.hpp"
#include "RTC/Shared.hpp"
#include "RTC/SrtpSession.hpp"
#include "RTC/StunPacket.hpp"
#include "RTC/TcpConnection.hpp"
#include "RTC/TcpServer.hpp"
#include "RTC/Transport.hpp"
#include "RTC/TransportTuple.hpp"
#include "RTC/UdpSocket.hpp"

// yeon: deadline slack
#include "RTC/FramePacketCsvWriter.hpp"
#include "RTC/FrameRecord.hpp"
#include "RTC/FrameRecordCsvWriter.hpp"
#include "RTC/NetworkState.hpp"
#include "RTC/SlackPredictor.hpp"
#include <vector>

// yeon: (pacer 구현)
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>

// yeon: multi viewer
#include <unordered_map>

// -------------------------------------------------------------------
// App message kind
// -------------------------------------------------------------------
enum class AppMessageKind
{
	None,
	Latency,
	SyncReq,
	SyncResp,
	Chat,
	Unknown
};

struct ParsedSctpAppMessage
{
	AppMessageKind kind{ AppMessageKind::None };

	// SCTP DATA chunk metadata
	uint16_t sid{ 0 };
	uint16_t ssn{ 0 };
	uint32_t ppid{ 0 };
	uint32_t tsn{ 0 };

	// Raw text payload
	std::string text;

	// Common parsed fields
	uint32_t seq{ 0 };

	// sync_req
	double t1ViewMs{ 0.0 };

	// sync_resp
	double t2SfuMs{ 0.0 };

	// latency
	double s2cMs{ 0.0 };

	bool valid{ false };
};
namespace RTC
{
	class WebRtcTransport : public RTC::Transport,
	                        public RTC::UdpSocket::Listener,
	                        public RTC::TcpServer::Listener,
	                        public RTC::TcpConnection::Listener,
	                        public RTC::IceServer::Listener,
	                        public RTC::DtlsTransport::Listener
	{
	public:
		class WebRtcTransportListener
		{
		public:
			virtual ~WebRtcTransportListener() = default;

		public:
			virtual void OnWebRtcTransportCreated(RTC::WebRtcTransport* webRtcTransport) = 0;
			virtual void OnWebRtcTransportClosed(RTC::WebRtcTransport* webRtcTransport)  = 0;
			virtual void OnWebRtcTransportLocalIceUsernameFragmentAdded(
			  RTC::WebRtcTransport* webRtcTransport, const std::string& usernameFragment) = 0;
			virtual void OnWebRtcTransportLocalIceUsernameFragmentRemoved(
			  RTC::WebRtcTransport* webRtcTransport, const std::string& usernameFragment) = 0;
			virtual void OnWebRtcTransportTransportTupleAdded(
			  RTC::WebRtcTransport* webRtcTransport, RTC::TransportTuple* tuple) = 0;
			virtual void OnWebRtcTransportTransportTupleRemoved(
			  RTC::WebRtcTransport* webRtcTransport, RTC::TransportTuple* tuple) = 0;
		};

	public:
		WebRtcTransport(
		  RTC::Shared* shared,
		  const std::string& id,
		  RTC::Transport::Listener* listener,
		  const FBS::WebRtcTransport::WebRtcTransportOptions* options);
		WebRtcTransport(
		  RTC::Shared* shared,
		  const std::string& id,
		  RTC::Transport::Listener* listener,
		  WebRtcTransportListener* webRtcTransportListener,
		  const std::vector<RTC::IceCandidate>& iceCandidates,
		  const FBS::WebRtcTransport::WebRtcTransportOptions* options);
		~WebRtcTransport() override;

	public:
		flatbuffers::Offset<FBS::WebRtcTransport::GetStatsResponse> FillBufferStats(
		  flatbuffers::FlatBufferBuilder& builder);
		flatbuffers::Offset<FBS::WebRtcTransport::DumpResponse> FillBuffer(
		  flatbuffers::FlatBufferBuilder& builder) const;
		void ProcessStunPacketFromWebRtcServer(RTC::TransportTuple* tuple, RTC::StunPacket* packet);
		void ProcessNonStunPacketFromWebRtcServer(
		  RTC::TransportTuple* tuple, const uint8_t* data, size_t len);
		void RemoveTuple(RTC::TransportTuple* tuple);

		/* Methods inherited from Channel::ChannelSocket::RequestHandler. */
	public:
		void HandleRequest(Channel::ChannelRequest* request) override;

		/* Methods inherited from Channel::ChannelSocket::NotificationHandler. */
	public:
		void HandleNotification(Channel::ChannelNotification* notification) override;

	private:
		bool IsConnected() const override;
		void MayRunDtlsTransport();
		void SendRtpPacket(
		  RTC::Consumer* consumer,
		  RTC::RtpPacket* packet,
		  RTC::Transport::onSendCallback* cb = nullptr) override;
		void SendRtcpPacket(RTC::RTCP::Packet* packet) override;
		void SendRtcpCompoundPacket(RTC::RTCP::CompoundPacket* packet) override;
		void SendMessage(
		  RTC::DataConsumer* dataConsumer,
		  const uint8_t* msg,
		  size_t len,
		  uint32_t ppid,
		  onQueuedCallback* cb = nullptr) override;
		void SendSctpData(const uint8_t* data, size_t len) override;
		// yeon
		void HandleParsedAppMessage(const ParsedSctpAppMessage& msg);
		void SendSyncResponse(uint16_t sid, uint32_t seq, double t1ViewMs, double t2SfuMs);
		void SendTextMessageToSctpStream(uint16_t sid, const std::string& payload);

		void RecvStreamClosed(uint32_t ssrc) override;
		void SendStreamClosed(uint32_t ssrc) override;
		void OnPacketReceived(RTC::TransportTuple* tuple, const uint8_t* data, size_t len);
		void OnStunDataReceived(RTC::TransportTuple* tuple, const uint8_t* data, size_t len);
		void OnDtlsDataReceived(const RTC::TransportTuple* tuple, const uint8_t* data, size_t len);
		void OnRtpDataReceived(RTC::TransportTuple* tuple, const uint8_t* data, size_t len);
		void OnRtcpDataReceived(RTC::TransportTuple* tuple, const uint8_t* data, size_t len);

		/* Pure virtual methods inherited from RTC::UdpSocket::Listener. */
	public:
		void OnUdpSocketPacketReceived(
		  RTC::UdpSocket* socket, const uint8_t* data, size_t len, const struct sockaddr* remoteAddr) override;

		/* Pure virtual methods inherited from RTC::TcpServer::Listener. */
	public:
		void OnRtcTcpConnectionClosed(RTC::TcpServer* tcpServer, RTC::TcpConnection* connection) override;

		/* Pure virtual methods inherited from RTC::TcpConnection::Listener. */
	public:
		void OnTcpConnectionPacketReceived(
		  RTC::TcpConnection* connection, const uint8_t* data, size_t len) override;

		/* Pure virtual methods inherited from RTC::IceServer::Listener. */
	public:
		void OnIceServerSendStunPacket(
		  const RTC::IceServer* iceServer,
		  const RTC::StunPacket* packet,
		  RTC::TransportTuple* tuple) override;
		void OnIceServerLocalUsernameFragmentAdded(
		  const RTC::IceServer* iceServer, const std::string& usernameFragment) override;
		void OnIceServerLocalUsernameFragmentRemoved(
		  const RTC::IceServer* iceServer, const std::string& usernameFragment) override;
		void OnIceServerTupleAdded(const RTC::IceServer* iceServer, RTC::TransportTuple* tuple) override;
		void OnIceServerTupleRemoved(const RTC::IceServer* iceServer, RTC::TransportTuple* tuple) override;
		void OnIceServerSelectedTuple(const RTC::IceServer* iceServer, RTC::TransportTuple* tuple) override;
		void OnIceServerConnected(const RTC::IceServer* iceServer) override;
		void OnIceServerCompleted(const RTC::IceServer* iceServer) override;
		void OnIceServerDisconnected(const RTC::IceServer* iceServer) override;

		/* Pure virtual methods inherited from RTC::DtlsTransport::Listener. */
	public:
		void OnDtlsTransportConnecting(const RTC::DtlsTransport* dtlsTransport) override;
		void OnDtlsTransportConnected(
		  const RTC::DtlsTransport* dtlsTransport,
		  RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
		  uint8_t* srtpLocalKey,
		  size_t srtpLocalKeyLen,
		  uint8_t* srtpRemoteKey,
		  size_t srtpRemoteKeyLen,
		  std::string& remoteCert) override;
		void OnDtlsTransportFailed(const RTC::DtlsTransport* dtlsTransport) override;
		void OnDtlsTransportClosed(const RTC::DtlsTransport* dtlsTransport) override;
		void OnDtlsTransportSendData(
		  const RTC::DtlsTransport* dtlsTransport, const uint8_t* data, size_t len) override;
		void OnDtlsTransportApplicationDataReceived(
		  const RTC::DtlsTransport* dtlsTransport, const uint8_t* data, size_t len) override;

	private:
		// Passed by argument.
		WebRtcTransportListener* webRtcTransportListener{ nullptr };
		// Allocated by this.
		RTC::IceServer* iceServer{ nullptr };
		// Map of UdpSocket/TcpServer and local announced address (if any).
		absl::flat_hash_map<RTC::UdpSocket*, std::string> udpSockets;
		absl::flat_hash_map<RTC::TcpServer*, std::string> tcpServers;
		RTC::DtlsTransport* dtlsTransport{ nullptr };
		RTC::SrtpSession* srtpRecvSession{ nullptr };
		RTC::SrtpSession* srtpSendSession{ nullptr };
		// Others.
		// Whether connect() was succesfully called.
		bool connectCalled{ false };
		std::vector<RTC::IceCandidate> iceCandidates;
		RTC::DtlsTransport::Role dtlsRole{ RTC::DtlsTransport::Role::AUTO };

		//------// pacing으로 추가한 부분
		// yeon: (pacer 구현)
	private:
		struct PendingRtp
		{
			RTC::SharedRtpPacket sharedPacket;  // owns a cloned RTP packet
			RTC::Consumer* consumer{ nullptr }; // valid while transport alive (same worker thread)
			const RTC::Transport::onSendCallback* cb{ nullptr }; // must call + delete
			uint64_t enqueuedAtMs{ 0 };
		};

		class TokenBucketPacer
		{
		public:
			explicit TokenBucketPacer(WebRtcTransport* transport);
			~TokenBucketPacer();

			void SetPacingRate(uint32_t bps);        // 일단 고정값 사용
			void SetBucketSize(uint32_t burketSize); // controls bucket size
			void SetQueueLimits(uint32_t maxDelayMs, size_t maxBytes);
			// 업데이트
			void UpdateBucketSize();
			void IncreaseBucketSize();
			void DecreaseBucketSize();
			void UpdatePredictedQueueBytes();

			// Enqueue a packet
			void Enqueue(
			  RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb);

			// Called when transport is closing to cleanup
			void StopAndFlush(bool sent);

			// 버킷 사이즈 업데이트에 반영하기 위한 네트워크 상황 지표
			void SetRttMs(double rttMs);
			void NotifyPacketLoss();

			// getter method
			double GetLinkCapacityBytesPerMs() const;
			double GetPacingRate();
			double GetBucketSize() const;
			size_t IsLossDetected() const;
			void SetLinkCapacityBytesPerMs(double value);
			void SetLossDetected(double value);

		private:
			void EnsureTimer();
			void PacerTimer(uint64_t delayMs);
			void OnTimer();
			void Refill(uint64_t nowMs);

			bool TrySendOne(uint64_t nowMs);
			bool ShouldDrop(const PendingRtp& item, uint64_t nowMs) const;

		public:
			WebRtcTransport* transport{ nullptr };

			// copa style의 RTT 계산을 위함: min RTT, Long RTT
			struct RttSample
			{
				int64_t timeMs;
				double rttMs;
			};
			std::deque<RttSample> rttHistory;

			// Token bucket state (bytes-based)
			double tokenRateBytesPerMs{ 0.0 };
			double bucketCapacityBytes{ 0.0 };
			double tokensBytes{ 0.0 };
			uint64_t lastRefillMs{ 0 };

			// ACE: pacer 관련 필드
			double additiveStepBytes{ 1200.0 };
			double alpha{ 0.8 };
			// queue size에 대한 threshold
			double queueThresholdBytes{ 12000.0 };
			// historical bucket when buffer empty
			double historicalEmptyBucketBytes{ 0.0 };
			// recent pre-loss queue size
			double lastQueueBytesBeforeLoss{ 0.0 };
			// 추정한 network queue size
			double predictedQueueBytes{ 0.0 };

			double minBucketBytes{ 1200.0 };
			// double maxBucketBytes{ 256 * 1024.0 };

			size_t maxQueueBytes{ 2 * 1024 * 1024 }; // default 2MB

			bool hasHistoricalInfo{ false };
			size_t lossDetected{ 0 };
			size_t previousFrameBytes{ 0 };

			double latestRttMs{ 0.0 };   // 최근 RTT값
			double srttMs{ 0.0 };        // copa style: queue delay 추정
			double standingRttMs{ 0.0 }; // copa style: queue delay 추정
			double minRttMs{ 0.0 };
			double queueDelayMs{ 0.0 };           // copa style: queue delay 추정
			double linkCapacityBytesPerMs{ 0.0 }; // 최근 link capacity: packetpair를 사용해야 하지만,
			                                      // 일단 현재는 gcc 비트레이트 사용

			int64_t flowStartTimeMs{ 0 }; // copa style: queue delay 추정
			bool started{ false };        // copa style: queue delay 추정

			// Queue
			std::deque<PendingRtp> q;
			size_t queuedBytes{ 0 };

			// Safety limits
			uint32_t maxQueueDelayMs{ 200 }; // default

			// libuv timer (same worker loop)
			uv_timer_t timer{};
			bool timerInited{ false };

			// Config
			uint32_t burstWindowMs{ 20 }; // default
		public:
			// 이전 프레임 사이즈 측정을 위한 필드
			uint32_t currentFrameTimestamp{ 0 };
			size_t currentFrameBytes{ 0 };
			bool frameInit{ false };
			void ObservePacketForFrame(const RTC::RtpPacket* pkt);
		};

		// yeon: pacer bitrate 즉, pacing rate를 조절하기 위해 사용하는 함수
	protected:
		void OnAvailableBitrateChanged(uint32_t availableBitrate) override;
		void OnPacketLossDetected(double loss) override;
		void OnRttUpdated(double rttMs) override;
		void OnSlack(const uint8_t* msg, size_t len) override;
		uint32_t GetAceBucketSizeBytes() const override;

	private:
		// yeon: TokenBucketPacer을 하나만 두고 소유를 함부로 주지 못하게
		std::unique_ptr<TokenBucketPacer> rtpPacer;

		std::unique_ptr<RTC::SlackPredictor> slackPredictor;

		// consumerId별 FrameRecordTable.
		// 각 table 내부에서는 기존처럼 frameId만 key로 써도 됨.
		std::unordered_map<std::string, std::shared_ptr<RTC::FrameRecordTable>> frameRecordTablesByConsumerId;

		// 이 transport 안에서 RTP timestamp가 어느 consumer로 나갔는지 기록.
		// 브라우저 telemetry에는 consumerId가 없으므로 OnSlack에서 이 map으로 찾음.
		std::unordered_map<uint32_t, std::string> frameConsumerIdByRtpTimestamp;

		// predicted slack도 consumerId + frameId 기준으로 관리.
		std::unordered_map<std::string, std::unordered_map<uint32_t, double>> pendingPredictedSlackByConsumerFrame;

		// 새 frame 감지도 consumer별로 관리.
		std::unordered_map<std::string, bool> predictFrameInitByConsumerId;
		std::unordered_map<std::string, uint32_t> currentPredictFrameTimestampByConsumerId;

		// Helper: actual immediate send path (your existing code moved here)
		void SendRtpPacketNow(
		  RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb);

		void PredictSlack(RTC::RtpPacket* packet);
		void PredictSlack(RTC::Consumer* consumer, RTC::RtpPacket* packet);
		FrameRecordTable* GetFrameRecordTableForConsumer(const std::string& consumerId);
		//------// pacing으로 추가한 부분
		// yeon: deadline slack
	public:
		std::unique_ptr<RTC::NetworkState> networkState;
		// std::unique_ptr<RTC::FrameRecordTable> frameRecordTable;
		std::shared_ptr<RTC::FrameRecordTable> frameRecordTable;
		std::unique_ptr<RTC::FrameRecordCsvWriter> frameRecordCsvWriter;
		std::unique_ptr<RTC::FramePacketCsvWriter> framePacketCsvWriter;

		// yeon: multi viewer
	private:
		std::optional<double> currentPredictedSlack;
	};

} // namespace RTC

#endif
