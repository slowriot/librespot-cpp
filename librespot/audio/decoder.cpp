#include "decoder.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

namespace librespot::audio {
namespace {

void check(int result, std::string const &operation) {
  /// Translate FFmpeg errors at the library boundary
  if(result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error{operation + ": " + message};
}

sample_format convert_format(AVSampleFormat format) {
  /// Preserve the decoder's native precision and storage representation
  switch(av_get_packed_sample_fmt(format)) {
  case AV_SAMPLE_FMT_U8: return sample_format::unsigned_8;
  case AV_SAMPLE_FMT_S16: return sample_format::signed_16;
  case AV_SAMPLE_FMT_S32: return sample_format::signed_32;
  case AV_SAMPLE_FMT_S64: return sample_format::signed_64;
  case AV_SAMPLE_FMT_FLT: return sample_format::float_32;
  case AV_SAMPLE_FMT_DBL: return sample_format::float_64;
  default: throw std::runtime_error{"unsupported decoded sample format"};
  }
}

struct frame_deleter {
  void operator()(AVFrame *frame) const noexcept {
    av_frame_free(&frame);
  }
};

struct packet_deleter {
  void operator()(AVPacket *packet) const noexcept {
    av_packet_free(&packet);
  }
};

struct codec_deleter {
  void operator()(AVCodecContext *codec) const noexcept {
    avcodec_free_context(&codec);
  }
};

} // anonymous namespace

struct pcm_frame::implementation {
  std::unique_ptr<AVFrame, frame_deleter> frame{av_frame_alloc()};
  unsigned int significant_bits{0};
  AVRational time_base{0, 1};
};

struct decoder::implementation {
  AVFormatContext *container{nullptr};
  AVIOContext *io{nullptr};
  std::shared_ptr<std::vector<std::byte> const> bytes;
  std::int64_t offset{0};
  std::unique_ptr<AVCodecContext, codec_deleter> codec;
  std::unique_ptr<AVPacket, packet_deleter> packet{av_packet_alloc()};
  int stream_index{-1};
  bool draining{false};
  std::optional<std::int64_t> seek_target;

  ~implementation() {
    /// Release custom IO separately because the format context does not own it
    avformat_close_input(&container);
    if(io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }

  static int read(void *opaque, std::uint8_t *buffer, int length) noexcept {
    /// Supply bounded memory reads without permitting exceptions across the C ABI
    auto &self{*static_cast<implementation *>(opaque)};
    if(length <= 0) return AVERROR(EINVAL);
    auto const remaining{self.bytes->size() - static_cast<std::size_t>(self.offset)};
    auto const count{std::min(remaining, static_cast<std::size_t>(length))};
    if(count == 0) return AVERROR_EOF;
    std::memcpy(buffer, self.bytes->data() + self.offset, count);
    self.offset += static_cast<std::int64_t>(count);
    return static_cast<int>(count);
  }

  static std::int64_t seek(void *opaque, std::int64_t offset, int origin) noexcept {
    /// Implement FFmpeg's size query and checked absolute or relative memory seeks
    auto &self{*static_cast<implementation *>(opaque)};
    auto const size{static_cast<std::int64_t>(self.bytes->size())};
    if(origin == AVSEEK_SIZE) return size;
    origin &= ~AVSEEK_FORCE;
    std::int64_t base{0};
    if(origin == SEEK_CUR) base = self.offset;
    else if(origin == SEEK_END) base = size;
    else if(origin != SEEK_SET) return AVERROR(EINVAL);
    if(offset < -base || offset > size - base) return AVERROR(EINVAL);
    self.offset = base + offset;
    return self.offset;
  }

  void open(char const *path) {
    /// Select one audio stream and open its native decoder
    if(!packet) throw std::bad_alloc{};
    check(avformat_open_input(&container, path, nullptr, nullptr), "open audio container");
    check(avformat_find_stream_info(container, nullptr), "read audio stream information");
    AVCodec const *selected{nullptr};
    stream_index = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, &selected, 0);
    check(stream_index, "find audio stream");
    codec.reset(avcodec_alloc_context3(selected));
    if(!codec) throw std::bad_alloc{};
    check(avcodec_parameters_to_context(codec.get(), container->streams[stream_index]->codecpar), "configure audio decoder");
    check(avcodec_open2(codec.get(), selected, nullptr), "open audio decoder");
  }
};

pcm_frame::pcm_frame(std::unique_ptr<implementation> state) : state{std::move(state)} {
  /// Transfer ownership of the decoded sample buffers
}

pcm_frame::~pcm_frame() = default;
pcm_frame::pcm_frame(pcm_frame &&) noexcept = default;
pcm_frame &pcm_frame::operator=(pcm_frame &&) noexcept = default;

pcm_format pcm_frame::format() const {
  auto const &frame{*state->frame};
  auto const format{static_cast<AVSampleFormat>(frame.format)};
  return {
    .samples{convert_format(format)},
    .sample_rate{static_cast<unsigned int>(frame.sample_rate)},
    .channels{static_cast<unsigned int>(frame.ch_layout.nb_channels)},
    .significant_bits{state->significant_bits},
    .planar{av_sample_fmt_is_planar(format) != 0},
  };
}

std::size_t pcm_frame::sample_count() const {
  return static_cast<std::size_t>(state->frame->nb_samples);
}

std::optional<std::chrono::microseconds> pcm_frame::position() const {
  /// Express the frame timestamp in the stream timeline when one is available
  auto const timestamp{state->frame->best_effort_timestamp};
  if(timestamp == AV_NOPTS_VALUE) return std::nullopt;
  return std::chrono::microseconds{av_rescale_q(timestamp, state->time_base, AVRational{1, 1'000'000})};
}

std::size_t pcm_frame::plane_count() const {
  return format().planar ? format().channels : 1;
}

std::span<std::byte const> pcm_frame::plane(std::size_t index) const {
  /// Expose only sample bytes, excluding FFmpeg's alignment padding
  if(index >= plane_count()) throw std::out_of_range{"PCM plane index"};
  auto const bytes_per_sample{av_get_bytes_per_sample(static_cast<AVSampleFormat>(state->frame->format))};
  auto const size{sample_count() * static_cast<std::size_t>(bytes_per_sample) * (format().planar ? 1u : format().channels)};
  return {reinterpret_cast<std::byte const *>(state->frame->extended_data[index]), size};
}

decoder::decoder(std::filesystem::path const &path) : state{std::make_unique<implementation>()} {
  /// Open a local file; network retrieval belongs to the streaming layer
  state->open(path.c_str());
}

decoder::decoder(std::shared_ptr<std::vector<std::byte> const> bytes) : state{std::make_unique<implementation>()} {
  /// Retain encoded bytes without copying them and attach a seekable custom input
  if(!bytes || bytes->empty() || bytes->size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument{"invalid encoded audio buffer"};
  }
  state->bytes = std::move(bytes);
  auto const buffer{static_cast<unsigned char *>(av_malloc(32 * 1024))};
  if(!buffer) throw std::bad_alloc{};
  state->io = avio_alloc_context(buffer, 32 * 1024, 0, state.get(), implementation::read, nullptr, implementation::seek);
  if(!state->io) {
    av_free(buffer);
    throw std::bad_alloc{};
  }
  state->container = avformat_alloc_context();
  if(!state->container) throw std::bad_alloc{};
  state->container->pb = state->io;
  state->container->flags |= AVFMT_FLAG_CUSTOM_IO;
  state->open(nullptr);
}

decoder::~decoder() = default;
decoder::decoder(decoder &&) noexcept = default;
decoder &decoder::operator=(decoder &&) noexcept = default;

std::optional<pcm_frame> decoder::next() {
  /// Drain complete decoded frames before reading another compressed packet
  auto frame{std::make_unique<pcm_frame::implementation>()};
  if(!frame->frame) throw std::bad_alloc{};
  frame->significant_bits = static_cast<unsigned int>(state->codec->bits_per_raw_sample);
  frame->time_base = state->container->streams[state->stream_index]->time_base;
  for(;;) {
    auto const result{avcodec_receive_frame(state->codec.get(), frame->frame.get())};
    if(result == 0) {
      if(state->seek_target) {
        auto const timestamp{frame->frame->best_effort_timestamp};
        if(timestamp == AV_NOPTS_VALUE) throw std::runtime_error{"cannot locate decoded frame after seeking"};
        auto const delta{*state->seek_target - timestamp};
        auto const skip{delta > 0 ? av_rescale_q_rnd(delta, frame->time_base,
          AVRational{1, frame->frame->sample_rate}, AV_ROUND_UP) : 0};
        if(skip >= frame->frame->nb_samples) {
          av_frame_unref(frame->frame.get());
          continue;
        }
        if(skip > 0) {
          auto const planar{av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->frame->format)) != 0};
          auto const planes{planar ? frame->frame->ch_layout.nb_channels : 1};
          auto const stride{av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->frame->format)) * (planar ? 1 : frame->frame->ch_layout.nb_channels)};
          for(int plane{0}; plane < planes; ++plane) frame->frame->extended_data[plane] += skip * stride;
          frame->frame->nb_samples -= static_cast<int>(skip);
          frame->frame->best_effort_timestamp += av_rescale_q(skip, AVRational{1, frame->frame->sample_rate}, frame->time_base);
        }
        state->seek_target.reset();
      }
      return pcm_frame{std::move(frame)};
    }
    if(result == AVERROR_EOF) return std::nullopt;
    if(result != AVERROR(EAGAIN)) check(result, "decode audio frame");
    if(state->draining) throw std::runtime_error{"audio decoder requested input after EOF"};
    int read_result{0};
    do {
      av_packet_unref(state->packet.get());
      read_result = av_read_frame(state->container, state->packet.get());
    } while(read_result >= 0 && state->packet->stream_index != state->stream_index);
    if(read_result == AVERROR_EOF) {
      state->draining = true;
      check(avcodec_send_packet(state->codec.get(), nullptr), "drain audio decoder");
    } else {
      check(read_result, "read encoded audio packet");
      check(avcodec_send_packet(state->codec.get(), state->packet.get()), "submit encoded audio packet");
    }
  }
}

void decoder::seek(std::chrono::microseconds position) {
  /// Reset decoding and trim preroll to the first sample at or after the target
  if(position.count() < 0) throw std::invalid_argument{"negative audio seek position"};
  auto const stream{state->container->streams[state->stream_index]};
  auto const timestamp{av_rescale_q_rnd(position.count(), AVRational{1, 1'000'000}, stream->time_base, AV_ROUND_UP)};
  auto result{avformat_seek_file(state->container, state->stream_index, 0, timestamp, timestamp, 0)};
  if(result < 0) {
    // a final FLAC frame can have no later timestamp to bound a binary seek
    if(auto const entry{avformat_index_get_entry_from_timestamp(stream, timestamp, AVSEEK_FLAG_BACKWARD)}) {
      result = av_seek_frame(state->container, state->stream_index, entry->timestamp, AVSEEK_FLAG_BACKWARD);
    }
  }
  check(result, "seek audio stream");
  avcodec_flush_buffers(state->codec.get());
  av_packet_unref(state->packet.get());
  state->draining = false;
  state->seek_target = timestamp;
}

} // namespace librespot::audio
