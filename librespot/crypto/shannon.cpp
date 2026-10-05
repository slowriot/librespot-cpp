#include "shannon.h"
#include <bit>
#include <limits>
#include <stdexcept>
#include <openssl/crypto.h>

namespace librespot::crypto {
namespace {

std::uint32_t constexpr initial_constant{0x69'96'c5'3au};

std::uint32_t sbox1(std::uint32_t word) {
  word ^= std::rotl(word, 5) | std::rotl(word, 7);
  word ^= std::rotl(word, 19) | std::rotl(word, 22);
  return word;
}

std::uint32_t sbox2(std::uint32_t word) {
  word ^= std::rotl(word, 7) | std::rotl(word, 22);
  word ^= std::rotl(word, 5) | std::rotl(word, 19);
  return word;
}

} // anonymous namespace

shannon::shannon(std::span<std::byte const> key) {
  /// Initialise the feedback register and irreversibly mix in the key
  if(key.empty() || key.size() > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument{"invalid Shannon key length"};
  registers[0] = registers[1] = 1;
  for(std::size_t index{2}; index < registers.size(); ++index) registers[index] = registers[index - 1] + registers[index - 2];
  load_key(key);
  constant = registers[0];
  initial = registers;
}

shannon::~shannon() {
  /// Erase keyed state when a connection is discarded
  OPENSSL_cleanse(registers.data(), sizeof(registers));
  OPENSSL_cleanse(crc.data(), sizeof(crc));
  OPENSSL_cleanse(initial.data(), sizeof(initial));
  OPENSSL_cleanse(&constant, sizeof(constant));
  OPENSSL_cleanse(&stream_word, sizeof(stream_word));
  OPENSSL_cleanse(&mac_word, sizeof(mac_word));
}

void shannon::cycle() {
  /// Advance the nonlinear feedback register and produce one stream word
  auto word{sbox1(registers[12] ^ registers[13] ^ constant) ^ std::rotl(registers[0], 1)};
  for(std::size_t index{1}; index < registers.size(); ++index) registers[index - 1] = registers[index];
  registers.back() = word;
  word = sbox2(registers[2] ^ registers[15]);
  registers[0] ^= word;
  stream_word = word ^ registers[8] ^ registers[12];
}

void shannon::load_key(std::span<std::byte const> key) {
  /// Fold padded little-endian words and the key length into the register
  for(std::size_t offset{0}; offset < key.size(); offset += 4) {
    std::uint32_t word{0};
    for(unsigned int index{0}; index < 4 && offset + index < key.size(); ++index) {
      word |= std::to_integer<std::uint32_t>(key[offset + index]) << (index * 8);
    }
    registers[13] ^= word;
    cycle();
  }
  registers[13] ^= static_cast<std::uint32_t>(key.size());
  cycle();
  crc = registers;
  for(unsigned int index{0}; index < 16; ++index) cycle();
  for(std::size_t index{0}; index < registers.size(); ++index) registers[index] ^= crc[index];
}

void shannon::mac(std::uint32_t word) {
  /// Accumulate plaintext in the parallel CRC and stream register
  auto const feedback{crc[0] ^ crc[2] ^ crc[15] ^ word};
  for(std::size_t index{1}; index < crc.size(); ++index) crc[index - 1] = crc[index];
  crc.back() = feedback;
  registers[13] ^= word;
}

void shannon::nonce(std::uint32_t value) {
  /// Restart a packet with a four-byte big-endian sequence number
  std::array<std::byte, 4> bytes;
  for(unsigned int index{0}; index < 4; ++index) bytes[index] = static_cast<std::byte>((value >> ((3 - index) * 8)) & 255u);
  registers = initial;
  constant = initial_constant;
  load_key(bytes);
  constant = registers[0];
  buffered_bits = 0;
  finished = false;
}

void shannon::process(std::span<std::byte> bytes, bool decrypt) {
  /// Preserve partial words across header and payload calls
  if(finished) throw std::logic_error{"Shannon packet already finished"};
  for(auto &byte : bytes) {
    if(buffered_bits == 0) {
      cycle();
      mac_word = 0;
      buffered_bits = 32;
    }
    auto const shift{32u - buffered_bits};
    auto const mask{static_cast<std::byte>((stream_word >> shift) & 255u)};
    if(decrypt) byte ^= mask;
    mac_word |= std::to_integer<std::uint32_t>(byte) << shift;
    if(!decrypt) byte ^= mask;
    buffered_bits -= 8;
    if(buffered_bits == 0) mac(mac_word);
  }
}

void shannon::encrypt(std::span<std::byte> bytes) {
  process(bytes, false);
}

void shannon::decrypt(std::span<std::byte> bytes) {
  process(bytes, true);
}

std::array<std::byte, 4> shannon::finish() {
  /// Mark the end of plaintext and diffuse the accumulated authenticator
  if(finished) throw std::logic_error{"Shannon packet already finished"};
  if(buffered_bits != 0) mac(mac_word);
  cycle();
  registers[13] ^= initial_constant ^ (buffered_bits << 3);
  buffered_bits = 0;
  for(std::size_t index{0}; index < registers.size(); ++index) registers[index] ^= crc[index];
  for(unsigned int index{0}; index < 16; ++index) cycle();
  cycle();
  std::array<std::byte, 4> result;
  for(unsigned int index{0}; index < 4; ++index) result[index] = static_cast<std::byte>((stream_word >> (index * 8)) & 255u);
  finished = true;
  return result;
}

} // namespace librespot::crypto
