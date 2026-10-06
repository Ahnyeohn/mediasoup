#include "RTC/FrameRecordCsvWriter.hpp"



#include <cmath>

#include <filesystem>

#include <iomanip>



namespace

{

    const char* FrameOutcomeToString(RTC::FrameOutcome outcome)

    {

        switch (outcome)

        {

            case RTC::FrameOutcome::NORMAL:

                return "NORMAL";



            case RTC::FrameOutcome::LATE:

                return "LATE";



            case RTC::FrameOutcome::PRE_DECODE_DROP:

                return "DROP";



            default:

                return "UNKNOWN";

        }

    }



    const char* ColdStartPhaseToString(RTC::KnnColdStartPhase phase)

    {

        switch (phase)

        {

            case RTC::KnnColdStartPhase::NONE:

                return "NONE";



            case RTC::KnnColdStartPhase::COLLECT:

                return "COLLECT";



            case RTC::KnnColdStartPhase::NETWORK_PAUSE:

                return "NETWORK_PAUSE";



            case RTC::KnnColdStartPhase::LAYER_PAUSE:

                return "LAYER_PAUSE";

        }



        return "UNKNOWN";

    }



    const char* KnnEvaluationPhaseToString(RTC::KnnEvaluationPhase phase)

    {

        switch (phase)

        {

            case RTC::KnnEvaluationPhase::NONE:

                return "NONE";



            case RTC::KnnEvaluationPhase::START_NETWORK_PAUSE:

                return "START_NETWORK_PAUSE";



            case RTC::KnnEvaluationPhase::COLLECT_NORMAL:

                return "COLLECT_NORMAL";



            case RTC::KnnEvaluationPhase::MID_NETWORK_PAUSE:

                return "MID_NETWORK_PAUSE";



            case RTC::KnnEvaluationPhase::COLLECT_6MBIT:

                return "COLLECT_6MBIT";



            case RTC::KnnEvaluationPhase::COLLECT_2MBIT:

                return "COLLECT_2MBIT";



            case RTC::KnnEvaluationPhase::COLLECT_4MBIT:

                return "COLLECT_4MBIT";



            case RTC::KnnEvaluationPhase::COLLECT_8MBIT:

                return "COLLECT_8MBIT";



            case RTC::KnnEvaluationPhase::COLLECT_LOSS_2PCT:

                return "COLLECT_LOSS_2PCT";



            case RTC::KnnEvaluationPhase::COLLECT_LOSS_5PCT:

                return "COLLECT_LOSS_5PCT";



            case RTC::KnnEvaluationPhase::COLLECT_LOSS_15PCT:

                return "COLLECT_LOSS_15PCT";



            case RTC::KnnEvaluationPhase::COMPLETE:

                return "COMPLETE";

        }



        return "UNKNOWN";

    }



    const char* ColdStartNetworkProfileToString(RTC::KnnColdStartNetworkProfile profile)

    {

        switch (profile)

        {

            case RTC::KnnColdStartNetworkProfile::NORMAL:

                return "NORMAL";



            case RTC::KnnColdStartNetworkProfile::LIMIT_6MBIT:

                return "6MBIT";



            case RTC::KnnColdStartNetworkProfile::LIMIT_2MBIT:

                return "2MBIT";



            case RTC::KnnColdStartNetworkProfile::LIMIT_4MBIT:

                return "4MBIT";



            case RTC::KnnColdStartNetworkProfile::LIMIT_8MBIT:

                return "8MBIT";



            case RTC::KnnColdStartNetworkProfile::LOSS_2PCT:

                return "LOSS_2PCT";



            case RTC::KnnColdStartNetworkProfile::LOSS_5PCT:

                return "LOSS_5PCT";



            case RTC::KnnColdStartNetworkProfile::LOSS_15PCT:

                return "LOSS_15PCT";

        }



        return "UNKNOWN";

    }

} // namespace



namespace RTC

{

    FrameRecordCsvWriter::FrameRecordCsvWriter(const std::string& filePath) : filePath(filePath)

    {

        this->out.open(this->filePath, std::ios::out | std::ios::app);



        WriteHeaderIfNeeded();

    }



    FrameRecordCsvWriter::~FrameRecordCsvWriter()

    {

        if (this->out.is_open())

        {

            this->out.flush();

            this->out.close();

        }

    }



    void FrameRecordCsvWriter::WriteHeaderIfNeeded()

    {

        if (!this->out.is_open() || this->headerWritten)

        {

            return;

        }



        const bool fileExists = std::filesystem::exists(this->filePath);



        const auto fileSize = fileExists ? std::filesystem::file_size(this->filePath) : 0;



        if (fileSize == 0)

        {

            this->out



              // =====================================================

              // 1. Frame identification.

              // =====================================================



              << "transportId,"

              << "consumerId,"

              << "producerId,"

              << "frameId,"

              << "logicalFrameId,"

              << "isKeyFrame,"



              << "incomingPictureId,"

              << "outgoingPictureId,"

              << "vp8HasTl0PictureIndex,"

              << "pictureIdSyncApplied,"

              << "pictureIdRewriteApplied,"



              // =====================================================

              // 2. Decision-time kNN input.

              // =====================================================



              << "decisionRttMs,"

              << "decisionLossRate,"

              << "decisionAvailableBitrateBps,"

              << "decisionCongestion,"

              << "decisionFrameSizeBytes,"

              << "decisionPacingDelayMs,"



              // =====================================================

              // 3. Decision-time ACE / Pacer state.

              // =====================================================



              << "decisionAceQueueBytes,"

              << "decisionPacingBacklogBytes,"

              << "decisionPacingBucketSizeBytes,"

              << "decisionTokenRateBytesPerMs,"



              // =====================================================

              // 4. Selected action.

              // =====================================================



              << "decisionMode,"



              << "coldStartPhase,"

              << "coldStartNetworkProfile,"

              << "coldStartTargetNetworkProfile,"

              << "coldStartPauseFrameIndex,"



              // KNN evaluation.

              << "knnEvaluationPhase,"

              << "knnEvaluationNetworkProfile,"

              << "knnEvaluationTargetNetworkProfile,"

              << "knnEvaluationPhaseFrameIndex,"

              << "knnEvaluationTerminalFrame,"



              // Cold/KNN 공통.

              << "knnTrainingEligible,"



              << "decisionSpatialLayer,"



              << "decisionFecProtectionFactor,"

              << "decisionFecRedundancyPercent,"

              << "decisionPacing,"

              << "decisionPredictedDecodeSlackMs,"

              << "decisionPredictedDeadlineMissProbability,"

              << "predictionErrorMs,"



              << "actualActionPredictedDecodeSlackMs,"

              << "actualActionPredictionErrorMs,"

              << "actualActionPredictedDeadlineMissProbability,"



              // =====================================================

              // 5. Actual action.

              // =====================================================



              << "actualSpatialLayer,"

              << "actualFecProtectionFactor,"

              << "actualFecRedundancyPercent,"

              << "actualPacing,"



              // 기존 layer state 유지.

              << "currentSpatialLayer,"

              << "targetSpatialLayer,"

              << "preferredSpatialLayer,"



              // =====================================================

              // 6. Actual send-time frame / network state.

              // =====================================================



              << "sendFrameSizeBytes,"

              << "sendPacketCount,"



              << "sendRttMs,"

              << "sendLossRate,"

              << "sendAvailableBitrateBps,"

              << "sendGccAvailableBitrateBps,"

              << "sendCamelAvailableBitrateBps,"

              << "sendCongestion,"



              // =====================================================

              // 7. Actual send-time ACE / Pacer state.

              // =====================================================



              << "sendAceQueueBytes,"

              << "sendPacingBacklogBytes,"

              << "sendPacingBucketSizeBytes,"

              << "sendTokenRateBytesPerMs,"

              << "sendCamelBurstLengthBytes,"



              // =====================================================

              // 8. Browser telemetry.

              // 기존 정보 유지.

              // =====================================================



              << "receiveTimeMs,"

              << "latestDecodeTimeMs,"

              << "frameBufferInsertTimeMs,"

              << "frameBufferExtractTimeMs,"

              << "decodeQueueInsertTimeMs,"

              << "decodeQueueExtractTimeMs,"



              << "now,"

              << "render_time,"

              << "max_wait,"



              << "decodeStartMs,"

              << "decodeFinishMs,"

              << "desiredReceiveTimeMs,"

              << "desiredDecodeStartMs,"



              << "actualSlackMs,"

              << "actualSlackEffectiveMs,"

              << "receiveSlackMs,"

              << "decodeSlackNominalMs,"

              << "rtpTimestampRewriteOffsetMs,"

              << "decodeSlackLogicalMs,"

              << "frameBufferResidenceMs,"



              << "decodeQueueResidenceMs,"

              << "decodeSlackEffectiveMs,"



              << "actualDeadlineMiss,"

              << "frameOutcome"



              << "\n";



            this->out.flush();

        }



        this->headerWritten = true;

    }



    void FrameRecordCsvWriter::WriteRecord(const FrameRecord& record)

    {

        std::lock_guard<std::mutex> lock(this->mutex);



        if (!this->out.is_open())

        {

            return;

        }



        WriteHeaderIfNeeded();



        const bool hasDecision = record.hasSlackActionDecision;



        const RTC::SlackActionDecision* decision = hasDecision ? &record.slackActionDecision : nullptr;



        const bool isColdStartDecision = decision && !decision->modelSelected;



        const bool isKnnEvaluationDecision =

          decision && decision->modelSelected &&

          decision->knnEvaluationPhase != RTC::KnnEvaluationPhase::NONE;



        const bool hasDecisionPrediction = decision && decision->hasPredictedDecodeSlack;



        const uint8_t actualFecPf = record.hasFecRedundancy ? record.fecProtectionFactor : 0u;



        const int16_t actualLayer = record.currentSpatialLayer;



        // ========================================================

        // 1. 기존 방식:

        //    KNN selected action == actual action 인지 확인.

        // ========================================================



        const bool layerMatches = decision && actualLayer == static_cast<int16_t>(decision->spatialLayer);



        const bool fecMatches = decision && actualFecPf == decision->fecProtectionFactor;



        const bool pacingMatches = decision && record.pacingEnabled == decision->pacingEnabled;



        const bool actionMatched = decision && layerMatches && fecMatches && pacingMatches;



        // ========================================================

        // 2. 기존 predictionErrorMs.

        //

        // selected action과 actual action이 완전히 같은 경우에만

        // 기존 selected-action prediction과 actual을 비교.

        // ========================================================



        const bool hasPredictionError = decision && decision->hasPredictedDecodeSlack &&

                                        record.hasDecodeSlackLogicalMs && actionMatched;



        double predictionErrorMs{ 0.0 };



        if (hasPredictionError)

        {

            predictionErrorMs = record.decodeSlackLogicalMs - decision->predictedDecodeSlackMs;

        }



        // const bool hasPredictionError = decision && decision->hasPredictedDecodeSlack &&

        //                                 record.hasDecodeSlackNominalMs && actionMatched;



        // double predictionErrorMs{ 0.0 };

        // if (hasPredictionError)

        // {

        //  predictionErrorMs = record.decodeSlackNominalMs - decision->predictedDecodeSlackMs;

        // }



        // ========================================================

        // 3. 신규:

        //    실제 실행된 action에 대응하는 prediction.

        //

        // KNN decision 시점에 selected FEC/Pacing 조합에 대해

        // L0/L1/L2 prediction을 이미 모두 저장해 두었으므로,

        // selected layer != actual layer 인 경우에도

        // actual layer prediction을 가져올 수 있다.

        // ========================================================



        bool hasActualActionPredictedDecodeSlack{ false };

        double actualActionPredictedDecodeSlackMs{ 0.0 };



        bool hasActualActionPredictedDeadlineMissProbability{ false };

        double actualActionPredictedDeadlineMissProbability{ 0.0 };



        const bool validActualLayer =

          decision && actualLayer >= 0 &&

          static_cast<size_t>(actualLayer) < decision->hasPredictedDecodeSlackBySpatialLayer.size();



        // per-layer prediction은 selected FEC/Pacing에 대해서 저장되어 있으므로,

        // 실제 FEC/Pacing도 selected와 같을 때만 사용할 수 있다.

        if (decision && decision->modelSelected && validActualLayer && fecMatches && pacingMatches)

        {

            const size_t layer = static_cast<size_t>(actualLayer);



            if (decision->hasPredictedDecodeSlackBySpatialLayer[layer])

            {

                hasActualActionPredictedDecodeSlack = true;



                actualActionPredictedDecodeSlackMs = decision->predictedDecodeSlackBySpatialLayer[layer];

            }



            if (decision->hasPredictedDeadlineMissProbabilityBySpatialLayer[layer])

            {

                hasActualActionPredictedDeadlineMissProbability = true;



                actualActionPredictedDeadlineMissProbability =

                  decision->predictedDeadlineMissProbabilityBySpatialLayer[layer];

            }

        }



        // ========================================================

        // 4. 신규:

        //    actual action 기준 prediction error.

        //

        // error = actual - predicted(actual action)

        // ========================================================



        const bool hasActualActionPredictionError =

          hasActualActionPredictedDecodeSlack && record.hasDecodeSlackLogicalMs;



        double actualActionPredictionErrorMs{ 0.0 };



        if (hasActualActionPredictionError)

        {

            actualActionPredictionErrorMs =

              record.decodeSlackLogicalMs - actualActionPredictedDecodeSlackMs;

        }



        // const bool hasActualActionPredictionError =

        //   hasActualActionPredictedDecodeSlack && record.hasDecodeSlackNominalMs;



        // double actualActionPredictionErrorMs{ 0.0 };

        // if (hasActualActionPredictionError)

        // {

        //  actualActionPredictionErrorMs =

        //    record.decodeSlackNominalMs - actualActionPredictedDecodeSlackMs;

        // }



        // 기존 selected-action prediction 출력용.

        const bool hasPredictedDecodeSlack = decision && decision->hasPredictedDecodeSlack;



        const bool hasPredictedDeadlineMissProbability =

          decision && decision->hasPredictedDeadlineMissProbability;



        const double actualSlack = record.hasSlack ? record.slackMs : NAN;



        this->out



          // =====================================================

          // 1. Frame identification.

          // =====================================================



          << record.transportId << "," << record.consumerId << "," << record.producerId << ","



          << record.frameId << ","



          << (record.hasLogicalFrameId ? std::to_string(record.logicalFrameId)

                                       : (decision ? std::to_string(decision->logicalFrameId) : ""))

          << ","



          << (record.isKeyFrame ? 1 : 0) << ","



          << (record.hasVp8PictureIdTrace && record.hasIncomingPictureId

                ? std::to_string(record.incomingPictureId)

                        : "")

          << ","



          << (record.hasVp8PictureIdTrace && record.hasOutgoingPictureId

                ? std::to_string(record.outgoingPictureId)

                        : "")

          << ","



          << (record.hasVp8PictureIdTrace ? (record.vp8HasTl0PictureIndex ? "1" : "0") : "") << ","



          << (record.hasVp8PictureIdTrace ? (record.pictureIdSyncApplied ? "1" : "0") : "") << ","



          << (record.hasVp8PictureIdTrace ? (record.pictureIdRewriteApplied ? "1" : "0") : "")

          << ","



          // =====================================================

          // 2. Decision-time kNN input.

          // =====================================================



          << (decision ? std::to_string(decision->feature.rttMs) : "") << ","



          << (decision ? std::to_string(decision->feature.lossRate) : "") << ","



          << (decision ? std::to_string(decision->feature.availableBitrateBps) : "") << ","



          << (decision ? std::to_string(decision->feature.congestion) : "") << ","



          << (decision ? std::to_string(decision->feature.frameSizeBytes) : "") << ","



          << (decision ? std::to_string(decision->feature.pacingDelayMs) : "")

          << ","



          // =====================================================

          // 3. Decision-time ACE / Pacer state.

          // =====================================================



          << (decision ? std::to_string(decision->aceQueueBytes) : "") << ","



          << (decision ? std::to_string(decision->pacingBacklogBytes) : "") << ","



          << (decision ? std::to_string(decision->pacingBucketSizeBytes) : "") << ","



          << (decision ? std::to_string(decision->tokenRateBytesPerMs) : "")

          << ","



          // =====================================================

          // 4. Selected action.

          // =====================================================



          << (decision ? (decision->modelSelected ? "KNN" : "COLD") : "")

          << ","



          // =====================================================

          // Cold Start metadata.

          // =====================================================



          << (isColdStartDecision ? ColdStartPhaseToString(decision->coldStartPhase) : "") << ","



          << (isColdStartDecision ? ColdStartNetworkProfileToString(decision->coldStartNetworkProfile)

                                  : "")

          << ","



          << (isColdStartDecision

                ? ColdStartNetworkProfileToString(decision->coldStartTargetNetworkProfile)

                        : "")

          << ","



          << (isColdStartDecision ? std::to_string(decision->coldStartPauseFrameIndex) : "")

          << ","



          // =====================================================

          // KNN evaluation metadata.

          // =====================================================



          << (isKnnEvaluationDecision ? KnnEvaluationPhaseToString(decision->knnEvaluationPhase) : "")

          << ","



          << (isKnnEvaluationDecision

                ? ColdStartNetworkProfileToString(decision->knnEvaluationNetworkProfile)

                        : "")

          << ","



          << (isKnnEvaluationDecision

                ? ColdStartNetworkProfileToString(decision->knnEvaluationTargetNetworkProfile)

                        : "")

          << ","



          << (isKnnEvaluationDecision ? std::to_string(decision->knnEvaluationPhaseFrameIndex) : "")

          << ","



          << (isKnnEvaluationDecision ? (decision->knnEvaluationTerminalFrame ? "1" : "0") : "")

          << ","



          // =====================================================

          // Training eligibility.

          //

          // Cold Start / KNN 공통.

          // =====================================================



          << (decision ? (decision->skipKnnTrainingSample ? "0" : "1") : "") << ","



          << (decision ? std::to_string(static_cast<unsigned int>(decision->spatialLayer)) : "") << ","



          << (decision ? std::to_string(static_cast<unsigned int>(decision->fecProtectionFactor)) : "")

          << ","



          << (decision ? std::to_string(decision->fecRedundancyRate * 100.0) : "") << ","



          << (decision ? (decision->pacingEnabled ? "1" : "0") : "")

          << ","



          // Selected action의 predicted Slack.

          << (hasDecisionPrediction ? std::to_string(decision->predictedDecodeSlackMs) : "")

          << ","



          // Selected action의 predicted deadline miss probability.

          << (hasPredictedDeadlineMissProbability

                ? std::to_string(decision->predictedDeadlineMissProbability)

                        : "")

          << ","



          // Prediction error = actual - predicted.

          << (hasPredictionError ? std::to_string(predictionErrorMs) : "")

          << ","



          // =====================================================

          // yeon: actual executed action 기준 KNN prediction.

          // =====================================================



          << (hasActualActionPredictedDecodeSlack ? std::to_string(actualActionPredictedDecodeSlackMs)

                                                  : "")

          << ","



          << (hasActualActionPredictionError ? std::to_string(actualActionPredictionErrorMs) : "") << ","



          << (hasActualActionPredictedDeadlineMissProbability

                ? std::to_string(actualActionPredictedDeadlineMissProbability)

                        : "")

          << ","



          // =====================================================

          // 5. Actual action.

          // =====================================================



          << record.currentSpatialLayer << ","



          << (record.hasFecRedundancy

                ? std::to_string(static_cast<unsigned int>(record.fecProtectionFactor))

                        : "")

          << ","



          << (record.hasFecRedundancy ? std::to_string(record.fecRedundancyPercent) : "") << ","



          << (record.pacingEnabled ? 1 : 0)

          << ","



          // =====================================================

          // 기존 layer state.

          // =====================================================



          << record.currentSpatialLayer << ","



          << record.targetSpatialLayer << ","



          << record.preferredSpatialLayer

          << ","



          // =====================================================

          // 6. Actual WebRtcTransport send-time state.

          // =====================================================



          << record.frameSizeBytes << ","



          << record.packetCount << ","



          << record.network.rttMs << ","



          << record.network.lossRate << ","



          << record.network.availableBitratebps << ","



          << record.network.gccAvailableBitrateBps << ","



          << record.network.camelAvailableBitrateBps << ","



          << record.network.camelCongestionGradientMsPerKb

          << ","



          // =====================================================

          // 7. Actual send-time ACE / Pacer.

          // =====================================================



          << record.network.aceQueueBytes << ","



          << record.network.pacingBacklogBytes << ","



          << record.network.pacingBucketSizeBytes << ","



          << record.network.pacingTokenRateBytesPerMs << ","



          << record.network.camelBurstLengthBytes

          << ","



          // =====================================================

          // 8. Browser telemetry.

          // =====================================================



          << (record.hasReceiveTimeMs ? std::to_string(record.receiveTimeMs) : "") << ","



          << (record.hasLatestDecodeTimeMs ? std::to_string(record.latestDecodeTimeMs) : "") << ","



          << (record.hasFrameBufferInsertTimeMs ? std::to_string(record.frameBufferInsertTimeMs) : "")

          << ","



          << (record.hasFrameBufferExtractTimeMs ? std::to_string(record.frameBufferExtractTimeMs) : "")

          << ","



          << (record.hasDecodeQueueInsertTimeMs ? std::to_string(record.decodeQueueInsertTimeMs) : "")

          << ","



          << (record.hasDecodeQueueExtractTimeMs ? std::to_string(record.decodeQueueExtractTimeMs) : "")

          << ","



          << (record.hasnow ? std::to_string(record.now) : "") << ","



          << (record.hasrender_time ? std::to_string(record.render_time) : "") << ","



          << (record.hasmax_wait ? std::to_string(record.max_wait) : "") << ","



          << (record.hasDecodeStartMs ? std::to_string(record.decodeStartMs) : "") << ","



          << (record.hasDecodeFinishMs ? std::to_string(record.decodeFinishMs) : "") << ","



          << (record.hasDesiredReceiveTimeMs ? std::to_string(record.desiredReceiveTimeMs) : "") << ","



          << (record.hasDesiredDecodeStartMs ? std::to_string(record.desiredDecodeStartMs) : "") << ","



          << actualSlack << ","



          << (record.hasActualSlackEffectiveMs ? std::to_string(record.actualSlackEffectiveMs) : "")

          << ","



          << (record.hasReceiveSlackMs ? std::to_string(record.receiveSlackMs) : "") << ","



          << (record.hasDecodeSlackNominalMs ? std::to_string(record.decodeSlackNominalMs) : "") << ","



          << (record.hasRtpTimestampRewriteOffsetMs ? std::to_string(record.rtpTimestampRewriteOffsetMs)

                                                    : "")

          << ","



          << (record.hasDecodeSlackLogicalMs ? std::to_string(record.decodeSlackLogicalMs) : "") << ","



          << (record.hasFrameBufferResidenceMs ? std::to_string(record.frameBufferResidenceMs) : "")

          << ","



          << (record.hasDecodeQueueResidenceMs ? std::to_string(record.decodeQueueResidenceMs) : "")

          << ","



          << (record.hasDecodeSlackEffectiveMs ? std::to_string(record.decodeSlackEffectiveMs) : "")

          << ","



          << (record.hasFrameOutcome ? (record.deadlineMiss ? "1" : "0") : "") << ","



          << (record.hasFrameOutcome ? FrameOutcomeToString(record.frameOutcome) : "")



          << "\n";



        this->out.flush();

    }

} // namespace RTC