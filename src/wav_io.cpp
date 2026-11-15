#include "vc/wav_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vc {

namespace {

std::uint16_t readU16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t readU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeU16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

void writeU32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

bool readAll(const std::string& path, std::vector<unsigned char>* buf, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "无法打开文件: " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        if (err) *err = "文件为空: " + path;
        return false;
    }
    buf->resize(static_cast<std::size_t>(size));
    const std::size_t rd = std::fread(buf->data(), 1, buf->size(), f);
    std::fclose(f);
    if (rd != buf->size()) {
        if (err) *err = "读取不完整: " + path;
        return false;
    }
    return true;
}

inline float clamp1(float v) { return (v > 1.0f) ? 1.0f : ((v < -1.0f) ? -1.0f : v); }

}  // namespace

bool readWav(const std::string& path, WavData* out, std::string* err) {
    std::vector<unsigned char> file;
    if (!readAll(path, &file, err)) return false;
    if (file.size() < 44) {
        if (err) *err = "文件过小，不是有效 WAV";
        return false;
    }
    if (std::memcmp(file.data(), "RIFF", 4) != 0 ||
        std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        if (err) *err = "不是 RIFF/WAVE 文件";
        return false;
    }

    std::uint16_t fmtTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    bool haveFmt = false;

    const unsigned char* pcm = nullptr;
    std::size_t pcmBytes = 0;

    std::size_t off = 12;
    while (off + 8 <= file.size()) {
        const unsigned char* id = file.data() + off;
        const std::uint32_t sz = readU32(file.data() + off + 4);
        const std::size_t dataOff = off + 8;
        if (dataOff > file.size()) break;
        const std::size_t avail = std::min<std::size_t>(sz, file.size() - dataOff);

        if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
            fmtTag = readU16(file.data() + dataOff);
            channels = readU16(file.data() + dataOff + 2);
            sampleRate = readU32(file.data() + dataOff + 4);
            bitsPerSample = readU16(file.data() + dataOff + 14);
            // WAVE_FORMAT_EXTENSIBLE：真实格式在子格式 GUID 的前两字节
            if (fmtTag == 0xFFFE && avail >= 40) {
                fmtTag = readU16(file.data() + dataOff + 24);
            }
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            pcm = file.data() + dataOff;
            pcmBytes = avail;
            break;
        }
        off = dataOff + avail + (sz & 1u);
    }

    if (!haveFmt) {
        if (err) *err = "缺少 fmt 块";
        return false;
    }
    if (!pcm || pcmBytes == 0) {
        if (err) *err = "缺少 data 块";
        return false;
    }
    if (channels < 1 || channels > 2) {
        if (err) *err = "仅支持单声道或立体声，当前声道数: " + std::to_string(channels);
        return false;
    }

    const bool isFloat = (fmtTag == 3);
    const bool isPcm = (fmtTag == 1);
    if (!isFloat && !isPcm) {
        if (err) *err = "不支持的编码格式 fmtTag=" + std::to_string(fmtTag);
        return false;
    }

    const std::size_t bytesPerSample = static_cast<std::size_t>(bitsPerSample / 8);
    if (bytesPerSample == 0) {
        if (err) *err = "非法位深";
        return false;
    }
    const std::size_t frameBytes = bytesPerSample * channels;
    const std::size_t frames = pcmBytes / frameBytes;
    if (frames == 0) {
        if (err) *err = "data 块内没有完整采样帧";
        return false;
    }

    out->sampleRate = sampleRate;
    out->channels = channels;
    out->samples.resize(frames * channels);

    for (std::size_t i = 0; i < frames * channels; ++i) {
        const unsigned char* s = pcm + i * bytesPerSample;
        float v = 0.0f;
        if (isFloat) {
            if (bitsPerSample == 32) {
                std::uint32_t raw = readU32(s);
                float tmp;
                std::memcpy(&tmp, &raw, sizeof(tmp));
                v = tmp;
            } else {
                if (err) *err = "仅支持 32-bit float";
                return false;
            }
        } else {
            if (bitsPerSample == 16) {
                const std::int16_t tmp = static_cast<std::int16_t>(readU16(s));
                v = static_cast<float>(tmp) / 32768.0f;
            } else if (bitsPerSample == 24) {
                std::int32_t tmp = (static_cast<std::int32_t>(s[0]) << 8) |
                                   (static_cast<std::int32_t>(s[1]) << 16) |
                                   (static_cast<std::int32_t>(s[2]) << 24);
                tmp >>= 8;
                v = static_cast<float>(tmp) / 8388608.0f;
            } else if (bitsPerSample == 32) {
                const std::int32_t tmp = static_cast<std::int32_t>(readU32(s));
                v = static_cast<float>(tmp) / 2147483648.0f;
            } else if (bitsPerSample == 8) {
                v = (static_cast<float>(s[0]) - 128.0f) / 128.0f;
            } else {
                if (err) *err = "不支持的位深: " + std::to_string(bitsPerSample);
                return false;
            }
        }
        out->samples[i] = v;
    }

    return true;
}

bool writeWav(const std::string& path, const WavData& data, std::string* err) {
    if (data.channels < 1 || data.channels > 2) {
        if (err) *err = "声道数非法";
        return false;
    }
    if (data.frames() == 0) {
        if (err) *err = "没有可写出的采样";
        return false;
    }

    const std::uint16_t channels = data.channels;
    const std::uint32_t sampleRate = data.sampleRate;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t frameCount = static_cast<std::uint32_t>(data.frames());
    const std::uint32_t dataBytes = frameCount * channels * 2u;
    const std::uint32_t riffBytes = 36u + dataBytes;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = "无法创建文件: " + path;
        return false;
    }

    unsigned char hdr[44];
    std::memcpy(hdr, "RIFF", 4);
    writeU32(hdr + 4, riffBytes);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    writeU32(hdr + 16, 16);
    writeU16(hdr + 20, 1);
    writeU16(hdr + 22, channels);
    writeU32(hdr + 24, sampleRate);
    writeU32(hdr + 28, sampleRate * channels * 2u);
    writeU16(hdr + 32, static_cast<std::uint16_t>(channels * 2u));
    writeU16(hdr + 34, bitsPerSample);
    std::memcpy(hdr + 36, "data", 4);
    writeU32(hdr + 40, dataBytes);

    if (std::fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        std::fclose(f);
        if (err) *err = "写入 WAV 头失败";
        return false;
    }

    const std::size_t total = data.samples.size();
    std::vector<unsigned char> block;
    block.resize(4096 * 2);

    std::size_t i = 0;
    while (i < total) {
        const std::size_t n = std::min<std::size_t>(4096, total - i);
        for (std::size_t k = 0; k < n; ++k) {
            const float v = clamp1(data.samples[i + k]);
            // 用 32767 而非 32768 做标度，避免 +1.0 溢出成 -32768 造成爆音
            const float scaled = v * 32767.0f;
            const long rounded = std::lrintf(scaled);
            const long sat = (rounded > 32767) ? 32767 : ((rounded < -32768) ? -32768 : rounded);
            writeU16(&block[k * 2], static_cast<std::uint16_t>(static_cast<std::int16_t>(sat)));
        }
        if (std::fwrite(block.data(), 2, n, f) != n) {
            std::fclose(f);
            if (err) *err = "写入 PCM 数据失败";
            return false;
        }
        i += n;
    }

    std::fclose(f);
    return true;
}

}  // namespace vc
