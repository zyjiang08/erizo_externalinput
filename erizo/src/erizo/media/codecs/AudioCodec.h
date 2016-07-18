/**
 * VideoCodec.h
 */

#ifndef AUDIOCODEC_H_
#define AUDIOCODEC_H_

#include "Codecs.h"
#include "logger.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

}

namespace erizo {

  class AudioEncoder {
    DECLARE_LOGGER();
    public:
      AudioEncoder();
      virtual ~AudioEncoder();
      int initEncoder (const AudioCodecInfo& info);
      int encodeAudio (unsigned char* inBuffer, int nSamples, AVPacket* pkt);
      int closeEncoder ();

    private:
      AVCodec* codec_;
  };

  class AudioDecoder {
    DECLARE_LOGGER();
    public:
      AudioDecoder();
      virtual ~AudioDecoder();
      int initDecoder (const AudioCodecInfo& info);
      int initDecoder (AVCodecContext* context, AVCodec* dec_codec);
      int int decodeAudio(AVPacket& input_packet, unsigned char* outbuf);
      int closeDecoder();

    private:
      AVCodec* codec_;
  };
}
#endif /* AUDIOCODEC_H_ */
