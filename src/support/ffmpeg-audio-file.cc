/*

MIT License

Copyright (c) 2022 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#include "support/ffmpeg-audio-file.h"

#include <stdint.h>

#include <bit>

AVSampleFormat PCSX::FFmpegAudioFile::getSampleFormat() const {
    switch (m_sampleFormat) {
        case SampleFormat::U8:
            return AV_SAMPLE_FMT_U8;
        case SampleFormat::S16:
            return AV_SAMPLE_FMT_S16;
        case SampleFormat::S32:
            return AV_SAMPLE_FMT_S32;
        case SampleFormat::F32:
            return AV_SAMPLE_FMT_FLT;
        case SampleFormat::D64:
            return AV_SAMPLE_FMT_DBL;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

unsigned PCSX::FFmpegAudioFile::getSampleSize() const {
    switch (m_sampleFormat) {
        case SampleFormat::U8:
            return 1;
        case SampleFormat::S16:
            return 2;
        case SampleFormat::S32:
            return 4;
        case SampleFormat::F32:
            return 4;
        case SampleFormat::D64:
            return 8;
        default:
            return 0;
    }
}

PCSX::FFmpegAudioFile::FFmpegAudioFile(IO<File> file, Channels channels, Endianness endianess,
                                       SampleFormat sampleFormat, unsigned frequency)
    : File(RO_SEEKABLE), m_file(file), m_channels(channels), m_endianess(endianess), m_sampleFormat(sampleFormat) {
    av_log_set_level(AV_LOG_QUIET);

    unsigned char *buffer = reinterpret_cast<unsigned char *>(av_malloc(4096));
    int ret;

    m_ioContext = avio_alloc_context(
        buffer, 4096, 0, this,
        [](void *opaque, uint8_t *buf, int bufSize) -> int {
            FFmpegAudioFile *f = reinterpret_cast<FFmpegAudioFile *>(opaque);
            if (f->m_file->failed()) return AVERROR_STREAM_NOT_FOUND;
            auto ret = f->m_file->read(buf, bufSize);
            if (ret <= 0) {
                if (f->m_file->eof()) return AVERROR_EOF;
                return AVERROR_UNKNOWN;
            }
            return ret;
        },
        nullptr,
        [](void *opaque, int64_t offset, int whence) -> int64_t {
            FFmpegAudioFile *f = reinterpret_cast<FFmpegAudioFile *>(opaque);
            if (f->m_file->failed()) return AVERROR_STREAM_NOT_FOUND;
            if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
                return f->m_file->size();
            }
            auto ret = f->m_file->rSeek(offset, whence & 0xffff);
            if (ret < 0) return AVERROR_UNKNOWN;
            return ret;
        });

    if (!m_ioContext) {
        av_freep(&buffer);
        m_failed = true;
        return;
    }

    m_formatContext = avformat_alloc_context();
    if (!m_formatContext) {
        m_failed = true;
        return;
    }

    m_formatContext->pb = m_ioContext;
    if (avformat_open_input(&m_formatContext, nullptr, nullptr, nullptr) < 0) {
        m_failed = true;
        return;
    }

    if (avformat_find_stream_info(m_formatContext, nullptr) < 0) {
        m_failed = true;
        return;
    }
    double duration = ((double)m_formatContext->duration) / ((double)AV_TIME_BASE);
    unsigned sampleSize = getSampleSize();
    if (channels == Channels::Stereo) sampleSize *= 2;
    m_size = ceil(duration * frequency * sampleSize);

    m_packet = av_packet_alloc();
    m_decodedFrame = av_frame_alloc();
    m_resampledFrame = av_frame_alloc();
    m_resampledFrame->sample_rate = frequency;
    m_resampledFrame->format = getSampleFormat();
    m_resampledFrame->ch_layout.nb_channels = channels == Channels::Stereo ? 2 : 1;
    m_resampledFrame->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    m_resampledFrame->ch_layout.u.mask = channels == Channels::Stereo ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    m_resampledFrame->nb_samples = 0;
    const AVCodec *codec;
    ret = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);

    if (!m_packet || !m_decodedFrame || !m_resampledFrame || (ret < 0)) {
        m_failed = true;
        return;
    }
    m_audioStreamIndex = ret;

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        m_failed = true;
        return;
    }

    avcodec_parameters_to_context(m_codecContext, m_formatContext->streams[m_audioStreamIndex]->codecpar);

    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        m_failed = true;
        return;
    }

    AVChannelLayout layout;
    if (channels == Channels::Stereo) {
        layout = AV_CHANNEL_LAYOUT_STEREO;
    } else {
        layout = AV_CHANNEL_LAYOUT_MONO;
    }
    if (swr_alloc_set_opts2(&m_resamplerContext, &layout, getSampleFormat(), frequency, &m_codecContext->ch_layout,
                            m_codecContext->sample_fmt, m_codecContext->sample_rate, 0, nullptr) < 0) {
        m_failed = true;
        return;
    }

    swr_init(m_resamplerContext);
    if (!swr_is_initialized(m_resamplerContext)) {
        m_failed = true;
        return;
    }

    m_failed = av_seek_frame(m_formatContext, m_audioStreamIndex, 0, AVSEEK_FLAG_BYTE) < 0;
}

void PCSX::FFmpegAudioFile::closeInternal() {
    if (m_ioContext) {
        av_freep(&m_ioContext->buffer);
        avio_context_free(&m_ioContext);
    }
    if (m_formatContext) avformat_free_context(m_formatContext);
    if (m_packet) av_packet_free(&m_packet);
    if (m_decodedFrame) av_frame_free(&m_decodedFrame);
    if (m_resampledFrame) av_frame_free(&m_resampledFrame);
    if (m_codecContext) avcodec_free_context(&m_codecContext);
    if (m_resamplerContext) swr_free(&m_resamplerContext);
}

ssize_t PCSX::FFmpegAudioFile::rSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_filePtr = pos;
            break;
        case SEEK_END:
            m_filePtr = m_size + pos;
            break;
        case SEEK_CUR:
            m_filePtr += pos;
            break;
    }
    return m_filePtr;
}

ssize_t PCSX::FFmpegAudioFile::read(void *dest_, size_t size) {
    uint8_t *dest = reinterpret_cast<uint8_t *>(dest_);

    ssize_t dumpDelta = m_filePtr - m_totalOut;
    if (dumpDelta < 0) {
        dumpDelta = m_filePtr;
        m_filePtr = 0;
        m_hitEOF = false;
        if (av_seek_frame(m_formatContext, m_audioStreamIndex, 0, AVSEEK_FLAG_BYTE) < 0) return -1;
        m_totalOut = 0;
    }
    if (m_hitEOF) return -1;
    ssize_t ret = 0;
    while (dumpDelta) {
        uint8_t dummy[256];
        ssize_t toDump = std::min(ssize_t(sizeof(dummy)), dumpDelta);
        ssize_t p = decompSome(dummy, toDump);
        if (p < 0) return p;
        dumpDelta -= p;
        if (m_hitEOF || !p) break;
    }
    while (size) {
        if (m_hitEOF) break;
        ssize_t p = decompSome(dest, size);
        if (p < 0) return p;
        if (!p) break;
        size -= p;
        ret += p;
        dest += p;
        m_filePtr += p;
    }

    return ret;
}

ssize_t PCSX::FFmpegAudioFile::decompSome(void *dest_, ssize_t size) {
    unsigned sampleSize = getSampleSize();
    if (m_channels == Channels::Stereo) sampleSize *= 2;
    ssize_t dataRead = 0;
    uint8_t *dest = reinterpret_cast<uint8_t *>(dest_);
    ssize_t available = m_resampledFrame->nb_samples * sampleSize - m_packetPtr;
    AVFrame *inFrame = nullptr;

    while (true) {
        ssize_t toCopy = std::min(available, size);
        if (toCopy) {
            if (((std::endian::native == std::endian::little) && (m_endianess == Endianness::Little)) ||
                ((std::endian::native == std::endian::big) && (m_endianess == Endianness::Big))) {
                memcpy(dest, m_resampledFrame->data[0] + m_packetPtr, toCopy);
            } else {
                for (ssize_t i = 0; i < toCopy; i += sampleSize) {
                    for (unsigned j = 0; j < sampleSize; ++j) {
                        dest[i + j] = m_resampledFrame->data[0][m_packetPtr + i + sampleSize - j - 1];
                    }
                }
            }
            m_packetPtr += toCopy;
            m_totalOut += toCopy;
            size -= toCopy;
            dest += toCopy;
            dataRead += toCopy;
            available -= toCopy;
        }
        if (size == 0) return dataRead;

        m_packetPtr = 0;

        if (swr_convert_frame(m_resamplerContext, m_resampledFrame, inFrame) < 0) return -1;
        available = m_resampledFrame->nb_samples * sampleSize;
        inFrame = nullptr;
        if (available == 0) {
            while (true) {
                if (av_read_frame(m_formatContext, m_packet) < 0) {
                    m_hitEOF = true;
                    return dataRead;
                }
                if (m_packet->stream_index != m_audioStreamIndex) {
                    av_packet_unref(m_packet);
                    continue;
                }
                break;
            }
            if (avcodec_send_packet(m_codecContext, m_packet) < 0) return -1;
            int ret = avcodec_receive_frame(m_codecContext, m_decodedFrame);
            av_packet_unref(m_packet);
            int eagain = AVERROR(EAGAIN);
            int eof = AVERROR_EOF;
            if ((ret == eagain) || (ret == eof)) {
                return dataRead;
            } else if (ret < 0) {
                return -1;
            }
            inFrame = m_decodedFrame;
        }
    }

    return -1;
}
