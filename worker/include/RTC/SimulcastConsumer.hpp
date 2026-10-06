#ifndef MS_RTC_SIMULCAST_CONSUMER_HPP

#define MS_RTC_SIMULCAST_CONSUMER_HPP



#include "FBS/consumer.h"

#include "RTC/Codecs/PayloadDescriptorHandler.hpp"

#include "RTC/Consumer.hpp"

#include "RTC/SeqManager.hpp"

#include "RTC/Shared.hpp"

#include "RTC/SlackPredictor.hpp"

#include <array>

#include <deque>

#include <map>

#include <unordered_map>

#include <unordered_set>



namespace RTC

{

    class SimulcastConsumer : public RTC::Consumer, public RTC::RtpStreamSend::Listener

    {

    public:

        SimulcastConsumer(

          RTC::Shared* shared,

          const std::string& id,

          const std::string& producerId,

          RTC::Consumer::Listener* listener,

          const FBS::Transport::ConsumeRequest* data);

        ~SimulcastConsumer() override;



    public:

        flatbuffers::Offset<FBS::Consumer::DumpResponse> FillBuffer(

          flatbuffers::FlatBufferBuilder& builder) const;

        flatbuffers::Offset<FBS::Consumer::GetStatsResponse> FillBufferStats(

          flatbuffers::FlatBufferBuilder& builder) override;

        flatbuffers::Offset<FBS::Consumer::ConsumerScore> FillBufferScore(

          flatbuffers::FlatBufferBuilder& builder) const override;

        VideoLayers GetPreferredLayers() const override

        {

            VideoLayers layers;



            layers.spatial  = this->preferredLayers.spatial;

            layers.temporal = this->preferredLayers.temporal;



            return layers;

        }



        VideoLayers GetTargetLayers() const override

        {

            VideoLayers layers;



            layers.spatial  = this->targetLayers.spatial;

            layers.temporal = this->targetLayers.temporal;



            return layers;

        }



        // yeon: target layer에 webrtctransport가 접근하기 위한 수정사항 consumer.hpp의 함수를 오버라이드

        int16_t GetCurrentSpatialLayer() const override

        {

            return this->currentSpatialLayer;

        }



        int16_t GetTargetSpatialLayer() const override

        {

            return this->targetLayers.spatial;

        }



        int16_t GetPreferredSpatialLayer() const override

        {

            return this->preferredLayers.spatial;

        }



        bool IsActive() const override

        {

            // clang-format off

            return (

                RTC::Consumer::IsActive() &&

                std::any_of(

                    this->producerRtpStreams.begin(),

                    this->producerRtpStreams.end(),

                    [](const RTC::RtpStreamRecv* rtpStream)

                    {

                        // If there is no RTP inactivity check do not consider the stream

                        // inactive despite it has score 0.

                        return (rtpStream != nullptr && (rtpStream->GetScore() > 0u || !rtpStream->HasRtpInactivityCheckEnabled()));

                    }

                )

            );

            // clang-format on

        }

        void ProducerRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc) override;

        void ProducerNewRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc) override;

        void ProducerRtpStreamScore(

          RTC::RtpStreamRecv* rtpStream, uint8_t score, uint8_t previousScore) override;

        void ProducerRtcpSenderReport(RTC::RtpStreamRecv* rtpStream, bool first) override;

        uint8_t GetBitratePriority() const override;

        uint32_t IncreaseLayer(uint32_t bitrate, bool considerLoss) override;

        void ApplyLayers() override;

        uint32_t GetDesiredBitrate() const override;

        void SendRtpPacket(RTC::RtpPacket* packet, RTC::SharedRtpPacket& sharedPacket) override;

        bool GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs) override;

        const std::vector<RTC::RtpStreamSend*>& GetRtpStreams() const override

        {

            return this->rtpStreams;

        }

        void NeedWorstRemoteFractionLost(uint32_t mappedSsrc, uint8_t& worstRemoteFractionLost) override;

        void ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket) override;

        void ReceiveKeyFrameRequest(RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc) override;

        void ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report) override;

        void ReceiveRtcpXrReceiverReferenceTime(RTC::RTCP::ReceiverReferenceTime* report) override;

        uint32_t GetTransmissionRate(uint64_t nowMs) override;

        float GetRtt() const override;



        /* Methods inherited from Channel::ChannelSocket::RequestHandler. */

    public:

        void HandleRequest(Channel::ChannelRequest* request) override;



    private:

        void UserOnTransportConnected() override;

        void UserOnTransportDisconnected() override;

        void UserOnPaused() override;

        void UserOnResumed() override;

        void CreateRtpStream();

        void RequestKeyFrames();

        void RequestKeyFrameForTargetSpatialLayer();

        void RequestKeyFrameForCurrentSpatialLayer();

        void MayChangeLayers(bool force = false);

        bool RecalculateTargetLayers(VideoLayers& newTargetLayers) const;

        void UpdateTargetLayers(int16_t newTargetSpatialLayer, int16_t newTargetTemporalLayer);

        bool CanSwitchToSpatialLayer(int16_t spatialLayer) const;

        void EmitScore() const;

        void StorePacketInTargetLayerRetransmissionBuffer(

          RTC::RtpPacket* packet, RTC::SharedRtpPacket& sharedPacket);

        void EmitLayersChange() const;

        RTC::RtpStreamRecv* GetProducerCurrentRtpStream() const;

        RTC::RtpStreamRecv* GetProducerTargetRtpStream() const;

        RTC::RtpStreamRecv* GetProducerTsReferenceRtpStream() const;



        /* Pure virtual methods inherited from RtpStreamSend::Listener. */

    public:

        void OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;

        void OnRtpStreamRetransmitRtpPacket(RTC::RtpStreamSend* rtpStream, RTC::RtpPacket* packet) override;



    private:

        // Allocated by this.

        RTC::RtpStreamSend* rtpStream{ nullptr };

        // Others.

        absl::flat_hash_map<uint32_t, int16_t> mapMappedSsrcSpatialLayer;

        std::vector<RTC::RtpStreamSend*> rtpStreams;

        std::vector<RTC::RtpStreamRecv*> producerRtpStreams; // Indexed by spatial layer.

        bool syncRequired{ false };

        int16_t spatialLayerToSync{ -1 };

        bool lastSentPacketHasMarker{ false };

        RTC::SeqManager<uint16_t> rtpSeqManager;

        VideoLayers preferredLayers;

        VideoLayers provisionalTargetLayers;

        VideoLayers targetLayers;

        int16_t currentSpatialLayer{ -1 };

        int16_t tsReferenceSpatialLayer{ -1 }; // Used for RTP TS sync.

        uint16_t snReferenceSpatialLayer{ 0 };

        bool checkingForOldPacketsInSpatialLayer{ false };

        std::unique_ptr<RTC::Codecs::EncodingContext> encodingContext;

        uint32_t tsOffset{ 0u }; // RTP Timestamp offset.

        bool keyFrameForTsOffsetRequested{ false };

        // Last time we moved to lower spatial layer due to BWE.

        uint64_t lastBweDowngradeAtMs{ 0u };

        // Buffer to store packets that arrive earlier than the first packet of the

        // video key frame.

        std::map<uint16_t, RTC::SharedRtpPacket, RTC::SeqManager<uint16_t>::SeqLowerThan>

          targetLayerRetransmissionBuffer;



    public:

        int16_t GetQosCurrentSpatialLayer() const override

        {

            return this->currentSpatialLayer;

        }



        int16_t GetQosTargetSpatialLayer() const override

        {

            return this->targetLayers.spatial;

        }



        int16_t GetQosProvisionalTargetSpatialLayer() const override

        {

            return this->provisionalTargetLayers.spatial;

        }



        void ForceQosProvisionalSpatialLayer(int16_t spatialLayer) override

        {

            if (spatialLayer < 0)

            {

                return;

            }



            if (spatialLayer >= static_cast<int16_t>(this->producerRtpStreams.size()))

            {

                return;

            }



            this->provisionalTargetLayers.spatial  = spatialLayer;

            this->provisionalTargetLayers.temporal = 0;

        }



    private:

        // Slack/KNN logical frame decision duplicate suppression.

        // Simulcast의 여러 spatial layer에서 동일 logicalFrameId의

        // first packet이 서로 다른 시점에 도착할 수 있다.

        // 따라서 "직전에 처리한 logicalFrameId" 하나만 기억하지 않고,

        // 최근 처리한 logicalFrameId들을 bounded cache로 유지한다.

        static constexpr size_t SlackDecisionLogicalFrameCacheSize{ 512u };



        bool HasProcessedSlackDecisionLogicalFrame(uint32_t logicalFrameId) const;

        void RememberProcessedSlackDecisionLogicalFrame(uint32_t logicalFrameId);



        std::unordered_set<uint32_t> processedSlackDecisionLogicalFrameIds;

        std::deque<uint32_t> processedSlackDecisionLogicalFrameOrder;



    private:

        std::unordered_map<uint32_t, RTC::SlackActionDecision> pendingSlackActionDecisionByLogicalFrame;



    private:

        bool knnControlActive{ false };



        // yeon: kNN spatial-layer CAP mode.

        // kNN이 선택한 layer를 직접 target layer로 강제하지 않고,

        // 기존 mediasoup BWE allocator가 선택할 수 있는

        // maximum spatial layer로 사용한다.

        int16_t knnMaxSpatialLayer{ -1 };



        // cold start:



        // ============================================================

        // yeon: last valid KNN action.

        // KNN phase에 들어간 뒤 일부 candidate prediction이 일시적으로

        // 불가능해지면 마지막으로 정상 선택했던 KNN action을 유지하도록

        // ============================================================

        bool hasLastKnnAction{ false };

        size_t lastKnnLayer{ 0u };

        size_t lastKnnFecIndex{ 0u };

        size_t lastKnnPacingIndex{ 0u };



        // 기존 cold start 샘플 수집 관련 모드

        //   frame    1 ~ 1000 : default pacing

        //   frame 1001 ~ 2000 : opposite pacing

        static constexpr size_t KnnColdStartDefaultPacingFrames{ 1000u };

        static constexpr size_t KnnColdStartOppositePacingFrames{ 1000u };



        size_t knnColdStartFrameCount{ 0u };



        bool knnColdStartDefaultPacingCaptured{ false };

        bool knnColdStartDefaultPacingEnabled{ false };



        // ============================================================

        // yeon: Cold Start spatial-layer force.

        // ============================================================



        bool knnColdStartForceLayerActive{ false };

        int16_t knnColdStartForcedSpatialLayer{ -1 };



        // ============================================================

        // yeon: Cold Start sequential spatial-layer collection.

        //

        // 순서:

        //   L0 -> L1 -> L2

        //

        // 각 layer의 sample 수는:

        //   KnnColdStartSamplesPerNetworkPhase * option profile count

        // 로 동적으로 결정된다.

        // A/B/E/F/G : 2 profiles

        // C         : 3 profiles

        // D/H       : 4 profiles

        // ============================================================



        RTC::KnnExperiment::NetworkExperimentOption knnNetworkExperimentOption{

            RTC::KnnExperiment::NetworkExperimentOption::A

        };



        size_t knnNetworkProfileCount{ 2u };

        size_t knnColdStartSamplesPerLayer{ 3000u };

        size_t knnColdStartRequiredTotalSamples{ 9000u };



        static constexpr size_t KnnColdStartTcSignal1{ 700u };

        static constexpr size_t KnnColdStartTcSignal2{ 1400u };

        static constexpr size_t KnnColdStartTcSignal3{ 2100u };



        // layer 변경 직후 안정화 구간.

        // 기존 값 그대로 유지.

        static constexpr size_t KnnColdStartLayerSettleFrames{ 60u };



        // ============================================================

        // collection order:

        //

        // 0 -> 1 -> 2

        // ============================================================



        int16_t knnColdStartCollectionLayer{ 0 };



        // 현재 collection layer에 실제 도달한 뒤 settle frame count.

        size_t knnColdStartStableLayerFrameCount{ 0u };



        // [L0, L1, L2]

        std::array<size_t, 3u> knnColdStartLayerSampleCounts{};



        // 실제 predictor에 들어간 Cold Start sample 총 개수.

        size_t knnColdStartTotalSampleCount{ 0u };



        bool knnColdStartCollectionComplete{ false };



        // ============================================================

        // yeon: Balanced Cold Start Scenario V2.

        //

        // false : 기존 cold-start 방식.

        // true  : Layer x FEC x Pacing balanced sampling + pause.

        // ============================================================

        static constexpr bool KnnColdStartBalancedScenarioEnabled{

            RTC::KnnExperiment::ColdStartActionCoverageEnabled

        };



        static constexpr bool KnnExperimentPauseStateEnabled{

    RTC::KnnExperiment::PauseStateEnabled

};



        static constexpr size_t KnnColdStartSamplesPerNetworkPhase{ 1500u };

        static constexpr size_t KnnColdStartSamplesPerActionPerNetworkPhase{ 150u };



        static constexpr size_t KnnColdStartPauseFrames{ 150u };



        static constexpr size_t KnnColdStartFecActionCount{ 5u };

        static constexpr size_t KnnColdStartPacingActionCount{ 2u };

        static constexpr size_t KnnColdStartActionCount{ KnnColdStartFecActionCount *

                                                         KnnColdStartPacingActionCount };



        static_assert(

          KnnColdStartSamplesPerNetworkPhase ==

            KnnColdStartSamplesPerActionPerNetworkPhase * KnnColdStartActionCount,

          "Cold-start balanced sample configuration mismatch");



        enum class KnnColdStartScenarioState : uint8_t

        {

            COLLECT = 0,

            NETWORK_PAUSE,

            LAYER_PAUSE,

            COMPLETE

        };



        KnnColdStartScenarioState knnColdStartScenarioState{ KnnColdStartScenarioState::COLLECT };



        // 시작은 사용자가 tc 제한을 주지 않은 NORMAL.

        RTC::KnnColdStartNetworkProfile knnColdStartNetworkProfile{

            RTC::KnnColdStartNetworkProfile::NORMAL

        };



        RTC::KnnColdStartNetworkProfile knnColdStartPauseTargetNetworkProfile{

            RTC::KnnColdStartNetworkProfile::NORMAL

        };



        // 현재 network collection 구간에서 확보된 valid samples.

        size_t knnColdStartPhaseSampleCount{ 0u };



        // pause에서는 logical frame 기준.

        size_t knnColdStartPauseFrameCount{ 0u };



        // 10-action round-robin 위치.

        size_t knnColdStartActionCursor{ 0u };



        // 현재 network 구간의 valid sample 수.

        // [fecIdx][pacingIdx]

        std::array<std::array<size_t, KnnColdStartPacingActionCount>, KnnColdStartFecActionCount>

          knnColdStartPhaseActionSampleCounts{};



        // 전체 layer 기준 action coverage 확인용.

        // [layer][fecIdx][pacingIdx]

        std::array<std::array<std::array<size_t, KnnColdStartPacingActionCount>, KnnColdStartFecActionCount>, 3u>

          knnColdStartLayerActionSampleCounts{};



        bool knnColdStartInitialLayerEnterLogged{ false };



        // ============================================================

        // yeon: KNN Evaluation Scenario.

        //

        // Cold Start 완료 후:

        //

        //   Cold Start final network

        //      ↓

        //   START_NETWORK_PAUSE 150 -> NORMAL

        //      ↓

        //   NORMAL / 1500

        //      ↓

        //   이후 option에 정의된 profile sequence를 순서대로 반복:

        //      MID_NETWORK_PAUSE 150 -> profile / 1500

        //

        //   예) H: NORMAL -> 2MBIT -> 4MBIT -> LOSS_5PCT

        //      ↓

        //   COMPLETE

        //

        // Pause:

        //   prediction/action O

        //   FrameRecord/CSV O

        //   predictor AddSample X

        // ============================================================



        static constexpr size_t KnnEvaluationPauseFrames{ 150u };

        static constexpr size_t KnnEvaluationCollectionFrames{ 1500u };



        enum class KnnEvaluationScenarioState : uint8_t

        {

            NOT_STARTED = 0,



            START_NETWORK_PAUSE,



            COLLECT_NORMAL,



            MID_NETWORK_PAUSE,



            COLLECT_6MBIT,

            COLLECT_2MBIT,

            COLLECT_4MBIT,

            COLLECT_8MBIT,

            COLLECT_LOSS_2PCT,

            COLLECT_LOSS_5PCT,

            COLLECT_LOSS_15PCT,



            COMPLETE

        };



        KnnEvaluationScenarioState knnEvaluationScenarioState{ KnnEvaluationScenarioState::NOT_STARTED };



        // MID_NETWORK_PAUSE에서 변경할 target profile.

        RTC::KnnColdStartNetworkProfile knnEvaluationPauseTargetNetworkProfile{

            RTC::KnnColdStartNetworkProfile::NORMAL

        };



        // 현재 evaluation에서 실제 적용되어 있다고 간주하는 network.

        RTC::KnnColdStartNetworkProfile knnEvaluationNetworkProfile{

            RTC::KnnColdStartNetworkProfile::NORMAL

        };



        // 현재 phase의 logical-frame count.

        size_t knnEvaluationPhaseFrameCount{ 0u };



        // 분석/로그용 실제 collection count.

        size_t knnEvaluationNormalFrameCount{ 0u };

        size_t knnEvaluation6MbitFrameCount{ 0u };

        size_t knnEvaluation2MbitFrameCount{ 0u };

        size_t knnEvaluation4MbitFrameCount{ 0u };

        size_t knnEvaluation8MbitFrameCount{ 0u };

        size_t knnEvaluationLoss2PctFrameCount{ 0u };

        size_t knnEvaluationLoss5PctFrameCount{ 0u };

        size_t knnEvaluationLoss15PctFrameCount{ 0u };



        // Cold Start 종료 직후 evaluation 시작.

        void BeginKnnEvaluation();



        // 매 KNN SlackActionDecision에 evaluation metadata를 붙이고

        // logical-frame 기준 state machine을 진행.

        void AnnotateAndAdvanceKnnEvaluationDecision(RTC::SlackActionDecision& decision);



    public:

        bool GetNextKnnColdStartBalancedAction(size_t& fecIdx, size_t& pacingIdx);



        void ResetKnnColdStartNetworkPhaseCoverage();



        bool IsKnnColdStartNetworkPhaseComplete() const;



        void EnterKnnColdStartNetworkPause();



        void EnterKnnColdStartLayerPause(int16_t nextLayer);



        void CompleteKnnColdStart();



        bool ShouldAcceptKnnColdStartTrainingSample(

          int16_t spatialLayer, uint8_t fecProtectionFactor, bool pacingEnabled) const;



        void OnKnnColdStartTrainingSampleAdded(

          int16_t spatialLayer, uint8_t fecProtectionFactor, bool pacingEnabled);

    };

} // namespace RTC



#endif
