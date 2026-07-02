#define MS_CLASS "RTC::WebRtcTransport"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/WebRtcTransport.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "QosLogger.hpp"
#include "Settings.hpp"
#include "Utils.hpp"
#include "FBS/webRtcTransport.h"
// TODO: For testing purposes. Must be removed.
#ifdef MS_SCTP_STACK
#include "RTC/SCTP/packet/Packet.hpp"
#endif
#include <arpa/inet.h> // ntohs, ntohl
#include <cmath>       // std::pow()
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

// yeon: sctp 파싱 및 packet type에 따른 분기 처리
#include <optional>

// yeon: fps 문제 해결을 위한 코드
#include <array>
#include <unordered_map>

// yeon: pacing 동작 확인을 위한 코드
#include <chrono>
#include <filesystem>
#include <fstream>

// yeon: deadline slack을 위한 코드
#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>

// yeon: ispacing을 런타임에 바꾸기 위함
#include <atomic>

#include <memory>

// ===== pacing 여부 ====
// static bool ispacing = false;
static bool ispacingg = false;
static std::atomic_bool ispacing{ false }; // 처음은 off
static std::atomic_bool pacingAutoSwitchDone{ false };
static uint64_t pacingExperimentStartMs{ 0 };
// static constexpr uint64_t PacingSwitchAfterMs = 50000; //
static constexpr uint64_t PacingSwitchAfterMs = 5000000; //
// ===== 패킷 send 분포 로그 ====
static bool packetsendlog = false;

// ===== bucket size, queue size 로그 저장 =====
static bool pacinglog            = false;
static const std::string logPath = "/home/n2sl/yeon/qos/network/log/pacing/ace_bucket_log.csv";
static bool headerWritten        = false;

// 추가: rtp 페이로드 분석
static constexpr uint8_t MAGIC_BE[4] = { 0x4c, 0x41, 0x54, 0x4e }; // "LATN"
static constexpr uint8_t MAGIC_LE[4] = { 0x4e, 0x54, 0x41, 0x4c }; // "NTAL" (JS little-endian로 쓴 경우)
static constexpr size_t LAT_HEADER_LEN = 32;

static inline uint16_t ReadBE16(const uint8_t* p)
{
	uint16_t v;
	std::memcpy(&v, p, sizeof(v));
	return ntohs(v);
}

static inline uint32_t ReadBE32(const uint8_t* p)
{
	uint32_t v;
	std::memcpy(&v, p, sizeof(v));
	return ntohl(v);
}

static std::string HexDump(const uint8_t* p, size_t n, size_t maxBytes = 64)
{
	std::ostringstream oss;
	const size_t m = (n < maxBytes) ? n : maxBytes;
	for (size_t i = 0; i < m; ++i)
	{
		if (i)
		{
			oss << ' ';
		}
		oss << std::hex << std::setw(2) << std::setfill('0') << (int)p[i];
	}
	if (n > maxBytes)
	{
		oss << " ...";
	}
	return oss.str();
}

// -------------------------------------------------------------------
// Monotonic time on SFU (ms)
// -------------------------------------------------------------------
static inline double GetMonotonicTimeMs()
{
	using namespace std::chrono;

	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// -------------------------------------------------------------------
// Very small JSON-like field extractors.
// NOTE:
// This is intentionally lightweight to fit your current style.
// If you already use a JSON library in your project, replace these
// helpers with proper JSON parsing.
// -------------------------------------------------------------------
static bool ExtractJsonString(const std::string& text, const std::string& key, std::string& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = text.find(needle);

	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find(':', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find('"', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	size_t end = text.find('"', pos + 1);
	if (end == std::string::npos)
	{
		return false;
	}

	out = text.substr(pos + 1, end - pos - 1);
	return true;
}

static bool ExtractJsonUint32(const std::string& text, const std::string& key, uint32_t& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = text.find(needle);

	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find(':', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	char* endPtr        = nullptr;
	const char* start   = text.c_str() + pos + 1;
	unsigned long value = std::strtoul(start, &endPtr, 10);

	if (start == endPtr)
	{
		return false;
	}

	out = static_cast<uint32_t>(value);
	return true;
}

static bool ExtractJsonInt64(const std::string& text, const std::string& key, int64_t& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = text.find(needle);

	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find(':', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	char* endPtr      = nullptr;
	const char* start = text.c_str() + pos + 1;
	long long value   = std::strtoll(start, &endPtr, 10);

	if (start == endPtr)
	{
		return false;
	}

	out = static_cast<int64_t>(value);
	return true;
}

static bool ExtractJsonUint64(const std::string& text, const std::string& key, uint64_t& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = text.find(needle);

	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find(':', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	char* endPtr             = nullptr;
	const char* start        = text.c_str() + pos + 1;
	unsigned long long value = std::strtoull(start, &endPtr, 10);

	if (start == endPtr)
	{
		return false;
	}

	out = static_cast<uint64_t>(value);
	return true;
}

static bool ExtractJsonDouble(const std::string& text, const std::string& key, double& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = text.find(needle);

	if (pos == std::string::npos)
	{
		return false;
	}

	pos = text.find(':', pos);
	if (pos == std::string::npos)
	{
		return false;
	}

	char* endPtr      = nullptr;
	const char* start = text.c_str() + pos + 1;
	double value      = std::strtod(start, &endPtr);

	if (start == endPtr)
	{
		return false;
	}

	out = value;
	return true;
}

static bool ExtractLatestDecodeTimingBundle(
  const std::string& text,
  int64_t& latestDecodeTimeMs,
  int64_t& now,
  int64_t& render_time,
  int64_t& max_wait)
{
	latestDecodeTimeMs = 0;
	now                = 0;
	render_time        = 0;
	max_wait           = 0;

	const std::string key = "\"latestDecodeTimeMs\"";
	size_t keyPos         = text.find(key);
	if (keyPos == std::string::npos)
	{
		return false;
	}

	size_t arrayStart = text.find('[', keyPos);
	if (arrayStart == std::string::npos)
	{
		return false;
	}

	size_t arrayEnd = text.find(']', arrayStart);
	if (arrayEnd == std::string::npos)
	{
		return false;
	}

	// 빈 배열 [] 허용
	size_t objStart = text.find('{', arrayStart);
	if (objStart == std::string::npos || objStart > arrayEnd)
	{
		return true;
	}

	size_t objEnd = text.find('}', objStart);
	if (objEnd == std::string::npos || objEnd > arrayEnd)
	{
		return false;
	}

	const std::string objText = text.substr(objStart, objEnd - objStart + 1);

	if (!ExtractJsonInt64(objText, "latest_decode_time", latestDecodeTimeMs))
	{
		return false;
	}

	if (!ExtractJsonInt64(objText, "now", now))
	{
		return false;
	}

	if (!ExtractJsonInt64(objText, "render_time", render_time))
	{
		return false;
	}

	if (!ExtractJsonInt64(objText, "max_wait", max_wait))
	{
		return false;
	}

	return true;
}

static bool ExtractPacketReceiveTimes(const std::string& text, std::vector<RTC::PacketReceiveInfo>& out)
{
	out.clear();

	const std::string key = "\"packetReceiveTimes\"";
	size_t keyPos         = text.find(key);
	if (keyPos == std::string::npos)
	{
		return false;
	}

	size_t arrayStart = text.find('[', keyPos);
	if (arrayStart == std::string::npos)
	{
		return false;
	}

	size_t arrayEnd = text.find(']', arrayStart);
	if (arrayEnd == std::string::npos || arrayEnd <= arrayStart)
	{
		return false;
	}

	size_t pos = arrayStart + 1;

	while (pos < arrayEnd)
	{
		size_t objStart = text.find('{', pos);
		if (objStart == std::string::npos || objStart >= arrayEnd)
		{
			break;
		}

		size_t objEnd = text.find('}', objStart);
		if (objEnd == std::string::npos || objEnd > arrayEnd)
		{
			return false;
		}

		const std::string objText = text.substr(objStart, objEnd - objStart + 1);

		uint64_t seq64{ 0 };
		uint64_t recvMs{ 0 };

		if (ExtractJsonUint64(objText, "sequenceNumber", seq64) && ExtractJsonUint64(objText, "receiveTimeMs", recvMs))
		{
			RTC::PacketReceiveInfo info;
			info.sequenceNumber = static_cast<uint16_t>(seq64);
			info.receiveTimeMs  = recvMs;
			out.emplace_back(info);
		}

		pos = objEnd + 1;
	}

	return true;
}

static AppMessageKind DetectAppMessageKind(const std::string& text)
{
	std::string type;

	if (!ExtractJsonString(text, "type", type))
	{
		return AppMessageKind::Unknown;
	}

	if (type == "latency")
	{
		return AppMessageKind::Latency;
	}
	else if (type == "sync_req")
	{
		return AppMessageKind::SyncReq;
	}
	else if (type == "sync_resp")
	{
		return AppMessageKind::SyncResp;
	}
	else if (type == "chat")
	{
		return AppMessageKind::Chat;
	}

	return AppMessageKind::Unknown;
}

/**
 * SCTP packet을 파싱하는 함수 (common header + chunks)
 * OnDtlsTransportApplicationDataReceived(data,len) 내부에서 호출
 */
/**
 * SCTP packet을 파싱해서 app-level message 하나를 반환.
 * 현재는 첫 번째 TEXT DATA chunk만 대상으로 처리.
 */
static std::optional<ParsedSctpAppMessage> ParseSctpData(const uint8_t* data, size_t len)
{
	if (!data)
	{
		MS_ERROR_STD("[SCTPDBG] data is null -> return");
		return std::nullopt;
	}

	// SCTP common header is 12 bytes.
	if (len < 12)
	{
		MS_ERROR_STD("[SCTPDBG] len < 12 (%zu) -> return (not SCTP or truncated)", len);
		MS_ERROR_STD("[SCTPDBG] first bytes: %s", HexDump(data, len).c_str());
		return std::nullopt;
	}

	const uint16_t srcPort = ReadBE16(data + 0);
	const uint16_t dstPort = ReadBE16(data + 2);
	const uint32_t vtag    = ReadBE32(data + 4);
	const uint32_t csum    = ReadBE32(data + 8);

	MS_TRACE();

	size_t off     = 12;
	int chunkIndex = 0;

	while (off + 4 <= len)
	{
		const uint8_t chunkType  = data[off + 0];
		const uint8_t chunkFlags = data[off + 1];
		const uint16_t chunkLen  = ReadBE16(data + off + 2);

		if (chunkLen < 4)
		{
			MS_ERROR_STD("[SCTPDBG] chunkLen < 4 -> break (corrupted?)");
			break;
		}

		if (off + chunkLen > len)
		{
			MS_ERROR_STD(
			  "[SCTPDBG] off+chunkLen(%zu) > len(%zu) -> break (truncated?)", off + chunkLen, len);
			MS_ERROR_STD("[SCTPDBG] chunk header bytes: %s", HexDump(data + off, (len - off)).c_str());
			break;
		}

		const uint8_t* chunk = data + off;

		// DATA chunk type=0
		if (chunkType == 0)
		{
			if (chunkLen < 16)
			{
				MS_ERROR_STD("[SCTPDBG] DATA chunkLen < 16 (%u) -> skip", chunkLen);
			}
			else
			{
				const uint32_t tsn  = ReadBE32(chunk + 4);
				const uint16_t sid  = ReadBE16(chunk + 8);
				const uint16_t ssn  = ReadBE16(chunk + 10);
				const uint32_t ppid = ReadBE32(chunk + 12);

				const uint8_t* user = chunk + 16;
				const size_t ulen   = chunkLen - 16;

				// TEXT / empty text
				if (ppid == 51 || ppid == 56)
				{
					std::string text(reinterpret_cast<const char*>(user), ulen);

					ParsedSctpAppMessage parsed;
					parsed.sid   = sid;
					parsed.ssn   = ssn;
					parsed.ppid  = ppid;
					parsed.tsn   = tsn;
					parsed.text  = text;
					parsed.kind  = DetectAppMessageKind(text);
					parsed.valid = true;

					switch (parsed.kind)
					{
						case AppMessageKind::Latency:
						{
							ExtractJsonDouble(text, "s2cMs", parsed.s2cMs);
							break;
						}

						case AppMessageKind::SyncReq:
						{
							ExtractJsonUint32(text, "seq", parsed.seq);
							ExtractJsonDouble(text, "t1ViewMs", parsed.t1ViewMs);
							break;
						}

						case AppMessageKind::SyncResp:
						{
							ExtractJsonUint32(text, "seq", parsed.seq);
							ExtractJsonDouble(text, "t2SfuMs", parsed.t2SfuMs);
							break;
						}

						case AppMessageKind::Chat:
						case AppMessageKind::Unknown:
						case AppMessageKind::None:
						default:
						{
							break;
						}
					}

					return parsed;
				}
				else
				{
					// binary payload
					// MS_ERROR_STD("[SCTPDBG] BIN hexdump=%s", HexDump(user, ulen).c_str());
				}
			}
		}

		// 4-byte alignment
		size_t adv       = chunkLen;
		const size_t pad = (4 - (adv % 4)) % 4;
		adv += pad;

		off += adv;
		chunkIndex++;
	}

	return std::nullopt;
}

static inline double NowEpochMs()
{
	using namespace std::chrono;
	return duration<double, std::milli>(system_clock::now().time_since_epoch()).count();
}

static inline uint32_t ReadU32LE(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline double ReadF64LE(const uint8_t* p)
{
	// prefix를 little-endian uint64로 조립
	uint64_t u = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
	             ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
	             ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);

	double d;
	static_assert(sizeof(double) == sizeof(uint64_t));
	std::memcpy(&d, &u, sizeof(double));
	return d;
}

// 이 함수에서 SFUrecvMs를 페이로드에 write 해야 함
// 지금 비디오 패킷은 MAGIC 값을 못읽는게 문제임
bool ParseFramePrefixforReceive(uint8_t* payload, size_t payloadLen, RTC::RtpPacket::RtpPrefix* pfx)
{
	if (!pfx)
	{
		return false;
	}
	if (!payload || payloadLen < LAT_HEADER_LEN)
	{
		return false;
	}

	// STAMP 확인: vp8 descriptor 이슈로 4바이트를 띄워야 함 : 즉, 구조: RTP 헤더 + vp8 codec
	// descriptor(4) + rtp prefix (여기에 stamp가 맨앞에서 4바이트)..... 즉, 아래 모든 payload 기반
	// 포인터 연산에서 4를 추가했음
	if (std::memcmp(payload + 4, MAGIC_LE, 4) != 0)
	{
		return false;
	}
	const uint32_t stamp = ReadU32LE(payload + 4);

	// 수신 시점 측정
	const double SFUrecvMs = NowEpochMs();

	// FrameID 읽기
	const uint32_t frameID = ReadU32LE(payload + 8);

	// sendTsMs 읽기 (offset 8)
	const double sendTsMs = ReadF64LE(payload + 12);
	if (!std::isfinite(sendTsMs))
	{
		return false;
	}

	// 여기서 prefix에 기록 (offset 16)
	WriteF64LE(payload + 20, SFUrecvMs);

	// p2s latency 측정
	const double p2s = SFUrecvMs - sendTsMs;

	pfx->Stamp     = stamp;
	pfx->FrameID   = frameID;
	pfx->sendTsMs  = sendTsMs;
	pfx->SFUrecvMs = SFUrecvMs;
	pfx->p2s       = p2s;
	return true;
}

bool ParseFramePrefixforSend(uint8_t* payload, size_t payloadLen, RTC::RtpPacket::RtpPrefix* pfx)
{
	if (!pfx)
	{
		// MS_ERROR_STD("prefix problem");
		return false;
	}
	if (!payload || payloadLen < LAT_HEADER_LEN)
	{
		// MS_ERROR_STD("payload problem");
		return false;
	}

	// STAMP 확인
	if (std::memcmp(payload + 4, MAGIC_LE, 4) != 0)
	{
		// MS_ERROR_STD("STAMP problem");
		return false;
	}

	// 송신 시점 측정
	const double SFUsendMs = NowEpochMs();

	pfx->SFUsendMs = SFUsendMs;
	WriteF64LE(payload + 28, SFUsendMs);

	const double testSFUsendMs = ReadF64LE(payload + 28);
	// MS_ERROR_STD("[test]: SFUsendMs=%.3f", testSFUsendMs);
	return true;
}

bool HasMagicPrefix(const uint8_t* payload, size_t payloadLen)
{
	if (!payload)
	{
		// MS_ERROR_STD("no payload data");
		return false;
	}
	else if (payloadLen < 4)
	{
		// MS_ERROR_STD("payload length < 4");
		return false;
	}

	// JS에서 big-endian으로 썼다면:
	if (std::memcmp(payload, MAGIC_BE, 4) == 0)
	{
		return true;
	}
	else if (std::memcmp(payload, MAGIC_LE, 4) == 0)
	{
		return true;
	}
	// JS에서 little-endian(true)로 썼다면:
	// if (std::memcmp(payload, MAGIC_LE, 4) == 0) return true;
	// MS_ERROR_STD("No Stamp");
	return false;
}
// 추가: rtp 페이로드 분석

// ====== latency prefix를 읽기 위한 vp8 descriptor parser
struct Vp8PrefixLocateResult
{
	bool isVp8FirstPacket{ false }; // S=1 && PID=0
	bool foundMagic{ false };
	size_t magicOffset{ 0 };      // offset from payload start
	size_t vp8DescriptorLen{ 0 }; // parsed VP8 descriptor length
};

// Parse VP8 payload descriptor length.
// Returns true on success.
static bool ParseVp8PayloadDescriptor(
  const uint8_t* payload, size_t payloadLen, size_t& descLen, bool& isFrameStartPacket)
{
	descLen            = 0;
	isFrameStartPacket = false;

	if (!payload || payloadLen < 1)
	{
		return false;
	}

	// First octet of VP8 payload descriptor:
	//  X | R | N | S | PID(4)
	const uint8_t b0 = payload[0];

	const bool X      = (b0 & 0x80) != 0;
	const bool S      = (b0 & 0x10) != 0;
	const uint8_t PID = (b0 & 0x0F);

	// For first packet of encoded frame in VP8:
	// S must be 1, and first partition is PID=0.
	isFrameStartPacket = (S && PID == 0);

	size_t off = 1; // base descriptor octet consumed

	if (!X)
	{
		descLen = off;
		return true;
	}

	// Extended control bits octet exists.
	if (payloadLen < off + 1)
	{
		return false;
	}

	const uint8_t ext = payload[off++];
	const bool I      = (ext & 0x80) != 0;
	const bool L      = (ext & 0x40) != 0;
	const bool T      = (ext & 0x20) != 0;
	const bool K      = (ext & 0x10) != 0;

	// I: PictureID present
	if (I)
	{
		if (payloadLen < off + 1)
		{
			return false;
		}

		const uint8_t pic0 = payload[off++];
		const bool M       = (pic0 & 0x80) != 0; // 15-bit PictureID if set

		if (M)
		{
			if (payloadLen < off + 1)
			{
				return false;
			}
			off += 1;
		}
	}

	// L: TL0PICIDX present
	if (L)
	{
		if (payloadLen < off + 1)
		{
			return false;
		}
		off += 1;
	}

	// T or K => TID/Y/KEYIDX octet present
	if (T || K)
	{
		if (payloadLen < off + 1)
		{
			return false;
		}
		off += 1;
	}

	descLen = off;
	return true;
}

// Search MAGIC only in early payload area of the FIRST packet.
// Why not whole payload?
// To reduce false positives in compressed bitstream.
static bool FindMagicInWindow(
  const uint8_t* payload,
  size_t payloadLen,
  size_t searchStart,
  size_t searchEndExclusive,
  size_t& magicOffset)
{
	magicOffset = 0;

	if (!payload || payloadLen < 4 || searchStart >= payloadLen)
	{
		return false;
	}

	const size_t end = std::min(searchEndExclusive, payloadLen);

	if (end < searchStart + 4)
	{
		return false;
	}

	for (size_t i = searchStart; i + 4 <= end; ++i)
	{
		if (std::memcmp(payload + i, MAGIC_LE, 4) == 0)
		{
			// Ensure full latency header fits in this RTP packet.
			if (i + LAT_HEADER_LEN <= payloadLen)
			{
				magicOffset = i;
				return true;
			}
		}
	}

	return false;
}

// Main helper:
// - Confirm this RTP packet is the FIRST packet of a VP8 frame.
// - Parse VP8 descriptor length.
// - Search MAGIC in the early region of payload.
static Vp8PrefixLocateResult LocateLatencyPrefixInVp8FirstPacket(const uint8_t* payload, size_t payloadLen)
{
	Vp8PrefixLocateResult res;

	size_t descLen          = 0;
	bool isFrameStartPacket = false;

	if (!ParseVp8PayloadDescriptor(payload, payloadLen, descLen, isFrameStartPacket))
	{
		return res;
	}

	res.vp8DescriptorLen = descLen;
	res.isVp8FirstPacket = isFrameStartPacket;

	if (!isFrameStartPacket)
	{
		return res;
	}

	// Search policy:
	// Start from descriptor end (best starting point),
	// but allow a tiny cushion before/after if you want robustness.
	//
	// Conservative version:
	//   search only [descLen, descLen + 64)
	//
	// If you want exactly "from payload[0] up to payload[63]" then use:
	//   FindMagicInWindow(payload, payloadLen, 0, 64, ...)
	//
	// I recommend starting at descLen.
	size_t magicOffset       = 0;
	const size_t searchStart = descLen;
	const size_t searchEnd   = descLen + 64;

	if (FindMagicInWindow(payload, payloadLen, searchStart, searchEnd, magicOffset))
	{
		res.foundMagic  = true;
		res.magicOffset = magicOffset;
	}

	return res;
}

void HandleVideoRtpPayload(const uint8_t* payload, size_t payloadLen)
{
	auto loc = LocateLatencyPrefixInVp8FirstPacket(payload, payloadLen);

	if (!loc.isVp8FirstPacket)
	{
		// Not first packet of frame -> skip
		return;
	}

	if (!loc.foundMagic)
	{
		// For debugging
		MS_ERROR_STD(
		  "[VP8 prefix] first packet but MAGIC not found "
		  "(payloadLen=%zu, descLen=%zu)",
		  payloadLen,
		  loc.vp8DescriptorLen);
		return;
	}

	const uint8_t* p = payload + loc.magicOffset;

	// Here:
	// p[0..3]   = MAGIC
	// p[4..7]   = frameId
	// p[8..15]  = sendTsMs
	// p[16..23] = SFUrecvMs
	// p[24..31] = SFUsendMs

	// Example:
	const double SFUsendMs = NowEpochMs();
	WriteF64LE(const_cast<uint8_t*>(p) + 24, SFUsendMs);

	const double check = ReadF64LE(p + 24);

	MS_ERROR_STD(
	  "[VP8 prefix] found: descLen=%zu magicOff=%zu SFUsendMs=%.3f check=%.3f",
	  loc.vp8DescriptorLen,
	  loc.magicOffset,
	  SFUsendMs,
	  check);
}

static int GetVp8FrameType(const uint8_t* payload, size_t len)
{
	if (!payload || len < 1)
	{
		return -1; // unknown
	}

	size_t pos = 0;

	// VP8 Payload Descriptor first byte
	uint8_t b0 = payload[pos++];

	bool x      = (b0 & 0x80) != 0; // X bit
	bool s      = (b0 & 0x10) != 0; // Start of VP8 partition
	uint8_t pid = (b0 & 0x0F);      // PartID

	// frame type 판단은 start-of-partition-0 패킷에서만 신뢰
	if (!s || pid != 0)
	{
		return -1; // unknown
	}

	if (x)
	{
		if (pos >= len)
		{
			return -1;
		}

		uint8_t ext = payload[pos++];
		bool i      = (ext & 0x80) != 0;
		bool l      = (ext & 0x40) != 0;
		bool t      = (ext & 0x20) != 0;
		bool k      = (ext & 0x10) != 0;

		if (i)
		{
			if (pos >= len)
			{
				return -1;
			}

			uint8_t pic = payload[pos++];
			bool m      = (pic & 0x80) != 0;
			if (m)
			{
				if (pos >= len)
				{
					return -1;
				}
				pos++;
			}
		}

		if (l)
		{
			if (pos >= len)
			{
				return -1;
			}
			pos++;
		}

		if (t || k)
		{
			if (pos >= len)
			{
				return -1;
			}
			pos++;
		}
	}

	// ===== LAT prefix가 descriptor 뒤에 들어간 경우 건너뛴다 =====
	if (pos + LAT_HEADER_LEN <= len && std::memcmp(payload + pos, MAGIC_LE, 4) == 0)
	{
		pos += LAT_HEADER_LEN;
	}

	if (pos >= len)
	{
		return -1;
	}

	// 이제 실제 VP8 uncompressed header의 첫 바이트를 본다.
	uint8_t vp8Payload0 = payload[pos];
	return ((vp8Payload0 & 0x01) == 0) ? 1 : 0;
}

namespace RTC
{
	/* Static. */

	static std::shared_ptr<RTC::FrameRecordTable> GetSharedFrameRecordTable()
	{
		static std::shared_ptr<RTC::FrameRecordTable> sharedFrameRecordTable =
		  std::make_shared<RTC::FrameRecordTable>(5000);

		return sharedFrameRecordTable;
	}

	static constexpr uint16_t IceCandidateDefaultLocalPriority{ 10000 };
	// We just provide "host" candidates so type preference is fixed.
	static constexpr uint16_t IceTypePreference{ 64 };
	// We do not support non rtcp-mux so component is always 1.
	static constexpr uint16_t IceComponent{ 1 };

	static inline uint32_t generateIceCandidatePriority(uint16_t localPreference)
	{
		MS_TRACE();

		return (std::pow(2, 24) * IceTypePreference) + (std::pow(2, 8) * localPreference) +
		       (std::pow(2, 0) * (256 - IceComponent));
	}

	// yeon: pacing 구현
	//  ---- TokenBucketPacer implementation ----
	WebRtcTransport::TokenBucketPacer::TokenBucketPacer(WebRtcTransport* transport)
	  : transport(transport)
	{
		// token state init
		const uint64_t now = DepLibUV::GetTimeMs();
		this->lastRefillMs = now;
	}

	RTC::WebRtcTransport::TokenBucketPacer::~TokenBucketPacer()
	{
		StopAndFlush(false);
	}

	void RTC::WebRtcTransport::TokenBucketPacer::SetPacingRate(uint32_t Bps)
	{
		// pacing rate
		this->tokenRateBytesPerMs = (static_cast<double>(Bps)) / 1000.0;
	}

	double RTC::WebRtcTransport::TokenBucketPacer::GetPacingRate()
	{
		// pacing rate
		return this->tokenRateBytesPerMs * 1000; // 표현해야 할 지표가 뭐지? => 초당 토큰 충전 속도?
	}
	double RTC::WebRtcTransport::TokenBucketPacer::GetBucketSize() const
	{
		return this->bucketCapacityBytes;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::SetBucketSize(uint32_t burketSize)
	{
		this->bucketCapacityBytes = static_cast<double>(burketSize);
		this->tokensBytes         = std::min(this->tokensBytes, this->bucketCapacityBytes);
	}

	void RTC::WebRtcTransport::TokenBucketPacer::SetQueueLimits(uint32_t maxDelayMs, size_t maxBytes)
	{
		this->maxQueueDelayMs = maxDelayMs;
		this->maxQueueBytes   = maxBytes;
	}

	double RTC::WebRtcTransport::TokenBucketPacer::GetLinkCapacityBytesPerMs() const
	{
		return this->linkCapacityBytesPerMs;
	}

	size_t RTC::WebRtcTransport::TokenBucketPacer::IsLossDetected() const
	{
		return this->lossDetected;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::SetLinkCapacityBytesPerMs(double value)
	{
		if (value < 0.0)
		{
			return;
		}

		this->linkCapacityBytesPerMs = value;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::SetLossDetected(double value)
	{
		if (value > 0)
		{
			this->lossDetected = 1;
		}
		else
		{
			this->lossDetected = 0;
		}
	}

	void RTC::WebRtcTransport::TokenBucketPacer::UpdatePredictedQueueBytes()
	{
		double queueDelayMs       = std::max(0.0, standingRttMs - minRttMs);
		this->predictedQueueBytes = queueDelayMs * this->linkCapacityBytesPerMs;
		// MS_ERROR_STD("queueDelayMs: %f, predictedQueueSize: %f", queueDelayMs, this->predictedQueueBytes);
	}

	void RTC::WebRtcTransport::TokenBucketPacer::IncreaseBucketSize()
	{
		double B = this->bucketCapacityBytes;

		bool queueCleared = (this->predictedQueueBytes <= 0.0);
		// MS_ERROR_STD("IncreaseBucketSize()");
		//  queue가 비어있다면 그때의 버킷 사이즈를 저장한다.
		if (queueCleared)
		{
			this->historicalEmptyBucketBytes = bucketCapacityBytes;
			hasHistoricalInfo                = true;
		}

		// 1. 유지
		// MS_ERROR_STD("previousFrameBytes: %zu", this->previousFrameBytes);
		if (this->previousFrameBytes > 0 && B > static_cast<double>(this->previousFrameBytes)) // 버킷사이즈가
		                                                                                       // 이전
		                                                                                       // 프레임보다
		                                                                                       // 크면 유지
		                                                                                       // 많아도
		                                                                                       // 16000바이트
		{
			// this->bucketCapacityBytes = std::max(this->minBucketBytes, std::min(B,
			// this->maxBucketBytes)); MS_ERROR_STD("유지");
			this->tokensBytes = std::min(this->tokensBytes, this->bucketCapacityBytes);
			return;
		}

		// 3.1 증가: fast recovery

		// MS_ERROR_STD("lastQueueBytesBeforeLoss: %f", this->lastQueueBytesBeforeLoss);

		if (queueCleared && this->hasHistoricalInfo && this->lastQueueBytesBeforeLoss > 0.0)
		{
			MS_ERROR_STD("빠른복구");
			const double recent = this->alpha * this->lastQueueBytesBeforeLoss;
			B                   = std::min(this->historicalEmptyBucketBytes, recent);

			// B                         = std::max(this->minBucketBytes, std::min(B, this->maxBucketBytes));
			this->bucketCapacityBytes = B;
			this->tokensBytes         = std::min(this->tokensBytes, this->bucketCapacityBytes);
			return;
		}

		// MS_ERROR_STD("상승");
		//  3.2 증가: additive increase
		B += this->additiveStepBytes;
		// B                         = std::max(this->minBucketBytes, std::min(B, this->maxBucketBytes));
		this->bucketCapacityBytes = B;
		this->tokensBytes         = std::min(this->tokensBytes, this->bucketCapacityBytes);
	}

	void RTC::WebRtcTransport::TokenBucketPacer::DecreaseBucketSize()
	{
		double B = this->bucketCapacityBytes;

		// 2.1 감소: 로스 발생으로 절반 감소
		if (this->lossDetected > 0)
		{
			MS_ERROR_STD("로스 감소");
			lastQueueBytesBeforeLoss = this->predictedQueueBytes; // 로스 발생 전 큐 사이즈를 저장함
			B /= 2.0;

			B                         = std::max(this->minBucketBytes, B);
			this->bucketCapacityBytes = B;
			this->tokensBytes         = std::min(this->tokensBytes, this->bucketCapacityBytes);
			this->lossDetected        = 0;
			return;
		}
		// 2.2 감소:
		// MS_ERROR_STD("현재 네트워크 큐 사이즈: %f", this->predictedQueueBytes);
		if (this->predictedQueueBytes > this->queueThresholdBytes) // queue 사이즈가 12000을 초과하면 그
		                                                           // 초과량만큼 감소시킨다.
		{
			MS_ERROR_STD("감소");
			B -=
			  (this->predictedQueueBytes - this->queueThresholdBytes); // 네트워크 큐에 있는 바이트 수
			                                                           // = 논문은 패킷 수 사용하여 적정값
			                                                           // 10이므로, X 1200= 12000

			B                         = std::max(this->minBucketBytes, B);
			this->bucketCapacityBytes = B;
			this->tokensBytes         = std::min(this->tokensBytes, this->bucketCapacityBytes);
			return;
		}
	}

	void RTC::WebRtcTransport::TokenBucketPacer::UpdateBucketSize()
	{
		this->IncreaseBucketSize();
		this->DecreaseBucketSize();
	}

	void RTC::WebRtcTransport::TokenBucketPacer::EnsureTimer()
	{
		if (this->timerInited)
		{
			return;
		}

		uv_loop_t* loop = DepLibUV::GetLoop(); // mediasoup DepLibUV helper
		uv_timer_init(loop, &this->timer);
		this->timer.data  = this;
		this->timerInited = true;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::PacerTimer(uint64_t delayMs)
	{
		EnsureTimer();

		// 타이머 멈추기
		uv_timer_stop(&this->timer);

		uv_timer_start(
		  &this->timer,
		  [](uv_timer_t* handle)
		  {
			  auto* self = static_cast<TokenBucketPacer*>(handle->data);
			  self->OnTimer();
		  },
		  delayMs,
		  0);
	}

	// void RTC::WebRtcTransport::TokenBucketPacer::SetBurstMs(uint32_t burstMs)
	// {
	// 	this->burstWindowMs = std::max<uint32_t>(1, burstMs);
	// 	this->bucketCapacityBytes = this->tokenRateBytesPerMs * static_cast<double>(this->burstWindowMs);
	// 	this->tokensBytes = std::min(this->tokensBytes, this->bucketCapacityBytes);
	// }
	void RTC::WebRtcTransport::TokenBucketPacer::StopAndFlush(bool sent)
	{
		if (this->timerInited)
		{
			uv_timer_stop(&this->timer);
			// uv_close는 transport 종료 시점의 실제 패턴에 맞춰 조정 가능
			// 여기서는 단순 stop만.
		}

		// flush queue
		for (auto& item : this->q)
		{
			if (item.cb)
			{
				(*item.cb)(sent);
				delete item.cb;
			}
		}
		this->q.clear();
		this->queuedBytes = 0;
	}

	bool RTC::WebRtcTransport::TokenBucketPacer::ShouldDrop(const PendingRtp& item, uint64_t nowMs) const
	{
		if (this->maxQueueDelayMs == 0)
		{
			return false;
		}
		return (nowMs - item.enqueuedAtMs) > this->maxQueueDelayMs;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::Refill(uint64_t nowMs)
	{
		if (nowMs <= this->lastRefillMs)
		{
			return;
		}

		const double deltaMs = static_cast<double>(nowMs - this->lastRefillMs);
		this->lastRefillMs   = nowMs;

		this->tokensBytes += deltaMs * this->tokenRateBytesPerMs;

		if (this->tokensBytes > this->bucketCapacityBytes)
		{
			this->tokensBytes = this->bucketCapacityBytes;
		}
	}

	void RTC::WebRtcTransport::TokenBucketPacer::Enqueue(
	  RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb)
	{
		const uint64_t now = DepLibUV::GetTimeMs();

		PendingRtp item;
		item.consumer     = consumer;
		item.cb           = cb;
		item.enqueuedAtMs = now;

		// SharedRtpPacket를 생성해서 그걸 queue에 삽입
		item.sharedPacket.Assign(packet);

		// enqueue
		this->queuedBytes += item.sharedPacket.GetPacket()->GetSize();
		// MS_ERROR_STD("queue: %zu", this->queuedBytes);

		this->q.push_back(std::move(item));

		Refill(now);

		// 여기에 리필 이후 매 전송 기회마다 bucket을 조절?
		// UpdatePredictedQueueBytes();
		// UpdateBucketSize();

		bool progressed = true;
		while (progressed)
		{
			progressed = TrySendOne(now);
		}

		if (!this->q.empty())
		{
			auto* front          = this->q.front().sharedPacket.GetPacket();
			const double need    = static_cast<double>(front->GetSize()); // bytes
			const double deficit = std::max(0.0, need - this->tokensBytes);

			uint64_t waitMs = 1;
			if (this->tokenRateBytesPerMs > 0.0)
			{
				waitMs = static_cast<uint64_t>(std::ceil(deficit / this->tokenRateBytesPerMs));
			}
			// tokenRateBytesPerMs = 1ms당 토큰이 채워지는 속도 => 저 부족한 값만큼 deficit를 채우기 위해
			// 필요한 시간이 waitMs이므로 저 시간 만큼 기다린다. 만약 저 속도가 빠르면 그만큼 waitMs도 줄어듦
			PacerTimer(waitMs); // 타이머를 걸어두고, 그 시간 후 onTimer()호출한다.
		}
	}

	bool RTC::WebRtcTransport::TokenBucketPacer::TrySendOne(uint64_t nowMs)
	{
		if (this->q.empty())
		{
			return false;
		}

		// Drop too-late packets (front) => gpt
		while (!this->q.empty() && ShouldDrop(this->q.front(), nowMs))
		{
			auto& item = this->q.front();
			if (item.cb)
			{
				(*item.cb)(false);
				delete item.cb;
			}
			this->queuedBytes -= item.sharedPacket.GetPacket()->GetSize();
			this->q.pop_front();
		}
		if (this->q.empty())
		{
			return false;
		}

		auto& item          = this->q.front();
		RTC::RtpPacket* pkt = item.sharedPacket.GetPacket();
		const double cost   = static_cast<double>(pkt->GetSize());

		// 토큰이 충분하지 않으면 대기 (다음 차례에)
		if (this->tokensBytes < cost)
		{
			return false;
		}

		// 토큰 소비
		this->tokensBytes -= cost;

		// 먼저 큐에서 제거
		// ?? re-entrancy issues
		auto cb           = item.cb;
		auto* consumer    = item.consumer;
		auto sharedPacket = item.sharedPacket;
		this->queuedBytes -= pkt->GetSize();
		this->q.pop_front();

		// Actually send now (SRTP encrypt + tuple send happens here)
		this->transport->SendRtpPacketNow(consumer, sharedPacket.GetPacket(), cb);

		return true;
	}

	void RTC::WebRtcTransport::TokenBucketPacer::OnTimer()
	{
		const uint64_t now = DepLibUV::GetTimeMs();
		Refill(now);

		// this->UpdatePredictedQueueBytes();
		// this->UpdateBucketSize();

		bool progressed = true;
		while (progressed)
		{
			progressed = TrySendOne(now);
		}

		if (this->q.empty())
		{
			uv_timer_stop(&this->timer);
			return;
		}

		// 다음에 가능할때 보내기
		auto* front          = this->q.front().sharedPacket.GetPacket();
		const double need    = static_cast<double>(front->GetSize());
		const double deficit = std::max(0.0, need - this->tokensBytes);

		uint64_t waitMs = 1;
		if (this->tokenRateBytesPerMs > 0.0)
		{
			waitMs = static_cast<uint64_t>(std::ceil(deficit / this->tokenRateBytesPerMs));
		}

		PacerTimer(waitMs);
	}

	void RTC::WebRtcTransport::TokenBucketPacer::ObservePacketForFrame(const RTC::RtpPacket* pkt)
	{
		// 기본 값은 false라서 처음 패킷은 true임
		if (!this->frameInit)
		{
			this->frameInit             = true;
			this->currentFrameTimestamp = pkt->GetTimestamp();
		}

		// 현재 패킷이 새로운 타임스탬프의 패킷이다 => 새로운 프레임이다 => 현재 저장된 프레임 바이트를
		// 이전 값으로 저장
		if (pkt->GetTimestamp() != this->currentFrameTimestamp)
		{
			this->previousFrameBytes    = this->currentFrameBytes;
			this->currentFrameBytes     = 0;
			this->currentFrameTimestamp = pkt->GetTimestamp();
		}

		this->currentFrameBytes += pkt->GetSize();

		// if (pkt->HasMarker()) // 마지막 패킷일 때, 현재까지 내용을 저장하고,
		// {
		// 	this->previousFrameBytes = this->currentFrameBytes;
		// 	this->currentFrameBytes  = 0;
		// }
	}

	// 네트워크 상황 지표
	void RTC::WebRtcTransport::TokenBucketPacer::SetRttMs(double rttMs)
	{
		if (rttMs <= 0.0)
		{
			return;
		}

		auto now = std::chrono::steady_clock::now();
		int64_t nowMs =
		  std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

		if (!this->started)
		{
			this->started         = true;
			this->flowStartTimeMs = nowMs;
		}

		this->latestRttMs = rttMs;

		// Copa-style SRTT (EWMA) 업데이트
		// RFC6298 스타일 alpha=1/8
		if (this->srttMs <= 0.0)
		{
			this->srttMs = rttMs;
		}
		else
		{
			this->srttMs = (1.0 - 0.125) * this->srttMs + 0.125 * rttMs;
		}

		// RTT 샘플 저장
		this->rttHistory.push_back({ nowMs, rttMs });

		// 가장 긴 윈도우(최대 10초)보다 더 오래된 샘플은 제거
		int64_t flowAgeMs    = nowMs - this->flowStartTimeMs;
		int64_t longWindowMs = std::min<int64_t>(10000, flowAgeMs > 0 ? flowAgeMs : 10000);
		int64_t cutoffLong   = nowMs - longWindowMs;

		while (!this->rttHistory.empty() && this->rttHistory.front().timeMs < cutoffLong)
		{
			this->rttHistory.pop_front();
		}

		// RTTstanding 계산: 최근 srtt/2 구간의 최소 RTT
		double standingWindowMsDouble = this->srttMs / 2.0;
		if (standingWindowMsDouble < 1.0)
		{
			standingWindowMsDouble = 1.0;
		}
		int64_t standingWindowMs = static_cast<int64_t>(standingWindowMsDouble);
		int64_t cutoffStanding   = nowMs - standingWindowMs;

		double standingMin = std::numeric_limits<double>::max();
		double longMin     = std::numeric_limits<double>::max();

		for (const auto& sample : this->rttHistory)
		{
			// long-window min = RTTmin
			if (sample.rttMs < longMin)
			{
				longMin = sample.rttMs;
			}

			// standing-window min = RTTstanding
			if (sample.timeMs >= cutoffStanding && sample.rttMs < standingMin)
			{
				standingMin = sample.rttMs;
			}
		}

		// 샘플이 부족하면 fallback
		if (standingMin == std::numeric_limits<double>::max())
		{
			standingMin = rttMs;
		}
		if (longMin == std::numeric_limits<double>::max())
		{
			longMin = rttMs;
		}

		this->standingRttMs = standingMin;
		this->minRttMs      = longMin;

		// Copa-style queueing delay
		this->queueDelayMs = this->standingRttMs - this->minRttMs;
		if (this->queueDelayMs < 0.0)
		{
			this->queueDelayMs = 0.0;
		}

		this->UpdatePredictedQueueBytes();
		this->UpdateBucketSize();

		if (this->transport && this->transport->networkState)
		{
			this->transport->networkState->UpdateAceQueueBytes(
			  this->predictedQueueBytes, static_cast<uint64_t>(nowMs));
			this->transport->networkState->UpdatePacingBacklogBytes(
			  static_cast<double>(this->queuedBytes), static_cast<uint64_t>(nowMs));
		}

		// 아래부터는 페이싱 로그를 위한 코드
		if (!pacinglog)
		{
			return;
		}
		// 프로그램 실행 중 최초 1회 헤더 작성
		if (!headerWritten)
		{
			bool fileExists = std::filesystem::exists(logPath);
			std::ofstream headerFile(logPath, std::ios::out | std::ios::app);

			if (headerFile.is_open())
			{
				if (!fileExists)
				{
					headerFile << "time_ms,rtt_ms,min_rtt_ms,predicted_queue_bytes,bucket_size_bytes\n";
					headerFile.flush();
				}
				headerFile.close();
				headerWritten = true;
			}
		}

		std::ofstream logFile(logPath, std::ios::out | std::ios::app);
		if (logFile.is_open())
		{
			auto now = std::chrono::steady_clock::now();
			auto nowMs =
			  std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

			logFile << nowMs << "," << this->latestRttMs << "," << this->minRttMs << ","
			        << this->predictedQueueBytes << "," << this->GetBucketSize() << "\n";

			logFile.flush();
			logFile.close();
		}

		// MS_ERROR_STD("bucket size=%f", this->GetBucketSize());
	}

	// ---- TokenBucketPacer implementation ----

	void RTC::WebRtcTransport::OnAvailableBitrateChanged(uint32_t availableBitrate)
	{
		const uint64_t nowMs = DepLibUV::GetTimeMs();

		if (this->networkState)
		{
			this->networkState->UpdateAvailableBitrateBps(static_cast<double>(availableBitrate), nowMs);
		}

		if (!this->rtpPacer)
		{
			return;
		}

		this->rtpPacer->SetLinkCapacityBytesPerMs(availableBitrate / 8000);
		this->rtpPacer->SetPacingRate(
		  availableBitrate / 8); // 초당 바이트 단위로 바꿔준다. 여기서 1ms당 바이트 단위로 바꾼다.
		                         // MS_ERROR_STD("pacing rate=%f", this->rtpPacer->GetPacingRate());
	}

	void RTC::WebRtcTransport::OnPacketLossDetected(double loss)
	{
		const uint64_t nowMs = DepLibUV::GetTimeMs();
		if (this->networkState)
		{
			this->networkState->UpdateLossRate(loss, nowMs);
		}
		// lossDetected = loss; // setter 함수 필요 private
		if (!this->rtpPacer || ispacing.load() == false)
		{
			return;
		}

		this->rtpPacer->SetLossDetected(loss);
	}

	void RTC::WebRtcTransport::OnRttUpdated(double rttMs)
	{
		const uint64_t nowMs = DepLibUV::GetTimeMs();

		if (this->networkState)
		{
			this->networkState->UpdateRttMs(rttMs, nowMs);
		}

		if (!this->rtpPacer || ispacing.load() == false)
		{
			return;
		}
		this->rtpPacer->SetRttMs(rttMs); // setter}
	}

	void RTC::WebRtcTransport::OnSlack(const uint8_t* msg, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("OnSlack");
		// MS_WARN_TAG(
		//   sctp,
		//   "[SLACK] transportId:%s raw telemetry payload len:%zu text:%s",
		//   this->id.c_str(),
		//   len,
		//   text.c_str());

		if (!msg || len == 0)
		{
			MS_WARN_TAG(sctp, "[SLACK] empty SCTP telemetry message");
			return;
		}

		// msg는 null-terminated가 아닐 수 있으므로 len 기반으로 문자열 생성
		const std::string text(reinterpret_cast<const char*>(msg), len);

		//MS_WARN_TAG(sctp, "[SLACK] raw telemetry payload len:%zu text:%s", len, text.c_str());

		std::string type;
		if (!ExtractJsonString(text, "type", type))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse field 'type' from payload:%s", text.c_str());
			return;
		}

		// 우리가 원하는 telemetry message만 처리
		if (type != "frame-telemetry")
		{
			MS_WARN_TAG(sctp, "[SLACK] ignore non frame-telemetry message type:%s", type.c_str());
			return;
		}

		uint64_t rtpTimestamp64{ 0 };
		uint64_t receiveTimeMs{ 0 };
		// uint64_t latestDecodeTimeUs{ 0 };
		uint64_t decodeStartMs{ 0 };
		uint64_t decodeFinishMs{ 0 };

		// effective slack
		int64_t latestDecodeTimeMs{ 0 };
		uint64_t frameBufferInsertTimeMs{ 0 };
		uint64_t frameBufferExtractTimeMs{ 0 };
		uint64_t decodeQueueInsertTimeMs{ 0 };
		uint64_t decodeQueueExtractTimeMs{ 0 };

		std::vector<RTC::PacketReceiveInfo> packetReceiveTimes;

		int64_t now{ 0 };
		int64_t render_time{ 0 };
		int64_t max_wait{ 0 };

		if (!ExtractJsonUint64(text, "rtpTimestamp", rtpTimestamp64))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse rtpTimestamp payload:%s", text.c_str());
			return;
		}

		if (!ExtractJsonUint64(text, "receiveTimeMs", receiveTimeMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse receiveTimeMs payload:%s", text.c_str());
			return;
		}

		// 브라우저 쪽 키 이름이 latestDecodeTimeUs 인지 latestDecodedTimeUs 인지 헷갈릴 수 있으니 둘 다 허용
		if (!ExtractJsonUint64(text, "decodeStartMs", decodeStartMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse decodeStartMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractJsonUint64(text, "decodeFinishMs", decodeFinishMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse decodeFinishMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractLatestDecodeTimingBundle(text, latestDecodeTimeMs, now, render_time, max_wait))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse latestDecodeTimeMs bundle payload:%s", text.c_str());
			return;
		}

		// MS_WARN_TAG(
		//   sctp,
		//   "[SLACK] decode timing bundle frameId:%" PRIu64 " latestDecodeTimeMs:%" PRId64 " now:%"
		//   PRId64 " render_time:%" PRId64 " max_wait:%" PRId64, rtpTimestamp64, latestDecodeTimeMs,
		//   now,
		//   render_time,
		//   max_wait);

		if (!ExtractJsonUint64(text, "frameBufferInsertTimeMs", frameBufferInsertTimeMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse FrameBufferInsertTimeMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractJsonUint64(text, "frameBufferExtractTimeMs", frameBufferExtractTimeMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse FrameBufferExtractTimeMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractJsonUint64(text, "decodeQueueInsertTimeMs", decodeQueueInsertTimeMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse decodeQueueInsertTimeMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractJsonUint64(text, "decodeQueueExtractTimeMs", decodeQueueExtractTimeMs))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse decodeQueueExtractTimeMs payload:%s", text.c_str());
			return;
		}

		if (!ExtractPacketReceiveTimes(text, packetReceiveTimes))
		{
			MS_WARN_TAG(sctp, "[SLACK] failed to parse packetReceiveTimes payload:%s", text.c_str());
			packetReceiveTimes.clear();
		}

		// 현재 frameId는 RTP timestamp를 사용 중
		const uint32_t frameId = static_cast<uint32_t>(rtpTimestamp64);

		// slack = latestDecodeTimeUs - receiveTimeUs
		// if (decodeStartMs < receiveTimeMs)
		// {
		// 	MS_WARN_TAG(
		// 	  sctp,
		// 	  "[SLACK] invalid telemetry: decodeStartMs(%" PRIu64 ") < receiveTimeMs(%" PRIu64
		// 	  ") frameId:%" PRIu32,
		// 	  decodeStartMs,
		// 	  receiveTimeMs,
		// 	  frameId);
		// }

		const double slackMs          = static_cast<double>(decodeStartMs - receiveTimeMs);
		const double decoding_latency = static_cast<double>(decodeFinishMs - decodeStartMs);
		bool attached{ false };

		if (this->frameRecordTable)
		{
			// this->frameRecordTable->AttachTimingAndDesiredTimes(
			//   frameId, receiveTimeMs, decodeStartMs, decodeFinishMs);
			this->frameRecordTable->AttachTimingAndDesiredTimes(
			  frameId,
			  receiveTimeMs,
			  latestDecodeTimeMs,
			  frameBufferInsertTimeMs,
			  frameBufferExtractTimeMs,
			  decodeQueueInsertTimeMs,
			  decodeQueueExtractTimeMs,
			  decodeStartMs,
			  decodeFinishMs,
			  now,
			  render_time,
			  max_wait);
			this->frameRecordTable->AttachPacketReceiveTimes(frameId, packetReceiveTimes);

			attached = this->frameRecordTable->AttachSlack(frameId, slackMs);
		}

		if (!attached)
		{
			MS_WARN_TAG(
			  sctp,
			  "[SLACK] no matching frame record for frameId:%" PRIu32 " slackMs:%.3f payload:%s",
			  frameId,
			  slackMs,
			  text.c_str());
			return;
		}
		else
		{
			// MS_ERROR_STD("no error");
		}

		if (attached && this->frameRecordTable && this->slackPredictor)
		{
			auto recordOpt = this->frameRecordTable->GetCompletedRecord(frameId);
			if (recordOpt.has_value())
			{
				const auto& rec = recordOpt.value();

				RTC::SlackSample sample;
				sample.frameId                    = rec.frameId;
				sample.feature.rttMs              = rec.network.rttMs;
				sample.feature.lossRate           = rec.network.lossRate;
				sample.feature.aceQueueBytes      = rec.network.aceQueueBytes;
				sample.feature.pacingBacklogBytes = rec.network.pacingBacklogBytes;
				sample.feature.frameSizeBytes     = static_cast<double>(rec.frameSizeBytes);
				sample.feature.packetCount        = static_cast<double>(rec.packetCount);
				sample.feature.temporalLayer      = static_cast<double>(rec.temporalLayer);
				sample.feature.isKeyFrame         = rec.isKeyFrame ? 1.0 : 0.0;
				sample.slackMs                    = rec.slackMs;

				this->slackPredictor->AddSample(sample);
				if (rec.hasReceiveTimeMs && rec.hasDesiredReceiveTimeMs && rec.hasReceiveSlackMs && this->frameRecordCsvWriter)
				{
					this->frameRecordCsvWriter->WriteRecord(rec);
				}
				if (this->framePacketCsvWriter && !packetReceiveTimes.empty())
				{
					this->framePacketCsvWriter->WritePacketReceiveTimes(frameId, packetReceiveTimes);
				}
			}
		}
		// MS_WARN_TAG(
		//   sctp,
		//   "[SLACK] attached frameId:%" PRIu32 " receiveTimeUs:%" PRIu64 " latestDecodeTimeUs:%"
		//   PRIu64 " slackMs:%.3f", frameId, receiveTimeUs, latestDecodeTimeUs, slackMs);
	} // 지금 네트워크 지표는 Slack과 더불어, 그때의 네트워크 상황이 수집되고 있음
	// 즉, 계산된 네트워크 Slack과 프레임 아이디인 Timestamp가 과거 데이터와 합체된다.
	// 이제 이걸 사용해서 slack을 예측하는 코드를 개발하면 된다.
	/**
	 * This constructor is used when the WebRtcTransport doesn't use a WebRtcServer.
	 */
	WebRtcTransport::WebRtcTransport(
	  RTC::Shared* shared,
	  const std::string& id,
	  RTC::Transport::Listener* listener,
	  const FBS::WebRtcTransport::WebRtcTransportOptions* options)
	  : RTC::Transport::Transport(shared, id, listener, options->base())
	{
		MS_TRACE();
		this->networkState     = std::make_unique<RTC::NetworkState>();
		this->frameRecordTable = std::make_unique<RTC::FrameRecordTable>(5000);
		this->slackPredictor   = std::make_unique<RTC::SlackPredictor>();
		try
		{
			const auto* listenIndividual = options->listen_as<FBS::WebRtcTransport::ListenIndividual>();
			const auto* listenInfos      = listenIndividual->listenInfos();
			uint16_t iceLocalPreferenceDecrement{ 0u };

			// Multiply by 2 to preallocate space in case |exposeInternalIp| is set.
			this->iceCandidates.reserve(listenInfos->size() * 2);

			for (const auto* listenInfo : *listenInfos)
			{
				auto ip = listenInfo->ip()->str();

				// This may throw.
				Utils::IP::NormalizeIp(ip);

				std::string announcedAddress;

				if (flatbuffers::IsFieldPresent(listenInfo, FBS::Transport::ListenInfo::VT_ANNOUNCEDADDRESS))
				{
					announcedAddress = listenInfo->announcedAddress()->str();
				}

				const bool exposeInternalIp = listenInfo->exposeInternalIp();

				RTC::Transport::SocketFlags flags;

				flags.ipv6Only     = listenInfo->flags()->ipv6Only();
				flags.udpReusePort = listenInfo->flags()->udpReusePort();

				const uint16_t iceLocalPreference =
				  IceCandidateDefaultLocalPriority - iceLocalPreferenceDecrement;
				const uint32_t icePriority = generateIceCandidatePriority(iceLocalPreference);

				if (listenInfo->protocol() == FBS::Transport::Protocol::UDP)
				{
					RTC::UdpSocket* udpSocket;

					if (listenInfo->portRange()->min() != 0 && listenInfo->portRange()->max() != 0)
					{
						uint64_t portRangeHash{ 0u };

						udpSocket = new RTC::UdpSocket(
						  this,
						  ip,
						  listenInfo->portRange()->min(),
						  listenInfo->portRange()->max(),
						  flags,
						  portRangeHash);
					}
					else if (listenInfo->port() != 0)
					{
						udpSocket = new RTC::UdpSocket(this, ip, listenInfo->port(), flags);
					}
					// NOTE: This is temporal to allow deprecated usage of worker port range.
					// In the future this should throw since |port| or |portRange| will be
					// required.
					else
					{
						uint64_t portRangeHash{ 0u };

						udpSocket = new RTC::UdpSocket(
						  this,
						  ip,
						  Settings::configuration.rtcMinPort,
						  Settings::configuration.rtcMaxPort,
						  flags,
						  portRangeHash);
					}

					this->udpSockets[udpSocket] = announcedAddress;

					if (announcedAddress.empty())
					{
						this->iceCandidates.emplace_back(udpSocket, icePriority);
					}
					else
					{
						this->iceCandidates.emplace_back(udpSocket, icePriority, announcedAddress);

						if (exposeInternalIp)
						{
							this->iceCandidates.emplace_back(udpSocket, icePriority - 1000);
						}
					}

					if (listenInfo->sendBufferSize() != 0)
					{
						// NOTE: This may throw.
						udpSocket->SetSendBufferSize(listenInfo->sendBufferSize());
					}

					if (listenInfo->recvBufferSize() != 0)
					{
						// NOTE: This may throw.
						udpSocket->SetRecvBufferSize(listenInfo->recvBufferSize());
					}

					MS_DEBUG_TAG(
					  info,
					  "UDP socket buffer sizes [send:%" PRIu32 ", recv:%" PRIu32 "]",
					  udpSocket->GetSendBufferSize(),
					  udpSocket->GetRecvBufferSize());
				}
				else if (listenInfo->protocol() == FBS::Transport::Protocol::TCP)
				{
					RTC::TcpServer* tcpServer;

					if (listenInfo->portRange()->min() != 0 && listenInfo->portRange()->max() != 0)
					{
						uint64_t portRangeHash{ 0u };

						tcpServer = new RTC::TcpServer(
						  this,
						  this,
						  ip,
						  listenInfo->portRange()->min(),
						  listenInfo->portRange()->max(),
						  flags,
						  portRangeHash);
					}
					else if (listenInfo->port() != 0)
					{
						tcpServer = new RTC::TcpServer(this, this, ip, listenInfo->port(), flags);
					}
					// NOTE: This is temporal to allow deprecated usage of worker port range.
					// In the future this should throw since |port| or |portRange| will be
					// required.
					else
					{
						uint64_t portRangeHash{ 0u };

						tcpServer = new RTC::TcpServer(
						  this,
						  this,
						  ip,
						  Settings::configuration.rtcMinPort,
						  Settings::configuration.rtcMaxPort,
						  flags,
						  portRangeHash);
					}

					this->tcpServers[tcpServer] = announcedAddress;

					if (announcedAddress.empty())
					{
						this->iceCandidates.emplace_back(tcpServer, icePriority);
					}
					else
					{
						this->iceCandidates.emplace_back(tcpServer, icePriority, announcedAddress);

						if (exposeInternalIp)
						{
							this->iceCandidates.emplace_back(tcpServer, icePriority - 1000);
						}
					}

					if (listenInfo->sendBufferSize() != 0)
					{
						// NOTE: This may throw.
						tcpServer->SetSendBufferSize(listenInfo->sendBufferSize());
					}

					if (listenInfo->recvBufferSize() != 0)
					{
						// NOTE: This may throw.
						tcpServer->SetRecvBufferSize(listenInfo->recvBufferSize());
					}

					MS_DEBUG_TAG(
					  info,
					  "TCP sockets buffer sizes [send:%" PRIu32 ", recv:%" PRIu32 "]",
					  tcpServer->GetSendBufferSize(),
					  tcpServer->GetRecvBufferSize());
				}

				// Decrement initial ICE local preference for next IP.
				iceLocalPreferenceDecrement += 100;
			}

			auto iceConsentTimeout = options->iceConsentTimeout();

			// Create a ICE server.
			this->iceServer = new RTC::IceServer(
			  this, Utils::Crypto::GetRandomString(32), Utils::Crypto::GetRandomString(32), iceConsentTimeout);

			// Create a DTLS transport.
			this->dtlsTransport = new RTC::DtlsTransport(this);

			// NOTE: This may throw.
			this->shared->channelMessageRegistrator->RegisterHandler(
			  this->id,
			  /*channelRequestHandler*/ this,
			  /*channelNotificationHandler*/ this);
		}
		catch (const MediaSoupError& error)
		{
			// Must delete everything since the destructor won't be called.

			delete this->dtlsTransport;
			this->dtlsTransport = nullptr;

			delete this->iceServer;
			this->iceServer = nullptr;

			for (auto& kv : this->udpSockets)
			{
				auto* udpSocket = kv.first;

				delete udpSocket;
			}
			this->udpSockets.clear();

			for (auto& kv : this->tcpServers)
			{
				auto* tcpServer = kv.first;

				delete tcpServer;
			}
			this->tcpServers.clear();

			this->iceCandidates.clear();

			throw;
		}
	}

	/**
	 * This constructor is used when the WebRtcTransport uses a WebRtcServer.
	 */
	WebRtcTransport::WebRtcTransport(
	  RTC::Shared* shared,
	  const std::string& id,
	  RTC::Transport::Listener* listener,
	  WebRtcTransportListener* webRtcTransportListener,
	  const std::vector<RTC::IceCandidate>& iceCandidates,
	  const FBS::WebRtcTransport::WebRtcTransportOptions* options)
	  : RTC::Transport::Transport(shared, id, listener, options->base()),
	    webRtcTransportListener(webRtcTransportListener), iceCandidates(iceCandidates)
	{
		MS_TRACE();

		// MS_ERROR_STD("WebRtcTransport()");
		//  yeon: pacing 구현
		//--//
		this->rtpPacer = std::make_unique<TokenBucketPacer>(this);
		// 기본값: 6 Mbps
		this->rtpPacer->SetPacingRate(1500000); // 초당 150만 바이트=> 1ms당 1500바이트 => 33ms당
		// 초당 30프레임 => 1개당 20패킷 =>

		this->rtpPacer->SetBucketSize(12000); // 일단 초기 값 사용 60000바이트

		// 큐 기본값
		this->rtpPacer->SetQueueLimits(/*maxDelayMs*/ 200, /*maxBytes*/ 2 * 1024 * 1024);
		//--//

		// yeon: deadline slack
		this->networkState     = std::make_unique<RTC::NetworkState>();
		//this->frameRecordTable = std::make_unique<RTC::FrameRecordTable>(5000); // 5000개만 저장하기
		this->frameRecordTable = GetSharedFrameRecordTable();
		this->slackPredictor   = std::make_unique<RTC::SlackPredictor>();

		if (ispacingg)
		{
			this->frameRecordCsvWriter = std::make_unique<RTC::FrameRecordCsvWriter>(
			  "/home/n2sl/yeon/qos/network/log/frame/frame_records_pacing.csv");
		}
		else
		{
			this->frameRecordCsvWriter = std::make_unique<RTC::FrameRecordCsvWriter>(
			  "/home/n2sl/yeon/qos/network/log/frame/frame_records.csv");
		}

		this->framePacketCsvWriter = std::make_unique<RTC::FramePacketCsvWriter>(
		  "/home/n2sl/yeon/qos/network/log/frame/frame_packets.csv");

		try
		{
			if (iceCandidates.empty())
			{
				MS_THROW_TYPE_ERROR("empty iceCandidates");
			}

			auto iceConsentTimeout = options->iceConsentTimeout();

			// Create a ICE server.
			this->iceServer = new RTC::IceServer(
			  this, Utils::Crypto::GetRandomString(32), Utils::Crypto::GetRandomString(32), iceConsentTimeout);

			// Create a DTLS transport.
			this->dtlsTransport = new RTC::DtlsTransport(this);

			// Notify the webRtcTransportListener.
			this->webRtcTransportListener->OnWebRtcTransportCreated(this);

			// NOTE: This may throw.
			this->shared->channelMessageRegistrator->RegisterHandler(
			  this->id,
			  /*channelRequestHandler*/ this,
			  /*channelNotificationHandler*/ this);
		}
		catch (const MediaSoupError& error)
		{
			// Must delete everything since the destructor won't be called.

			delete this->dtlsTransport;
			this->dtlsTransport = nullptr;

			delete this->iceServer;
			this->iceServer = nullptr;

			throw;
		}
	}

	WebRtcTransport::~WebRtcTransport()
	{
		MS_TRACE();

		// We need to tell the Transport parent class that we are about to destroy
		// the class instance. This is because child's destructor runs before
		// parent's destructor. See comment in Transport::OnSctpAssociationSendData().
		Destroying();

		// yeon: pacing 구현
		//--//
		if (this->rtpPacer)
		{
			// 남아있는 queued packet은 실패 처리
			this->rtpPacer->StopAndFlush(false);
			this->rtpPacer.reset();
		}
		//--//

		this->shared->channelMessageRegistrator->UnregisterHandler(this->id);

		// Must delete the DTLS transport first since it will generate a DTLS alert
		// to be sent.
		delete this->dtlsTransport;
		this->dtlsTransport = nullptr;

		delete this->iceServer;
		this->iceServer = nullptr;

		for (auto& kv : this->udpSockets)
		{
			auto* udpSocket = kv.first;

			delete udpSocket;
		}
		this->udpSockets.clear();

		for (auto& kv : this->tcpServers)
		{
			auto* tcpServer = kv.first;

			delete tcpServer;
		}
		this->tcpServers.clear();

		this->iceCandidates.clear();

		delete this->srtpSendSession;
		this->srtpSendSession = nullptr;

		delete this->srtpRecvSession;
		this->srtpRecvSession = nullptr;

		// Notify the webRtcTransportListener.
		if (this->webRtcTransportListener)
		{
			this->webRtcTransportListener->OnWebRtcTransportClosed(this);
		}

		if (this->frameRecordTable)
		{
			const auto count = this->frameRecordTable->GetSlackCount();
			const auto avg   = this->frameRecordTable->GetAverageSlackMs();
			const auto min   = this->frameRecordTable->GetMinSlackMs();
			const auto max   = this->frameRecordTable->GetMaxSlackMs();

			MS_WARN_TAG(
			  sctp, "[SLACK-STATS] count:%" PRIu64 " avg:%.3f ms min:%.3f ms max:%.3f ms", count, avg, min, max);

			const auto recvCount = this->frameRecordTable->GetReceiveSlackCount();
			const auto recvAvg   = this->frameRecordTable->GetAverageReceiveSlackMs();
			const auto recvMin   = this->frameRecordTable->GetMinReceiveSlackMs();
			const auto recvMax   = this->frameRecordTable->GetMaxReceiveSlackMs();

			MS_WARN_TAG(
			  sctp,
			  "[RECV-SLACK-STATS] count:%" PRIu64 " avg:%.3f ms min:%.3f ms max:%.3f ms",
			  recvCount,
			  recvAvg,
			  recvMin,
			  recvMax);
		}
	}

	flatbuffers::Offset<FBS::WebRtcTransport::DumpResponse> WebRtcTransport::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Add iceParameters.
		auto iceParameters = FBS::WebRtcTransport::CreateIceParametersDirect(
		  builder,
		  this->iceServer->GetUsernameFragment().c_str(),
		  this->iceServer->GetPassword().c_str(),
		  true);

		std::vector<flatbuffers::Offset<FBS::WebRtcTransport::IceCandidate>> iceCandidates;
		iceCandidates.reserve(this->iceCandidates.size());

		for (const auto& iceCandidate : this->iceCandidates)
		{
			iceCandidates.emplace_back(iceCandidate.FillBuffer(builder));
		}

		// Add iceState.
		auto iceState = RTC::IceServer::IceStateToFbs(this->iceServer->GetState());

		// Add iceSelectedTuple.
		flatbuffers::Offset<FBS::Transport::Tuple> iceSelectedTuple;

		if (this->iceServer->GetSelectedTuple())
		{
			iceSelectedTuple = this->iceServer->GetSelectedTuple()->FillBuffer(builder);
		}

		// Add dtlsParameters.fingerprints.
		std::vector<flatbuffers::Offset<FBS::WebRtcTransport::Fingerprint>> fingerprints;

		for (const auto& fingerprint : RTC::DtlsTransport::GetLocalFingerprints())
		{
			auto algorithm    = DtlsTransport::AlgorithmToFbs(fingerprint.algorithm);
			const auto& value = fingerprint.value;

			fingerprints.emplace_back(
			  FBS::WebRtcTransport::CreateFingerprintDirect(builder, algorithm, value.c_str()));
		}

		// Add dtlsParameters.role.
		auto dtlsRole  = DtlsTransport::RoleToFbs(this->dtlsRole);
		auto dtlsState = DtlsTransport::StateToFbs(this->dtlsTransport->GetState());

		// Add base transport dump.
		auto base = Transport::FillBuffer(builder);
		// Add dtlsParameters.
		auto dtlsParameters =
		  FBS::WebRtcTransport::CreateDtlsParametersDirect(builder, &fingerprints, dtlsRole);

		return FBS::WebRtcTransport::CreateDumpResponseDirect(
		  builder,
		  base,
		  FBS::WebRtcTransport::IceRole::CONTROLLED,
		  iceParameters,
		  &iceCandidates,
		  iceState,
		  iceSelectedTuple,
		  dtlsParameters,
		  dtlsState);
	}

	flatbuffers::Offset<FBS::WebRtcTransport::GetStatsResponse> WebRtcTransport::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		// Add iceState.
		auto iceState = RTC::IceServer::IceStateToFbs(this->iceServer->GetState());

		// Add iceSelectedTuple.
		flatbuffers::Offset<FBS::Transport::Tuple> iceSelectedTuple;

		if (this->iceServer->GetSelectedTuple())
		{
			iceSelectedTuple = this->iceServer->GetSelectedTuple()->FillBuffer(builder);
		}

		auto dtlsState = DtlsTransport::StateToFbs(this->dtlsTransport->GetState());

		// Base Transport stats.
		auto base = Transport::FillBufferStats(builder);

		return FBS::WebRtcTransport::CreateGetStatsResponse(
		  builder,
		  base,
		  // iceRole (we are always "controlled").
		  FBS::WebRtcTransport::IceRole::CONTROLLED,
		  iceState,
		  iceSelectedTuple,
		  dtlsState);
	}

	void WebRtcTransport::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::TRANSPORT_GET_STATS:
			{
				auto responseOffset = FillBufferStats(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::WebRtcTransport_GetStatsResponse, responseOffset);

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_DUMP:
			{
				auto dumpOffset = FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::WebRtcTransport_DumpResponse, dumpOffset);

				break;
			}

			case Channel::ChannelRequest::Method::WEBRTCTRANSPORT_CONNECT:
			{
				// Ensure this method is not called twice.
				if (this->connectCalled)
				{
					MS_THROW_ERROR("connect() already called");
				}

				const auto* body = request->data->body_as<FBS::WebRtcTransport::ConnectRequest>();

				const auto* dtlsParameters = body->dtlsParameters();

				RTC::DtlsTransport::Fingerprint dtlsRemoteFingerprint;
				RTC::DtlsTransport::Role dtlsRemoteRole;

				if (dtlsParameters->fingerprints()->size() == 0)
				{
					MS_THROW_TYPE_ERROR("empty dtlsParameters.fingerprints array");
				}

				// NOTE: Just take the first fingerprint.
				for (const auto& fingerprint : *dtlsParameters->fingerprints())
				{
					dtlsRemoteFingerprint.algorithm = DtlsTransport::AlgorithmFromFbs(fingerprint->algorithm());

					dtlsRemoteFingerprint.value = fingerprint->value()->str();

					// Just use the first fingerprint.
					break;
				}

				dtlsRemoteRole = RTC::DtlsTransport::RoleFromFbs(dtlsParameters->role());

				// Set local DTLS role.
				switch (dtlsRemoteRole)
				{
					case RTC::DtlsTransport::Role::CLIENT:
					{
						this->dtlsRole = RTC::DtlsTransport::Role::SERVER;

						break;
					}
					// If the peer has role "auto" we become "client" since we are ICE controlled.
					case RTC::DtlsTransport::Role::SERVER:
					case RTC::DtlsTransport::Role::AUTO:
					{
						this->dtlsRole = RTC::DtlsTransport::Role::CLIENT;

						break;
					}
				}

				this->connectCalled = true;

				// Pass the remote fingerprint to the DTLS transport.
				if (this->dtlsTransport->SetRemoteFingerprint(dtlsRemoteFingerprint))
				{
					// If everything is fine, we may run the DTLS transport if ready.
					MayRunDtlsTransport();
				}

				// Tell the caller about the selected local DTLS role.
				auto dtlsLocalRole = DtlsTransport::RoleToFbs(this->dtlsRole);

				auto responseOffset =
				  FBS::WebRtcTransport::CreateConnectResponse(request->GetBufferBuilder(), dtlsLocalRole);

				request->Accept(FBS::Response::Body::WebRtcTransport_ConnectResponse, responseOffset);

				break;
			}

			case Channel::ChannelRequest::Method::TRANSPORT_RESTART_ICE:
			{
				const std::string usernameFragment = Utils::Crypto::GetRandomString(32);
				const std::string password         = Utils::Crypto::GetRandomString(32);

				this->iceServer->RestartIce(usernameFragment, password);

				MS_DEBUG_DEV(
				  "WebRtcTransport ICE usernameFragment and password changed [id:%s]", this->id.c_str());

				// Reply with the updated ICE local parameters.
				auto responseOffset = FBS::Transport::CreateRestartIceResponseDirect(
				  request->GetBufferBuilder(),
				  this->iceServer->GetUsernameFragment().c_str(),
				  this->iceServer->GetPassword().c_str(),
				  true /* iceLite */
				);

				request->Accept(FBS::Response::Body::Transport_RestartIceResponse, responseOffset);

				break;
			}

			default:
			{
				// Pass it to the parent class.
				RTC::Transport::HandleRequest(request);
			}
		}
	}

	void WebRtcTransport::HandleNotification(Channel::ChannelNotification* notification)
	{
		MS_TRACE();

		// Pass it to the parent class.
		RTC::Transport::HandleNotification(notification);
	}

	void WebRtcTransport::ProcessStunPacketFromWebRtcServer(
	  RTC::TransportTuple* tuple, RTC::StunPacket* packet)
	{
		MS_TRACE();

		// Pass it to the IceServer.
		this->iceServer->ProcessStunPacket(packet, tuple);
	}

	// 실제 호출되는 함수임
	void WebRtcTransport::ProcessNonStunPacketFromWebRtcServer(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("debug");
		//   Increase receive transmission.
		RTC::Transport::DataReceived(len);

		// Check if it's RTCP.
		if (RTC::RTCP::Packet::IsRtcp(data, len))
		{
			OnRtcpDataReceived(tuple, data, len);
		}
		// Check if it's RTP.
		else if (RTC::RtpPacket::IsRtp(data, len))
		{
			OnRtpDataReceived(tuple, data, len);
		}
		// Check if it's DTLS.
		else if (RTC::DtlsTransport::IsDtls(data, len))
		{
			OnDtlsDataReceived(tuple, data, len);
		}
		else
		{
			MS_WARN_DEV("ignoring received packet of unknown type");
		}
	}

	void WebRtcTransport::RemoveTuple(RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		this->iceServer->RemoveTuple(tuple);
	}

	inline bool WebRtcTransport::IsConnected() const
	{
		MS_TRACE();

		// clang-format off
		return (
			(
				this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
				this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED
			) &&
			this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED
		);
		// clang-format on
	}

	void WebRtcTransport::MayRunDtlsTransport()
	{
		MS_TRACE();

		// Do nothing if we have the same local DTLS role as the DTLS transport.
		// NOTE: local role in DTLS transport can be NONE, but not ours.
		if (this->dtlsTransport->GetLocalRole() == this->dtlsRole)
		{
			return;
		}

		// Check our local DTLS role.
		switch (this->dtlsRole)
		{
			// If still 'auto' then transition to 'server' if ICE is 'connected' or
			// 'completed'.
			case RTC::DtlsTransport::Role::AUTO:
			{
				// clang-format off
				if (
					this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
					this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED
				)
				// clang-format on
				{
					MS_DEBUG_TAG(
					  dtls, "transition from DTLS local role 'auto' to 'server' and running DTLS transport");

					this->dtlsRole = RTC::DtlsTransport::Role::SERVER;
					this->dtlsTransport->Run(RTC::DtlsTransport::Role::SERVER);
				}

				break;
			}

			// 'client' is only set if a 'connect' request was previously called with
			// remote DTLS role 'server'.
			//
			// If 'client' then wait for ICE to be 'completed' (got USE-CANDIDATE).
			//
			// NOTE: This is the theory, however let's be more flexible as told here:
			//   https://bugs.chromium.org/p/webrtc/issues/detail?id=3661
			case RTC::DtlsTransport::Role::CLIENT:
			{
				// clang-format off
				if (
					this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
					this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED
				)
				// clang-format on
				{
					MS_DEBUG_TAG(dtls, "running DTLS transport in local role 'client'");

					this->dtlsTransport->Run(RTC::DtlsTransport::Role::CLIENT);
				}

				break;
			}

			// If 'server' then run the DTLS transport if ICE is 'connected' (not yet
			// USE-CANDIDATE) or 'completed'.
			case RTC::DtlsTransport::Role::SERVER:
			{
				// clang-format off
				if (
					this->iceServer->GetState() == RTC::IceServer::IceState::CONNECTED ||
					this->iceServer->GetState() == RTC::IceServer::IceState::COMPLETED
				)
				// clang-format on
				{
					MS_DEBUG_TAG(dtls, "running DTLS transport in local role 'server'");

					this->dtlsTransport->Run(RTC::DtlsTransport::Role::SERVER);
				}

				break;
			}
		}
	}

	static int GetVp8Tid(const uint8_t* payload, size_t len)
	{
		if (!payload || len < 1)
		{
			return -1;
		}

		size_t i   = 0;
		uint8_t b0 = payload[i++];

		bool X = (b0 & 0x80) != 0; // extension present?
		// bool N = (b0 & 0x20) != 0; // non-reference frame (not needed)
		// bool S = (b0 & 0x10) != 0; // start of VP8 partition
		// PartID = b0 & 0x0F

		if (!X)
		{
			return -1; // no extension => no TID in this minimal approach
		}

		if (i >= len)
		{
			return -1;
		}
		uint8_t x1 = payload[i++];

		bool I = (x1 & 0x80) != 0;
		bool L = (x1 & 0x40) != 0;
		bool T = (x1 & 0x20) != 0;
		bool K = (x1 & 0x10) != 0;

		// If I set, skip PictureID (1 or 2 bytes)
		if (I)
		{
			if (i >= len)
			{
				return -1;
			}
			uint8_t pic = payload[i++];
			if (pic & 0x80) // 16-bit PictureID
			{
				if (i >= len)
				{
					return -1;
				}
				i++;
			}
		}

		// If L set, skip TL0PICIDX (1 byte)
		if (L)
		{
			if (i >= len)
			{
				return -1;
			}
			i++;
		}

		// If T or K set, there is a byte with TID/KEYIDX
		if (T || K)
		{
			if (i >= len)
			{
				return -1;
			}
			uint8_t tk = payload[i++];

			if (T)
			{
				int tid = (tk >> 6) & 0x03; // bits 7..6
				return tid;
			}
		}

		return -1;
	}

	struct SsrcAgg
	{
		// last timestamp seen for this SSRC within current window
		uint32_t lastTs{ 0 };
		bool hasLastTs{ false };

		// number of distinct RTP timestamps observed (proxy for "frames")
		uint32_t frameCount{ 0 };

		// packet count by tid (0..2) and unknown(-1)
		std::array<uint32_t, 4> tidPkts{ 0, 0, 0, 0 }; // [0],[1],[2],[unknown]
	};

	// yeon : pacing 구현으로 인해 함수 변경
	// void WebRtcTransport::SendRtpPacket(
	//   RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb)
	// {
	// 	MS_TRACE();
	// 	int a;
	// 	//MS_ERROR_STD();
	// 	if(this->tccClient)
	// 	{
	// 		a = 101;
	// 	}
	// 	else
	// 	{
	// 		a = 141;
	// 	}
	// 	if (!IsConnected())
	// 	{
	// 		if (cb)
	// 		{
	// 			(*cb)(false);
	// 			delete cb;
	// 		}

	// 		return;
	// 	}

	// 	// Ensure there is sending SRTP session.
	// 	if (!this->srtpSendSession)
	// 	{
	// 		MS_WARN_DEV("ignoring RTP packet due to non sending SRTP session");

	// 		if (cb)
	// 		{
	// 			(*cb)(false);
	// 			delete cb;
	// 		}

	// 		return;
	// 	}

	// 	if(ParseFramePrefixforSend(packet->GetPayload(), packet->GetPayloadLength(),
	// &packet->rtpPrefix)) { 		const double SFUlatency = packet->rtpPrefix.SFUsendMs -
	// packet->rtpPrefix.SFUrecvMs;
	// 		// MS_ERROR_STD("[STAMP] FrameID=%d, p2s= %.3f, SFUlatency=%.3f, a=%d",
	// 		// 	packet->rtpPrefix.FrameID, packet->rtpPrefix.p2s, SFUlatency, a);

	// 		// MS_ERROR_STD("[STAMP] FrameID=%d, sendTsMs= %.3f, SFUsendMs=%.3f, SFUrecvMs=%.3f,
	// SFUlatency=%.3f",
	// 		// 	packet->rtpPrefix.FrameID, packet->rtpPrefix.sendTsMs, packet->rtpPrefix.SFUsendMs,
	// packet->rtpPrefix.SFUrecvMs, SFUlatency);

	// 	}

	// 	const uint8_t* data = packet->GetData();
	// 	auto len            = packet->GetSize();

	// 	if (!this->srtpSendSession->EncryptRtp(&data, &len))
	// 	{
	// 		if (cb)
	// 		{
	// 			(*cb)(false);
	// 			delete cb;
	// 		}

	// 		return;
	// 	}

	// 	/** logger **/
	// 	// auto recvUs = packet->GetReceivedAtUs();
	// 	// auto nowUs = DepLibUV::GetTimeUsInt64();
	// 	// auto delayUs = (recvUs != 0 ? (nowUs - recvUs) : 0);

	// 	// 추가: RTP 패킷의 각종 정보를 로깅하는 코드
	// 	// MS_ERROR_STD("[SFU_DELAY] ssrc=%" PRIu32 " seq=%" PRIu16 " timestamp=%" PRIu32 "
	// packetsize=%zu payloadlength=%zu recvUs=%" PRIu64 " nowUs=%" PRIu64 " deltaUs=%" PRId64 "\n",
	// 	// 	packet->GetSsrc(),
	// 	// 	packet->GetSequenceNumber(),
	// 	// 	packet->GetTimestamp(),
	// 	// 	packet->GetSize(),
	// 	// 	packet->GetPayloadLength(),
	// 	// 	recvUs,
	// 	// 	nowUs,
	// 	// 	delayUs);

	// 	this->iceServer->GetSelectedTuple()->Send(data, len, cb);

	// 	// Increase send transmission.
	// 	RTC::Transport::DataSent(len);
	// }

	// yeon : pacing 구현을 위한 새로운 SendRtpPacket 정의
	void WebRtcTransport::PredictSlack(RTC::RtpPacket* packet)
	{
		RTC::SlackFeature current;
		auto snapshot = this->networkState->GetSnapshot();

		current.rttMs              = snapshot.rttMs;
		current.lossRate           = snapshot.lossRate;
		current.aceQueueBytes      = snapshot.aceQueueBytes;
		current.pacingBacklogBytes = snapshot.pacingBacklogBytes;

		// 초기 버전: frame 전체 크기를 아직 모르므로 packet size를 근사로 사용
		current.frameSizeBytes = static_cast<double>(packet->GetSize());
		// current.packetCount    = 1.0;
		// current.temporalLayer  = 0.0;
		// current.isKeyFrame     = 0.0;

		this->currentPredictedSlack.reset();

		if (this->slackPredictor)
		{
			this->currentPredictedSlack = this->slackPredictor->PredictSlackMs(current);
			const uint32_t frameId      = packet->GetTimestamp();
			if (this->currentPredictedSlack.has_value())
			{
				this->pendingPredictedSlackByFrame[frameId] = this->currentPredictedSlack.value();
			}
			// if (this->currentPredictedSlack.has_value())
			// {
			// 	// MS_ERROR_STD(
			// 	//   "[SLACK-PREDICT] frame ts:%" PRIu32 " predicted slack: %.3f ms",
			// 	//   ts,
			// 	//   this->currentPredictedSlack.value());

			// 	const uint32_t frameId = packet->GetTimestamp();
			// 	this->frameRecordTable->AttachPredictedSlack(frameId,
			// this->currentPredictedSlack.value());
			// }
			// else
			// {
			// 	double heuristicScore = this->slackPredictor->PredictHeuristicSlackScore(current);
			// 	// MS_ERROR_STD(
			// 	//   "[SLACK-PREDICT] frame ts:%" PRIu32 " fallback heuristic score: %.3f", ts,
			// heuristicScore);
			// }
		}
	}

	void WebRtcTransport::SendRtpPacket(
	  RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb)
	{
		MS_TRACE();

		// 항상 frame 관측은 해둠
		if (this->rtpPacer)
		{
			this->rtpPacer->ObservePacketForFrame(packet);
		}

		// ===== 새 프레임의 첫 패킷인지 확인 =====
		const uint32_t ts = packet->GetTimestamp();
		bool isNewFrame   = false;

		if (!this->predictFrameInit)
		{
			this->predictFrameInit             = true;
			this->currentPredictFrameTimestamp = ts;
			isNewFrame                         = true;
		}
		else if (this->currentPredictFrameTimestamp != ts)
		{
			this->currentPredictFrameTimestamp = ts;
			isNewFrame                         = true;
		}

		// ===== 새 프레임일 때만 이 패킷을 보내기 전에 slack을 예측 ====
		// 현재는 slack 예측이 아니라 최근 slack 데이터를 사용해서 긴급성을 판단
		if (isNewFrame)
		{
			PredictSlack(packet);
		}
		// TODO:
		// 여기서 currentPredictedSlack 또는 heuristic score를 바탕으로
		// pacer parameter를 조정할 수 있음

		// ===== pacing experiment: runtime switch off -> on =====
		const uint64_t nowMs = DepLibUV::GetTimeMs();

		if (pacingExperimentStartMs == 0)
		{
			pacingExperimentStartMs = nowMs;
		}

		if (!pacingAutoSwitchDone.load() && nowMs - pacingExperimentStartMs >= PacingSwitchAfterMs)
		{
			ispacing.store(true);
			pacingAutoSwitchDone.store(true);

			MS_ERROR_STD("[PACING-EXP] pacing switched ON at %" PRIu64 " ms", nowMs);
		}

		const bool pacingEnabled = ispacing.load();

		if (this->rtpPacer && pacingEnabled)
		{
			this->rtpPacer->Enqueue(consumer, packet, cb);
		}
		else
		{
			SendRtpPacketNow(consumer, packet, cb);
		}
	}

	static FILE* g_videoSendLog{ nullptr };

	static inline uint64_t NowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
		         std::chrono::steady_clock::now().time_since_epoch())
		  .count();
	}

	static void InitVideoSendLog()
	{
		if (!g_videoSendLog)
		{
			if (ispacingg == true)
			{
				g_videoSendLog = std::fopen("/home/n2sl/yeon/qos/network/log/video_send_log_pacing.csv", "a");
			}
			else if (ispacingg == false)
			{
				g_videoSendLog =
				  std::fopen("/home/n2sl/yeon/qos/network/log/video_send_log_no_pacing.csv", "a");
			}

			if (g_videoSendLog)
			{
				std::fprintf(g_videoSendLog, "send_time_us,consumer_id,ssrc,seq,rtp_timestamp,marker,size\n");
				std::fflush(g_videoSendLog);
			}
		}
	}

	static void LogVideoPacketSend(
	  const std::string& consumerId,
	  uint32_t ssrc,
	  uint16_t seq,
	  uint32_t rtpTimestamp,
	  bool marker,
	  size_t size)
	{
		if (!g_videoSendLog)
		{
			return;
		}

		const uint64_t nowUs = NowUs();

		std::fprintf(
		  g_videoSendLog,
		  "%" PRIu64 ",%s,%" PRIu32 ",%" PRIu16 ",%" PRIu32 ",%d,%zu\n",
		  nowUs,
		  consumerId.c_str(),
		  ssrc,
		  seq,
		  rtpTimestamp,
		  marker ? 1 : 0,
		  size);

		// 너무 자주 fflush 하면 오버헤드가 크므로, 처음엔 실험용이면 해도 됨
		std::fflush(g_videoSendLog);
	}

	void RTC::WebRtcTransport::SendRtpPacketNow(
	  RTC::Consumer* consumer, RTC::RtpPacket* packet, const RTC::Transport::onSendCallback* cb)
	{
		MS_TRACE();
		int a;
		// MS_ERROR_STD();
		if (this->tccClient)
		{
			a = 101;
		}
		else
		{
			a = 141;
		}
		if (!IsConnected())
		{
			if (cb)
			{
				(*cb)(false);
				delete cb;
			}

			return;
		}

		// Ensure there is sending SRTP session.
		if (!this->srtpSendSession)
		{
			MS_WARN_DEV("ignoring RTP packet due to non sending SRTP session");

			if (cb)
			{
				(*cb)(false);
				delete cb;
			}

			return;
		}

		// HandleVideoRtpPayload(packet->GetPayload(), packet->GetPayloadLength());
		if (ParseFramePrefixforSend(packet->GetPayload(), packet->GetPayloadLength(), &packet->rtpPrefix))
		{
			const double SFUlatency = packet->rtpPrefix.SFUsendMs - packet->rtpPrefix.SFUrecvMs;
			// MS_ERROR_STD("[STAMP] FrameID=%d, p2s= %.3f, SFUlatency=%.3f, a=%d",
			// 	packet->rtpPrefix.FrameID, packet->rtpPrefix.p2s, SFUlatency, a);

			// MS_ERROR_STD("[STAMP] FrameID=%d, sendTsMs= %.3f, SFUsendMs=%.3f, SFUrecvMs=%.3f,
			// SFUlatency=%.3f", 	packet->rtpPrefix.FrameID, packet->rtpPrefix.sendTsMs,
			// packet->rtpPrefix.SFUsendMs, packet->rtpPrefix.SFUrecvMs, SFUlatency);
		}

		const uint8_t* data = packet->GetData();
		auto len            = packet->GetSize();

		// 비디오만
		if (packetsendlog)
		{
			if (consumer && consumer->GetKind() == RTC::Media::Kind::VIDEO)
			{
				InitVideoSendLog();

				LogVideoPacketSend(
				  consumer->id,
				  packet->GetSsrc(),
				  packet->GetSequenceNumber(),
				  packet->GetTimestamp(),
				  packet->HasMarker(),
				  packet->GetSize());
			}
		}

		if (!this->srtpSendSession->EncryptRtp(&data, &len))
		{
			if (cb)
			{
				(*cb)(false);
				delete cb;
			}

			return;
		}

		this->iceServer->GetSelectedTuple()->Send(data, len, cb);

		// ---- frame record store begin ----
		if (this->frameRecordTable && this->networkState)
		{
			const uint32_t frameId         = packet->GetTimestamp();
			const bool isLastPacketOfFrame = packet->HasMarker();
			const size_t packetSize        = packet->GetSize();
			const uint64_t nowMs           = DepLibUV::GetTimeMs();

			int frameType             = -1; // 1=key, 0=inter, -1=unknown
			int temporalLayer         = -1; // -1=unknown
			int spatialLayer          = -1;
			int currentSpatialLayer   = -1;
			int targetSpatialLayer    = -1;
			int preferredSpatialLayer = -1;

			if (consumer)
			{
				currentSpatialLayer   = consumer->GetCurrentSpatialLayer();
				targetSpatialLayer    = consumer->GetTargetSpatialLayer();
				preferredSpatialLayer = consumer->GetPreferredSpatialLayer();
			}

			if (packet->GetPayload() && packet->GetPayloadLength() > 0)
			{
				frameType = GetVp8FrameType(packet->GetPayload(), packet->GetPayloadLength());
			}

			if (packet->GetPayload())
			{
				temporalLayer = GetVp8Tid(packet->GetPayload(), packet->GetPayloadLength());
			}

			auto snapshot = this->networkState->GetSnapshot();

			if (this->rtpPacer)
			{
				snapshot.pacingBacklogBytes = static_cast<double>(this->rtpPacer->queuedBytes);
			}

			const bool pacingEnabled = ispacing.load();

			this->frameRecordTable->OnPacketSent(
			  frameId,
			  packetSize,
			  isLastPacketOfFrame,
			  frameType,
			  temporalLayer,
			  spatialLayer,
			  currentSpatialLayer,
			  targetSpatialLayer,
			  preferredSpatialLayer,
			  pacingEnabled,
			  nowMs,
			  snapshot);

			// if (this->currentPredictedSlack.has_value())
			// {
			// 	// MS_ERROR_STD(
			// 	//   "[SLACK-PREDICT] frame ts:%" PRIu32 " predicted slack: %.3f ms",
			// 	//   ts,
			// 	//   this->currentPredictedSlack.value());

			// 	const uint32_t frameId = packet->GetTimestamp();
			// 	this->frameRecordTable->AttachPredictedSlack(frameId, this->currentPredictedSlack.value());
			// }
			if (isLastPacketOfFrame)
			{
				auto it = this->pendingPredictedSlackByFrame.find(frameId);
				if (it != this->pendingPredictedSlackByFrame.end())
				{
					this->frameRecordTable->AttachPredictedSlack(frameId, it->second);
					this->pendingPredictedSlackByFrame.erase(it);
				}
			}
			// 여기에다가 이제 넣기
		}
		// ---- frame record store end ----

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	void WebRtcTransport::SendRtcpPacket(RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		if (!IsConnected())
		{
			return;
		}

		const uint8_t* data = packet->GetData();
		auto len            = packet->GetSize();

		// Ensure there is sending SRTP session.
		if (!this->srtpSendSession)
		{
			MS_WARN_DEV("ignoring RTCP packet due to non sending SRTP session");

			return;
		}

		if (!this->srtpSendSession->EncryptRtcp(&data, &len))
		{
			return;
		}

		this->iceServer->GetSelectedTuple()->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	void WebRtcTransport::SendRtcpCompoundPacket(RTC::RTCP::CompoundPacket* packet)
	{
		MS_TRACE();

		if (!IsConnected())
		{
			return;
		}

		packet->Serialize(RTC::RTCP::Buffer);

		const uint8_t* data = packet->GetData();
		auto len            = packet->GetSize();

		// Ensure there is sending SRTP session.
		if (!this->srtpSendSession)
		{
			MS_WARN_TAG(rtcp, "ignoring RTCP compound packet due to non sending SRTP session");

			return;
		}

		if (!this->srtpSendSession->EncryptRtcp(&data, &len))
		{
			return;
		}

		this->iceServer->GetSelectedTuple()->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	void WebRtcTransport::SendMessage(
	  RTC::DataConsumer* dataConsumer, const uint8_t* msg, size_t len, uint32_t ppid, onQueuedCallback* cb)
	{
		MS_TRACE();
		MS_ERROR_STD("SendMessage");
		this->sctpAssociation->SendSctpMessage(dataConsumer, msg, len, ppid, cb);
	}

	void WebRtcTransport::SendSctpData(const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("SendSctpData");
		// clang-format on
		if (!IsConnected())
		{
			MS_WARN_TAG(sctp, "DTLS not connected, cannot send SCTP data");

			return;
		}

// TODO: For testing purposes. Must be removed.
#ifdef MS_SCTP_STACK
		MS_DUMP(">>> sending SCTP packet...");

		auto* packet = RTC::SCTP::Packet::Parse(data, len);

		if (!packet)
		{
			MS_WARN_TAG(sctp, "data to be sent is not a valid SCTP packet");

			return;
		}

		packet->Dump();

		delete packet;
#endif

		this->dtlsTransport->SendApplicationData(data, len);
	}

	void WebRtcTransport::RecvStreamClosed(uint32_t ssrc)
	{
		MS_TRACE();

		if (this->srtpRecvSession)
		{
			this->srtpRecvSession->RemoveStream(ssrc);
		}
	}

	void WebRtcTransport::SendStreamClosed(uint32_t ssrc)
	{
		MS_TRACE();

		if (this->srtpSendSession)
		{
			this->srtpSendSession->RemoveStream(ssrc);
		}
	}

	inline void WebRtcTransport::OnPacketReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("debug");
		//   Increase receive transmission.
		RTC::Transport::DataReceived(len);

		// Check if it's STUN.
		if (RTC::StunPacket::IsStun(data, len))
		{
			OnStunDataReceived(tuple, data, len);
		}
		// Check if it's RTCP.
		else if (RTC::RTCP::Packet::IsRtcp(data, len))
		{
			OnRtcpDataReceived(tuple, data, len);
		}
		// Check if it's RTP.
		else if (RTC::RtpPacket::IsRtp(data, len))
		{
			OnRtpDataReceived(tuple, data, len);
		}
		// Check if it's DTLS.
		else if (RTC::DtlsTransport::IsDtls(data, len))
		{
			OnDtlsDataReceived(tuple, data, len);
		}
		else
		{
			MS_WARN_DEV("ignoring received packet of unknown type");
		}
	}

	inline void WebRtcTransport::OnStunDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		RTC::StunPacket* packet = RTC::StunPacket::Parse(data, len);

		if (!packet)
		{
			MS_WARN_DEV("ignoring wrong STUN packet received");

			return;
		}

		// Pass it to the IceServer.
		this->iceServer->ProcessStunPacket(packet, tuple);

		delete packet;
	}

	inline void WebRtcTransport::OnDtlsDataReceived(
	  const RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(dtls, "ignoring DTLS data coming from an invalid tuple");

			return;
		}

		// Trick for clients performing aggressive ICE regardless we are ICE-Lite.
		this->iceServer->MayForceSelectedTuple(tuple);

		// Check that DTLS status is 'connecting' or 'connected'.
		if (
		  this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTING ||
		  this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_DEV("DTLS data received, passing it to the DTLS transport");
			// MS_ERROR_STD("DTLS data received, passing it to the DTLS transport");
			this->dtlsTransport->ProcessDtlsData(data, len);
		}
		else
		{
			MS_WARN_TAG(dtls, "Transport is not 'connecting' or 'connected', ignoring received DTLS data");

			return;
		}
	}

	inline void WebRtcTransport::OnRtpDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD("debug");
		//  Ensure DTLS is connected.
		if (this->dtlsTransport->GetState() != RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_2TAGS(dtls, rtp, "ignoring RTP packet while DTLS not connected");

			return;
		}

		// Ensure there is receiving SRTP session.
		if (!this->srtpRecvSession)
		{
			MS_DEBUG_TAG(srtp, "ignoring RTP packet due to non receiving SRTP session");

			return;
		}

		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(rtp, "ignoring RTP packet coming from an invalid tuple");

			return;
		}

		// Decrypt the SRTP packet.
		if (!this->srtpRecvSession->DecryptSrtp(const_cast<uint8_t*>(data), &len)) // d
		{
			RTC::RtpPacket* packet = RTC::RtpPacket::Parse(data, len);

			if (!packet)
			{
				MS_WARN_TAG(srtp, "DecryptSrtp() failed due to an invalid RTP packet");
			}
			else
			{
				MS_WARN_TAG(
				  srtp,
				  "DecryptSrtp() failed [ssrc:%" PRIu32 ", payloadType:%" PRIu8 ", seq:%" PRIu16 "]",
				  packet->GetSsrc(),
				  packet->GetPayloadType(),
				  packet->GetSequenceNumber());

				delete packet;
			}

			return;
		}

		RTC::RtpPacket* packet = RTC::RtpPacket::Parse(data, len);

		if (!packet)
		{
			MS_WARN_TAG(rtp, "received data is not a valid RTP packet");

			return;
		}

		// 추가: SFU RTP 패킷 페이로드 확인 및 prefix를 찾는 코드
		// 패킷이 복호화되고 나서부터 데이터를 읽을 수 있으므로 그 이후 진행
		if (ParseFramePrefixforReceive(packet->GetPayload(), packet->GetPayloadLength(), &packet->rtpPrefix))
		{
		}

		// Trick for clients performing aggressive ICE regardless we are ICE-Lite.
		this->iceServer->MayForceSelectedTuple(tuple);

		// 추가: SFU latency 측정을 위한 코드
		auto nowUs = DepLibUV::GetTimeUsInt64();
		packet->SetReceivedAtUs(nowUs);
		// Pass the packet to the parent transport.
		RTC::Transport::ReceiveRtpPacket(packet);
	}

	inline void WebRtcTransport::OnRtcpDataReceived(
	  RTC::TransportTuple* tuple, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		// Ensure DTLS is connected.
		if (this->dtlsTransport->GetState() != RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			MS_DEBUG_2TAGS(dtls, rtcp, "ignoring RTCP packet while DTLS not connected");

			return;
		}

		// Ensure there is receiving SRTP session.
		if (!this->srtpRecvSession)
		{
			MS_DEBUG_TAG(srtp, "ignoring RTCP packet due to non receiving SRTP session");

			return;
		}

		// Ensure it comes from a valid tuple.
		if (!this->iceServer->IsValidTuple(tuple))
		{
			MS_WARN_TAG(rtcp, "ignoring RTCP packet coming from an invalid tuple");

			return;
		}

		// Decrypt the SRTCP packet.
		if (!this->srtpRecvSession->DecryptSrtcp(const_cast<uint8_t*>(data), &len))
		{
			return;
		}

		RTC::RTCP::Packet* packet = RTC::RTCP::Packet::Parse(data, len);

		if (!packet)
		{
			MS_WARN_TAG(rtcp, "received data is not a valid RTCP compound or single packet");

			return;

			// MS_ERROR_STD("RTCP type: %u",
			// 	static_cast<unsigned>(static_cast<uint8_t>(packet->GetType())));
		}

		// Pass the packet to the parent transport.
		RTC::Transport::ReceiveRtcpPacket(packet);
	}

	inline void WebRtcTransport::OnUdpSocketPacketReceived(
	  RTC::UdpSocket* socket, const uint8_t* data, size_t len, const struct sockaddr* remoteAddr)
	{
		MS_TRACE();
		// MS_ERROR_STD("debug");
		RTC::TransportTuple tuple(socket, remoteAddr);

		OnPacketReceived(&tuple, data, len);
	}

	inline void WebRtcTransport::OnRtcTcpConnectionClosed(
	  RTC::TcpServer* /*tcpServer*/, RTC::TcpConnection* connection)
	{
		MS_TRACE();

		RTC::TransportTuple tuple(connection);

		this->iceServer->RemoveTuple(&tuple);
	}

	inline void WebRtcTransport::OnTcpConnectionPacketReceived(
	  RTC::TcpConnection* connection, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		////MS_ERROR_STD("debug");
		RTC::TransportTuple tuple(connection);

		OnPacketReceived(&tuple, data, len);
	}

	inline void WebRtcTransport::OnIceServerSendStunPacket(
	  const RTC::IceServer* /*iceServer*/, const RTC::StunPacket* packet, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		// Send the STUN response over the same transport tuple.
		tuple->Send(packet->GetData(), packet->GetSize());

		// Increase send transmission.
		RTC::Transport::DataSent(packet->GetSize());
	}

	inline void WebRtcTransport::OnIceServerLocalUsernameFragmentAdded(
	  const RTC::IceServer* /*iceServer*/, const std::string& usernameFragment)
	{
		MS_TRACE();

		if (this->webRtcTransportListener)
		{
			this->webRtcTransportListener->OnWebRtcTransportLocalIceUsernameFragmentAdded(
			  this, usernameFragment);
		}
	}

	inline void WebRtcTransport::OnIceServerLocalUsernameFragmentRemoved(
	  const RTC::IceServer* /*iceServer*/, const std::string& usernameFragment)
	{
		MS_TRACE();

		if (this->webRtcTransportListener)
		{
			this->webRtcTransportListener->OnWebRtcTransportLocalIceUsernameFragmentRemoved(
			  this, usernameFragment);
		}
	}

	inline void WebRtcTransport::OnIceServerTupleAdded(
	  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		if (this->webRtcTransportListener)
		{
			this->webRtcTransportListener->OnWebRtcTransportTransportTupleAdded(this, tuple);
		}
	}

	inline void WebRtcTransport::OnIceServerTupleRemoved(
	  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		if (this->webRtcTransportListener)
		{
			this->webRtcTransportListener->OnWebRtcTransportTransportTupleRemoved(this, tuple);
		}

		// If this is a TCP tuple, close its underlaying TCP connection.
		if (tuple->GetProtocol() == RTC::TransportTuple::Protocol::TCP)
		{
			tuple->CloseTcpConnection();
		}
	}

	inline void WebRtcTransport::OnIceServerSelectedTuple(
	  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* /*tuple*/)
	{
		MS_TRACE();

		/*
		 * RFC 5245 section 11.2 "Receiving Media":
		 *
		 * ICE implementations MUST be prepared to receive media on each component
		 * on any candidates provided for that component.
		 */

		MS_DEBUG_TAG(ice, "ICE selected tuple");

		// Notify the Node WebRtcTransport.
		auto tuple = this->iceServer->GetSelectedTuple()->FillBuffer(
		  this->shared->channelNotifier->GetBufferBuilder());

		auto notification = FBS::WebRtcTransport::CreateIceSelectedTupleChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), tuple);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_ICE_SELECTED_TUPLE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_IceSelectedTupleChangeNotification,
		  notification);
	}

	inline void WebRtcTransport::OnIceServerConnected(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(ice, "ICE connected");

		// Notify the Node WebRtcTransport.
		auto iceStateChangeOffset = FBS::WebRtcTransport::CreateIceStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::WebRtcTransport::IceState::CONNECTED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_ICE_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_IceStateChangeNotification,
		  iceStateChangeOffset);

		// If ready, run the DTLS handler.
		MayRunDtlsTransport();

		// If DTLS was already connected, notify the parent class.
		if (this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			RTC::Transport::Connected();
		}
	}

	inline void WebRtcTransport::OnIceServerCompleted(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(ice, "ICE completed");

		// Notify the Node WebRtcTransport.
		auto iceStateChangeOffset = FBS::WebRtcTransport::CreateIceStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::WebRtcTransport::IceState::COMPLETED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_ICE_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_IceStateChangeNotification,
		  iceStateChangeOffset);

		// If ready, run the DTLS handler.
		MayRunDtlsTransport();

		// If DTLS was already connected, notify the parent class.
		if (this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			RTC::Transport::Connected();
		}
	}

	inline void WebRtcTransport::OnIceServerDisconnected(const RTC::IceServer* /*iceServer*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(ice, "ICE disconnected");

		// Notify the Node WebRtcTransport.
		auto iceStateChangeOffset = FBS::WebRtcTransport::CreateIceStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::WebRtcTransport::IceState::DISCONNECTED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_ICE_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_IceStateChangeNotification,
		  iceStateChangeOffset);

		// If DTLS was already connected, notify the parent class.
		if (this->dtlsTransport->GetState() == RTC::DtlsTransport::DtlsState::CONNECTED)
		{
			RTC::Transport::Disconnected();
		}
	}

	inline void WebRtcTransport::OnDtlsTransportConnecting(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_DEBUG_TAG(dtls, "DTLS connecting");

		// Notify the Node WebRtcTransport.
		auto dtlsStateChangeOffset = FBS::WebRtcTransport::CreateDtlsStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::WebRtcTransport::DtlsState::CONNECTING);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_DTLS_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_DtlsStateChangeNotification,
		  dtlsStateChangeOffset);
	}

	inline void WebRtcTransport::OnDtlsTransportConnected(
	  const RTC::DtlsTransport* /*dtlsTransport*/,
	  RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
	  uint8_t* srtpLocalKey,
	  size_t srtpLocalKeyLen,
	  uint8_t* srtpRemoteKey,
	  size_t srtpRemoteKeyLen,
	  std::string& remoteCert)
	{
		MS_TRACE();

		MS_DEBUG_TAG(dtls, "DTLS connected");

		// Close it if it was already set and update it.
		delete this->srtpSendSession;
		this->srtpSendSession = nullptr;

		delete this->srtpRecvSession;
		this->srtpRecvSession = nullptr;

		try
		{
			this->srtpSendSession = new RTC::SrtpSession(
			  RTC::SrtpSession::Type::OUTBOUND, srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen);
		}
		catch (const MediaSoupError& error)
		{
			MS_ERROR("error creating SRTP sending session: %s", error.what());
		}

		try
		{
			this->srtpRecvSession = new RTC::SrtpSession(
			  RTC::SrtpSession::Type::INBOUND, srtpCryptoSuite, srtpRemoteKey, srtpRemoteKeyLen);

			// Notify the Node WebRtcTransport.
			auto dtlsStateChangeOffset = FBS::WebRtcTransport::CreateDtlsStateChangeNotificationDirect(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  FBS::WebRtcTransport::DtlsState::CONNECTED,
			  remoteCert.c_str());

			this->shared->channelNotifier->Emit(
			  this->id,
			  FBS::Notification::Event::WEBRTCTRANSPORT_DTLS_STATE_CHANGE,
			  FBS::Notification::Body::WebRtcTransport_DtlsStateChangeNotification,
			  dtlsStateChangeOffset);

			// Tell the parent class.
			RTC::Transport::Connected();
		}
		catch (const MediaSoupError& error)
		{
			MS_ERROR("error creating SRTP receiving session: %s", error.what());

			delete this->srtpSendSession;
			this->srtpSendSession = nullptr;
		}
	}

	inline void WebRtcTransport::OnDtlsTransportFailed(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_WARN_TAG(dtls, "DTLS failed");

		// Notify the Node WebRtcTransport.
		auto dtlsStateChangeOffset = FBS::WebRtcTransport::CreateDtlsStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::WebRtcTransport::DtlsState::FAILED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_DTLS_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_DtlsStateChangeNotification,
		  dtlsStateChangeOffset);
	}

	inline void WebRtcTransport::OnDtlsTransportClosed(const RTC::DtlsTransport* /*dtlsTransport*/)
	{
		MS_TRACE();

		MS_WARN_TAG(dtls, "DTLS remotely closed");

		// Notify the Node WebRtcTransport.
		auto dtlsStateChangeOffset = FBS::WebRtcTransport::CreateDtlsStateChangeNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), FBS::WebRtcTransport::DtlsState::CLOSED);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::WEBRTCTRANSPORT_DTLS_STATE_CHANGE,
		  FBS::Notification::Body::WebRtcTransport_DtlsStateChangeNotification,
		  dtlsStateChangeOffset);

		// Tell the parent class.
		RTC::Transport::Disconnected();
	}

	inline void WebRtcTransport::OnDtlsTransportSendData(
	  const RTC::DtlsTransport* /*dtlsTransport*/, const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (!this->iceServer->GetSelectedTuple())
		{
			MS_WARN_TAG(dtls, "no selected tuple set, cannot send DTLS packet");

			return;
		}

		this->iceServer->GetSelectedTuple()->Send(data, len);

		// Increase send transmission.
		RTC::Transport::DataSent(len);
	}

	inline void WebRtcTransport::SendTextMessageToSctpStream(uint16_t sid, const std::string& payload)
	{
		MS_TRACE();

		// ----------------------------------------------------------------
		// TODO: 반드시 당신 코드베이스의 실제 SCTP 송신 경로로 연결해야 함.
		//
		// 예시 의도:
		// - sid 를 그대로 사용
		// - PPID = 51 (WebRTC string)
		// - ordered/reliability는 기존 data producer 설정에 맞춤
		// - 현재 transport(peer)에게 응답 전송
		//
		// 가능한 구현 위치 예:
		// - SCTP association send API
		// - DataProducer/DataConsumer send 경로
		// - transport 내부의 SendSctpMessage(...) 류 API
		// ----------------------------------------------------------------

		MS_WARN_TAG(
		  sctp,
		  "SendTextMessageToSctpStream() is a stub. Connect it to your real SCTP send path [sid:%u, payload:%s]",
		  sid,
		  payload.c_str());
	}

	inline void WebRtcTransport::SendSyncResponse(uint16_t sid, uint32_t seq, double t1ViewMs, double t2SfuMs)
	{
		std::string payload = std::string("{\"type\":\"sync_resp\",\"seq\":") + std::to_string(seq) +
		                      ",\"t1ViewMs\":" + std::to_string(t1ViewMs) +
		                      ",\"t2SfuMs\":" + std::to_string(t2SfuMs) + "}";

		MS_DEBUG_TAG(
		  sctp,
		  "[APPMSG] sending sync_resp [sid:%u, seq:%u, t1ViewMs:%f, t2SfuMs:%f]",
		  sid,
		  seq,
		  t1ViewMs,
		  t2SfuMs);

		// ----------------------------------------------------------------
		// 중요: 실제 sctp를 보낼 수 있는 함수를 호출헤야 함
		// ----------------------------------------------------------------
		this->SendTextMessageToSctpStream(sid, payload);
	}

	inline void WebRtcTransport::HandleParsedAppMessage(const ParsedSctpAppMessage& msg)
	{
		switch (msg.kind)
		{
			case AppMessageKind::Latency:
			{
				MS_DEBUG_TAG(
				  sctp,
				  "[APPMSG] latency received [sid:%u, seq:%u, s2cMs:%f, text:%s]",
				  msg.sid,
				  msg.seq,
				  msg.s2cMs,
				  msg.text.c_str());

				// TODO:
				// 기존 latency 저장/분석 로직이 있다면 여기에 연결
				break;
			}

			case AppMessageKind::SyncReq:
			{
				const double t2SfuMs = GetMonotonicTimeMs();

				MS_DEBUG_TAG(
				  sctp,
				  "[APPMSG] sync_req received [sid:%u, seq:%u, t1ViewMs:%f]",
				  msg.sid,
				  msg.seq,
				  msg.t1ViewMs);

				this->SendSyncResponse(msg.sid, msg.seq, msg.t1ViewMs, t2SfuMs);
				break;
			}

			case AppMessageKind::SyncResp:
			{
				MS_DEBUG_TAG(
				  sctp,
				  "[APPMSG] sync_resp received [sid:%u, seq:%u, t2SfuMs:%f]",
				  msg.sid,
				  msg.seq,
				  msg.t2SfuMs);

				// viewer-initiated sync 구조면
				// SFU가 sync_resp를 받을 일은 보통 없음.
				break;
			}

			case AppMessageKind::Chat:
			{
				// MS_DEBUG_TAG(sctp, "[APPMSG] chat received [sid:%u]", msg.sid);
				break;
			}

			case AppMessageKind::Unknown:
			case AppMessageKind::None:
			default:
			{
				break;
			}
		}
	}

	inline void WebRtcTransport::OnDtlsTransportApplicationDataReceived(
	  const RTC::DtlsTransport* /*dtlsTransport*/, const uint8_t* data, size_t len)
	{
		MS_TRACE();
		// MS_ERROR_STD();
// TODO: For testing purposes. Must be removed.
#ifdef MS_SCTP_STACK
		MS_DUMP("<<< receiving SCTP packet...");

		// 1) SCTP DATA chunk에서 "hello" 같은 payload 확인(디버그)

		auto* packet = RTC::SCTP::Packet::Parse(data, len);

		if (!packet)
		{
			MS_WARN_TAG(sctp, "received data is not a valid SCTP packet");

			return;
		}

		packet->Dump();

		delete packet;
#endif
		// SctpData에서 필요한 데이터 추출 (latency). 이외에도 컨트롤 메세지나 chat 데이터가 올 수 있음
		auto parsed = ParseSctpData(data, len);

		if (parsed.has_value() && parsed->valid)
		{
			this->HandleParsedAppMessage(*parsed);
		}
		// Pass it to the parent transport.
		RTC::Transport::ReceiveSctpData(data, len);
	}
} // namespace RTC
