#include "../tests_common.h"

#include "src/rtsp_limits.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace {
  constexpr std::size_t encrypted_header_size = 24;  // type and length (4), sequence number (4), GCM tag (16)
}

TEST(RtspMessageLimit, AnAnnounceOfSeveralKilobytesFits) {
  // an ANNOUNCE with automatic FEC and bitrate went past the old 2048-byte limit
  for (const std::size_t payload : {std::size_t {1500}, std::size_t {2048}, std::size_t {2100}, std::size_t {4096}, std::size_t {8192}}) {
    EXPECT_TRUE(rtsp_stream::rtsp_message_fits(rtsp_stream::RTSP_MAX_MESSAGE_SIZE, 0, encrypted_header_size, payload)) << payload;
  }
  EXPECT_GE(rtsp_stream::RTSP_MAX_MESSAGE_SIZE, std::size_t {16 * 1024});
}

TEST(RtspMessageLimit, TheLimitStaysSmall) {
  // a peer that is not known yet makes the host hold this much: it must not grow into a memory sink
  EXPECT_LE(rtsp_stream::RTSP_MAX_MESSAGE_SIZE, std::size_t {64 * 1024});
}

TEST(RtspMessageLimit, TheLastByteFitsAndOneMoreDoesNot) {
  constexpr auto capacity = rtsp_stream::RTSP_MAX_MESSAGE_SIZE;
  constexpr auto room = capacity - encrypted_header_size;
  EXPECT_TRUE(rtsp_stream::rtsp_message_fits(capacity, 0, encrypted_header_size, room));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(capacity, 0, encrypted_header_size, room + 1));
}

TEST(RtspMessageLimit, ThePositionInTheBufferCounts) {
  constexpr auto capacity = rtsp_stream::RTSP_MAX_MESSAGE_SIZE;
  EXPECT_TRUE(rtsp_stream::rtsp_message_fits(capacity, 100, encrypted_header_size, capacity - 100 - encrypted_header_size));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(capacity, 100, encrypted_header_size, capacity - 100 - encrypted_header_size + 1));
}

TEST(RtspMessageLimit, ALengthTheHeaderCannotEvenHoldIsRefusedWithoutWrapping) {
  constexpr auto capacity = rtsp_stream::RTSP_MAX_MESSAGE_SIZE;
  // the encrypted header declares up to 2^31 - 1 bytes; a sum with the buffer position must not wrap and let it through
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(capacity, 0, encrypted_header_size, 0x7fffffffu));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(capacity, 0, encrypted_header_size, std::numeric_limits<std::uint64_t>::max()));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(capacity, 0, encrypted_header_size, std::numeric_limits<std::uint64_t>::max() - encrypted_header_size + 1));
}

TEST(RtspMessageLimit, ABufferWithoutRoomForTheHeaderRefusesEverything) {
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(16, 0, encrypted_header_size, 0));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(100, 90, encrypted_header_size, 0));
  EXPECT_FALSE(rtsp_stream::rtsp_message_fits(100, 101, 0, 0));  // used beyond the end
  EXPECT_TRUE(rtsp_stream::rtsp_message_fits(100, 76, encrypted_header_size, 0));
}

TEST(RtspMessageLimit, TheLogLineNamesThePeerTheLengthAndTheLimit) {
  const auto line = rtsp_stream::describe_oversized_rtsp_message("handle_read_encrypted_header()", "192.0.2.7", 20'000, rtsp_stream::RTSP_MAX_MESSAGE_SIZE);
  EXPECT_NE(line.find("handle_read_encrypted_header()"), std::string::npos) << line;
  EXPECT_NE(line.find("192.0.2.7"), std::string::npos) << line;
  EXPECT_NE(line.find("20000"), std::string::npos) << line;
  EXPECT_NE(line.find(std::to_string(rtsp_stream::RTSP_MAX_MESSAGE_SIZE)), std::string::npos) << line;
  EXPECT_EQ(line.find("at least"), std::string::npos) << line;
}

TEST(RtspMessageLimit, ALengthThatIsOnlyTheBytesReadSoFarSaysSo) {
  const auto line = rtsp_stream::describe_oversized_rtsp_message("read()", "198.51.100.4", 16'384, 16'384, false);
  EXPECT_NE(line.find("is at least 16384 bytes"), std::string::npos) << line;
}

TEST(RtspMessageLimit, AnUnknownPeerIsNamedAsSuch) {
  const auto line = rtsp_stream::describe_oversized_rtsp_message("read()", "", 1, 2);
  EXPECT_NE(line.find("(unknown)"), std::string::npos) << line;
}
