#pragma once

#include <cstdint>
#include <mutex>

namespace RTC
{
	// yeon: pacer 상태를 가져와서 pacing delay 예측하기 위함
	struct PacerSnapshot
	{
		// decision 시점까지 refill되었다고 가정한 현재 유효 token.
		double effectiveTokensBytes{ 0.0 };

		// 현재 pacer FIFO에 대기 중인 전체 bytes.
		double queuedBytes{ 0.0 };

		// 1ms당 생성되는 token.
		double tokenRateBytesPerMs{ 0.0 };

		// token bucket 최대 용량.
		double bucketCapacityBytes{ 0.0 };

		uint64_t sampledAtMs{ 0u };
	};
	struct NetworkSnapshot
	{
		double rttMs{ 0.0 };
		double lossRate{ 0.0 }; // 0.0 ~ 1.0

		double twccDelayTrend{ 0.0 };
		double availableBitratebps{ 0.0 };

		// 추가
		uint32_t gccAvailableBitrateBps{ 0u };
		uint32_t camelAvailableBitrateBps{ 0u };

		double aceQueueBytes{ 0.0 };
		double pacingBacklogBytes{ 0.0 };
		double pacingBucketSizeBytes{ 0.0 }; // yeon: ACE/token bucket size
		double pacingTokenRateBytesPerMs{ 0.0 };

		uint64_t updatedAtMs{ 0 };

		// yeon: Camel burst length controller output.
		uint32_t camelBurstLengthBytes{ 0u };


		// Camel congestion detector output.
		//
		// S(D, inflight) ≈ ΔDelay / ΔInflight
		// unit: ms / KB
		double camelCongestionGradientMsPerKb{ 0.0 };
	};

	class NetworkState
	{
	public:
		NetworkState()  = default;
		~NetworkState() = default;

	public:
		void UpdateRttMs(double rttMs, uint64_t nowMs);
		void UpdateLossRate(double lossRate, uint64_t nowMs);
		void UpdateTwccDelayTrend(double trend, uint64_t nowMs);
		void UpdateAvailableBitrateBps(double bitrateBps, uint64_t nowMs);
		void UpdateAceQueueBytes(double queueBytes, uint64_t nowMs);
		void UpdatePacingBacklogBytes(double backlogBytes, uint64_t nowMs);

		NetworkSnapshot GetSnapshot() const;

	private:
		mutable std::mutex mutex;
		NetworkSnapshot snapshot;
	};
} // namespace RTC