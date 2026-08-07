#ifndef WEBRTC_MODULES_RTP_RTCP_SOURCE_MEDIASOUP_FEC_LOGGING_H_
#define WEBRTC_MODULES_RTP_RTCP_SOURCE_MEDIASOUP_FEC_LOGGING_H_

#include <ostream>
#include <streambuf>

namespace webrtc
{
	namespace mediasoup_fec_compat
	{
		class NullLogBuffer : public std::streambuf
		{
		protected:
			int overflow(int value) override
			{
				return value;
			}
		};

		inline std::ostream& NullLogStream()
		{
			static NullLogBuffer buffer;
			static std::ostream stream(&buffer);

			return stream;
		}
	} // namespace mediasoup_fec_compat
} // namespace webrtc

// Imported WebRTC FEC code uses RTC_LOG only for diagnostic messages.
// Do not import the complete WebRTC logging subsystem into mediasoup.
#ifndef RTC_LOG
#define RTC_LOG(severity) \
	::webrtc::mediasoup_fec_compat::NullLogStream()
#endif

#endif
