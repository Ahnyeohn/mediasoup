#pragma once



#include <array>

#include <cstddef>

#include <cstdint>

#include <deque>

#include <mutex>

#include <optional>

#include <vector>



namespace RTC

{

    namespace KnnExperiment

    {

        // false = 기존 kNN, true = 동일 action sample만 neighbor 후보.

        static constexpr bool ActionConditionedPredictionEnabled{ true };



        // false = 기존 cold-start, true = 30-action balanced cold-start + pause scenario.

        static constexpr bool ColdStartActionCoverageEnabled{ true };



        static constexpr bool PauseStateEnabled{ true };



        enum class NetworkExperimentOption : uint8_t

        {

            A = 0, // NORMAL, 2MBIT

            B,     // NORMAL, 4MBIT

            C,     // NORMAL, 2MBIT, 4MBIT

            D,     // NORMAL, 2MBIT, 4MBIT, 6MBIT

            E,     // NORMAL, LOSS_2PCT

            F,     // NORMAL, LOSS_5PCT

            G,     // NORMAL, LOSS_15PCT

            H      // NORMAL, 2MBIT, 4MBIT, LOSS_5PCT

        };

    }



    // ============================================================

    // kNN input feature

    // X(A) =

    // [ RTT, Loss, BW, Congestion, FrameSize(A), PredictedPacingDelay(A) ]

    // ============================================================

    struct SlackFeature

    {

        double rttMs{ 0.0 };

        double lossRate{ 0.0 };

        double availableBitrateBps{ 0.0 };



        // Camel congestion detector의 raw congestion signal.

        // S(D, inflight) ~= ΔDelay / ΔInflight, unit: ms / KB.

        double congestion{ 0.0 };



        // Layer + FEC action을 고려한 예상 frame size.

        double frameSizeBytes{ 0.0 };



        // 해당 action에서 예상되는 pacing delay. Pacing OFF이면 0.

        double pacingDelayMs{ 0.0 };

    };



    struct SlackActionIdentity

    {

        uint8_t spatialLayer{ 0u };

        uint8_t fecProtectionFactor{ 0u };

        bool pacingEnabled{ false };

    };



    enum class KnnColdStartPhase : uint8_t

    {

        NONE = 0,

        COLLECT,

        NETWORK_PAUSE,

        LAYER_PAUSE

    };



    enum class KnnColdStartNetworkProfile : uint8_t

    {

        NORMAL = 0,

        LIMIT_6MBIT,

        LIMIT_2MBIT,

        LIMIT_4MBIT,

        // Legacy profile kept for backward-compatible logs / analysis.
        LIMIT_8MBIT,

        LOSS_2PCT,

        LOSS_5PCT,

        LOSS_15PCT

    };



    // ============================================================

    // KNN evaluation phase.

    //

    // Cold Start가 끝난 뒤 KNN 자체를 평가하기 위한 별도 phase.

    // ============================================================

    enum class KnnEvaluationPhase : uint8_t

    {

        NONE = 0,



        // Cold Start 마지막 network에서 NORMAL로 변경하면서 150 frame 대기.

        START_NETWORK_PAUSE,



        // NORMAL network에서 KNN 평가 1500 frames.

        COLLECT_NORMAL,



        // Network profile 전환 후 150 frame 대기.

        MID_NETWORK_PAUSE,



        // 각 network profile에서 KNN 평가 1500 frames.

        COLLECT_6MBIT,

        COLLECT_2MBIT,

        COLLECT_4MBIT,

        // Legacy profile.
        COLLECT_8MBIT,

        COLLECT_LOSS_2PCT,

        COLLECT_LOSS_5PCT,

        COLLECT_LOSS_15PCT,



        COMPLETE

    };



    struct SlackActionDecision

    {

        uint32_t logicalFrameId{ 0u };



        // 실제 kNN에 사용되는 6개 feature.

        SlackFeature feature;



        // 같은 FEC/Pacing action을 L0/L1/L2 각각에 적용했을 때의

        // decision-time feature. Training에서는 actual layer에 해당하는

        // feature를 사용한다.

        std::array<SlackFeature, 3> actionFeatureBySpatialLayer{};

        std::array<bool, 3> hasActionFeatureBySpatialLayer{ false, false, false };



        std::array<double, 3> predictedDecodeSlackBySpatialLayer{ 0.0, 0.0, 0.0 };

        std::array<bool, 3> hasPredictedDecodeSlackBySpatialLayer{ false, false, false };



        std::array<double, 3> predictedDeadlineMissProbabilityBySpatialLayer{ 0.0, 0.0, 0.0 };

        std::array<bool, 3> hasPredictedDeadlineMissProbabilityBySpatialLayer{ false, false, false };



        // Decision-time raw pacer diagnostics.

        double aceQueueBytes{ 0.0 };

        double pacingBacklogBytes{ 0.0 };

        double pacingBucketSizeBytes{ 0.0 };

        double tokenRateBytesPerMs{ 0.0 };



        // kNN prediction for the selected action.

        bool hasPredictedDecodeSlack{ false };

        double predictedDecodeSlackMs{ 0.0 };



        bool hasPredictedDeadlineMissProbability{ false };

        double predictedDeadlineMissProbability{ 0.0 };



        // Action identity / execution metadata.

        uint8_t spatialLayer{ 0u };

        size_t fecIndex{ 0u };

        double fecRedundancyRate{ 0.0 };

        uint8_t fecProtectionFactor{ 0u };

        bool pacingEnabled{ false };



        // false: cold-start decision, true: kNN-selected decision.

        bool modelSelected{ false };



        // Balanced Cold Start에서만 true.

        // true이면 WebRtcTransport가 legacy adaptive FEC / global ispacing 대신

        // 이 decision의 FEC/Pacing을 현재 frame에 직접 적용한다.

        bool forceRuntimeAction{ false };



        // Cold Start pause 및 KNN evaluation pause에서 true.

        // prediction/action/FrameRecord/CSV는 모두 정상 수행하지만

        // SlackPredictor historical training sample에는 추가하지 않는다.

        bool skipKnnTrainingSample{ false };



        // Cold Start experiment metadata.

        KnnColdStartPhase coldStartPhase{ KnnColdStartPhase::NONE };

        KnnColdStartNetworkProfile coldStartNetworkProfile{ KnnColdStartNetworkProfile::NORMAL };

        KnnColdStartNetworkProfile coldStartTargetNetworkProfile{ KnnColdStartNetworkProfile::NORMAL };



        // pause frame이면 1~150, 그 외에는 0.

        size_t coldStartPauseFrameIndex{ 0u };



        // ============================================================

        // KNN evaluation metadata.

        //

        // modelSelected == true인 KNN frame에서도

        // 현재 evaluation phase를 FrameRecord/CSV에 보존.

        // ============================================================



        KnnEvaluationPhase knnEvaluationPhase{ KnnEvaluationPhase::NONE };



        // 이 frame을 decision한 시점에서 현재 실험 network profile.

        KnnColdStartNetworkProfile knnEvaluationNetworkProfile{ KnnColdStartNetworkProfile::NORMAL };



        // pause 중 변경하려는 목표 network.

        // collect 상태에서는 current == target.

        KnnColdStartNetworkProfile knnEvaluationTargetNetworkProfile{ KnnColdStartNetworkProfile::NORMAL };



        // 각 evaluation phase 내부 frame 번호.

        //

        // pause   : 1 ~ 150

        // collect : 1 ~ 1500

        // 그 외   : 0

        size_t knnEvaluationPhaseFrameIndex{ 0u };



        // 현재 option의 마지막 network collection 1500번째 logical frame.

        // Option별 마지막 profile은 A~H의 profile sequence에 의해 결정된다.

        // WebRtcTransport가 이 frame의 feedback/CSV 기록을 끝낸 뒤

        // [KNN-EXPERIMENT-COMPLETE] 로그를 출력하는 데 사용.

        bool knnEvaluationTerminalFrame{ false };

    };



    struct SlackRuntimeActionState

    {

        bool fecEnabled{ false };

        uint8_t fecProtectionFactor{ 0u };

        bool pacingEnabled{ false };

    };



    enum class FrameOutcome : uint8_t

    {

        NORMAL = 0,

        LATE,

        PRE_DECODE_DROP

    };



    enum class KnnSampleOrigin : uint8_t

    {

        COLD_START = 0,

        ONLINE

    };



    // ============================================================

    // Historical kNN sample.

    // X_i = 과거 실제 실행 action의 feature

    // Y_i = feedback으로 받은 actual decode slack

    // ============================================================

    struct SlackSample

    {

        SlackFeature feature;

        SlackActionIdentity action;



        double decodeSlackMs{ 0.0 };

        bool deadlineMiss{ false };

        FrameOutcome outcome{ FrameOutcome::NORMAL };



        uint32_t frameId{ 0u };



        // Cold Start samples are retained permanently.

        // ONLINE samples are subject to FIFO aging.

        KnnSampleOrigin origin{ KnnSampleOrigin::ONLINE };

    };



    static constexpr size_t KnnDebugNeighborCount{ 5u };



    struct KnnNeighborDebugInfo

    {

        uint32_t frameId{ 0u };

        double distance{ 0.0 };

        SlackFeature feature;

        SlackActionIdentity action;

        double decodeSlackMs{ 0.0 };

        bool deadlineMiss{ false };

    };



    struct SlackPrediction

    {

        double predictedDecodeSlackMs{ 0.0 };

        double deadlineMissProbability{ 0.0 };

        size_t neighborCount{ 0u };



        size_t sampleCount{ 0u };

        size_t debugNeighborCount{ 0u };

        std::array<KnnNeighborDebugInfo, KnnDebugNeighborCount> debugNeighbors{};

    };



    class SlackPredictor

    {

    public:

        struct Config

        {

            size_t maxOnlineSamples{ 6000u };

            size_t k{ 7u };

            size_t minSamplesToPredict{ 2000u };

            size_t minSamplesPerSpatialLayer{ 200u };



            // Feature normalization scale.

            double rttScaleMs{ 100.0 };

            double lossScale{ 0.05 };

            double bandwidthScaleBps{ 5000000.0 };

            double congestionScale{ 1.0 };

            double frameSizeScaleBytes{ 20'000.0 };

            double pacingDelayScaleMs{ 50.0 };



            // Legacy action-distance terms. With action-conditioned prediction ON,

            // exact same-action filtering makes these terms all zero.

            double spatialLayerMismatchDistance{ 1.0 };

            double fecProtectionScale{ 128.0 };

            double pacingMismatchDistance{ 1.0 };

        };



        public:

            SlackPredictor();

            explicit SlackPredictor(const Config& config);



        public:

            bool AddSample(const SlackSample& sample);

            size_t GetSampleCount() const;

            size_t GetColdStartSampleCount() const;

            size_t GetOnlineSampleCount() const;

            bool CanPredict() const;



            std::optional<SlackPrediction> Predict(

              const SlackFeature& feature, const SlackActionIdentity& action) const;



            void Clear();



        private:

            struct Neighbor

            {

                double distance{ 0.0 };

                double decodeSlackMs{ 0.0 };

                bool deadlineMiss{ false };

                const SlackSample* sample{ nullptr };

            };



        private:

            double ComputeDistance(

              const SlackFeature& aFeature,

              const SlackActionIdentity& aAction,

              const SlackFeature& bFeature,

              const SlackActionIdentity& bAction) const;



            double NormalizeDifference(double a, double b, double scale) const;

            bool IsValidFeature(const SlackFeature& feature) const;

            bool CanPredictLocked() const;



            // Exact action identity comparison used by action-conditioned kNN.

            bool IsSameAction(const SlackActionIdentity& a, const SlackActionIdentity& b) const;



        private:

            Config config;

            mutable std::mutex mutex;



            // Cold Start samples are never aged out.

            std::deque<SlackSample> coldStartSamples;



            // Only KNN online samples are capped and FIFO-aged.

            std::deque<SlackSample> onlineSamples;



            std::array<size_t, 3u> spatialLayerSampleCounts{ 0u, 0u, 0u };

            bool initialSpatialCoverageSatisfied{ false };

        };

    } // namespace RTC
