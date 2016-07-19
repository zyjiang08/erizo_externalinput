/**
 * AudioCodec.pp
 */

#include "AudioCodec.h"
#include "../../rtp/RtpHeaders.h"

#include <cstdio>
#include <cstdlib>
#include <string.h>

#define OUTPUT_BIT_RATE 64000
#define OUTPUT_CHANNELS 1
#define OUTPUT_SAMPLE_FORMAT AV_SAMPLE_FMT_U8

#define PACKAGED_BUFFER_SIZE 2000*3

namespace erizo {

    DEFINE_LOGGER(AudioEncoder, "media.codecs.AudioEncoder");
    DEFINE_LOGGER(AudioDecoder, "media.codecs.AudioDecoder");

    inline  AVCodecID
        AudioCodecID2ffmpegDecoderID(AudioCodecID codec)
        {
            switch (codec)
            {
                case AUDIO_CODEC_PCM_U8: return AV_CODEC_ID_PCM_U8;
                case AUDIO_CODEC_VORBIS: return AV_CODEC_ID_VORBIS;
                default: return AV_CODEC_ID_PCM_U8;
            }
        }

    extern AVFormatContext* input_format_context;
    AVCodecContext *input_codec_context = NULL, *output_codec_context = NULL;
    SwrContext *resample_context = NULL;
    AVAudioFifo *fifo = NULL;

    const char *get_error_text(const int error)
    {
        static char error_buffer[255];
        av_strerror(error, error_buffer, sizeof(error_buffer));
        return error_buffer;
    }

    void init_packet(AVPacket *packet)
    {
        av_init_packet(packet);
        packet->data = NULL;
        packet->size = 0;
    }

    int init_input_frame(AVFrame **frame)
    {
        if (!(*frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
        return 0;
    }


    /**
     * Initialize the audio resampler based on the input and output codec settings.
     * If the input and output sample formats differ, a conversion is required
     * libswresample takes care of this, but requires initialization.
     */
    int AudioDecoder::init_resampler(AVCodecContext *input_codec_context,
            AVCodecContext *output_codec_context)
    {
        int error;

        /**
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity (they are sometimes not detected
         * properly by the demuxer and/or decoder).
         */
        resample_context = swr_alloc_set_opts(NULL,
                av_get_default_channel_layout(output_codec_context->channels),
                output_codec_context->sample_fmt,
                output_codec_context->sample_rate,
                av_get_default_channel_layout(input_codec_context->channels),
                input_codec_context->sample_fmt,
                input_codec_context->sample_rate,
                0, NULL);

        if (!resample_context) {
            ELOG_WARN( "Could not allocate resample context\n");
            return AVERROR(ENOMEM);
        }

        ELOG_DEBUG( "resample_context = swr_alloc_set_opts done");

        /**
         * Perform a sanity check so that the number of converted samples is
         * not greater than the number of samples to be converted.
         * If the sample rates differ, this case has to be handled differently
         */
        av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

        ELOG_DEBUG( "av_assert0 done");

        /** Open the resampler with the specified parameters. */
        if ((error = swr_init(resample_context)) < 0) {
            ELOG_WARN( "Could not open resample context");
            swr_free(&resample_context);
            return error;
        }
        ELOG_DEBUG( "av_assert0 done");


        /** Open the resampler with the specified parameters. */
        if ((error = swr_init(resample_context)) < 0) {
            ELOG_DEBUG( "Could not open resample context");
            swr_free(&resample_context);
            return error;
        }

        ELOG_DEBUG( "swr_init done");

        return 0;
    }

    int AudioDecoder::add_samples_to_fifo(AVAudioFifo *fifo,
            uint8_t **converted_input_samples,
            const int frame_size)
    {
        int error;
        /**
         * Make the FIFO as large as it needs to be to hold both,
         * the old and the new samples.
         */
        if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
            ELOG_WARN("Could not reallocate FIFO");
            return error;
        }
        /** Store the new samples in the FIFO buffer. */
        if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                    frame_size) < frame_size) {
            ELOG_DEBUG("Could not write data to FIFO");
            return AVERROR_EXIT;
        }
        return 0;
    }
    /** Initialize a FIFO buffer for the audio samples to be encoded. */
    int AudioDecoder::init_fifo(AVAudioFifo **fifo)
    {
        /** Create the FIFO buffer based on the specified output sample format. */
        if (!(*fifo = av_audio_fifo_alloc(OUTPUT_SAMPLE_FORMAT, OUTPUT_CHANNELS, 1)))
        {
            ELOG_DEBUG("Could not allocate FIFO");
            return AVERROR(ENOMEM);
        }
        return 0;
    }

    /**
      Initialize one input frame for writing to the output file.
     * The frame will be exactly frame_size samples large.
     */
    int AudioDecoder::init_output_frame(AVFrame **frame,
            AVCodecContext *output_codec_context,
            int frame_size)
    {
        int error;

        /** Create a new frame to store the audio samples. */
        if (!(*frame = av_frame_alloc())) {
            ELOG_DEBUG( "Could not allocate output frame");
            return AVERROR_EXIT;
        }

        /**
         * Set the frame's parameters, especially its size and format.
         * av_frame_get_buffer needs this to allocate memory for the
         * audio samples of the frame.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity.
         */
        (*frame)->nb_samples     = frame_size;
        (*frame)->channel_layout = output_codec_context->channel_layout;
        (*frame)->format         = output_codec_context->sample_fmt;
        (*frame)->sample_rate    = output_codec_context->sample_rate;

        /**
         * Allocate the samples of the created frame. This call will make
         * sure that the audio frame can hold as many samples as specified.
         */
        if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
            ELOG_DEBUG( "Could allocate output frame samples (error '%s')\n", get_error_text(error));
            av_frame_free(frame);
            return error;
        }

        return 0;
    }

    int AudioDecoder::encode_package_audio_frame(AVFrame *frame, unsigned char* outbuf)
    {
        /** Packet used for temporary storage. */
        AVPacket output_packet;
        int error;
        init_packet(&output_packet);
        output_packet.data = NULL;
        output_packet.size = 0;
        /**
         * Encode the audio frame and store it in the temporary packet.
         * The output audio stream encoder is used to do this.
         */
        int data_present = 0;
        if ((error = avcodec_encode_audio2(output_codec_context, &output_packet,
                        frame, &data_present)) < 0) {
            ELOG_WARN("Could not encode frame,%s", get_error_text(error));
            av_free_packet(&output_packet);
            return error;
        }

        // Package it, and send it.
        int ret = packageAudio(output_packet.data, output_packet.size, outbuf);

        //free it.
        av_free_packet(&output_packet);

        return ret;
    }

    /**
     * Load one audio frame from the FIFO buffer, encode and write it to the
     * output file.
     */
    int AudioDecoder::load_encode_and_write(unsigned char* outbuf)
    {
        /** Temporary storage of the output samples of the frame written to the file. */
        AVFrame *output_frame;
        /**
         * Use the maximum number of possible samples per frame.
         * If there is less than the maximum possible frame size in the FIFO
         * buffer use this number. Otherwise, use the maximum possible frame size
         */
        const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                output_codec_context->frame_size);
        /** Initialize temporary storage for one output frame. */
        if (init_output_frame(&output_frame, output_codec_context, frame_size))
        {
            ELOG_WARN(" init_output_frame failed!!");
            return 0;
        }
        /**
         * Read as many samples from the FIFO buffer as required to fill the frame.
         * The samples are stored in the frame temporarily.
         */
        if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
            ELOG_WARN("Could not read data from FIFO\n");
            av_frame_free(&output_frame);
            return 0;
        }
        /** Encode one frame worth of audio samples. */
        int pktlen = encode_package_audio_frame(output_frame, outbuf);
        if (pktlen <= 0)
        {
            ELOG_WARN("Failed to encode_audio_frame!!");
        }
        av_frame_free(&output_frame);

        return pktlen;
    }


    /**
     * Initialize a temporary storage for the specified number of audio samples.
     * The conversion requires temporary storage due to the different format.
     * The number of audio samples to be allocated is specified in frame_size.
     */
    int AudioDecoder::init_converted_samples(uint8_t ***converted_input_samples,
            AVCodecContext *output_codec_context,
            int frame_size)
    {
        int error;

        /**
         * Allocate as many pointers as there are audio channels.
         * Each pointer will later point to the audio samples of the corresponding
         * channels (although it may be NULL for interleaved formats).
         */
        if (!(*converted_input_samples = (uint8_t**)calloc(output_codec_context->channels,
                        sizeof(**converted_input_samples)))) {
            ELOG_WARN( "Could not allocate converted input sample pointers");
            return AVERROR(ENOMEM);
        }

        /**
         * Allocate memory for the samples of all channels in one consecutive
         * block for convenience.
         */
        if ((error = av_samples_alloc(*converted_input_samples, NULL,
                        output_codec_context->channels,
                        frame_size,
                        output_codec_context->sample_fmt, 0)) < 0) {
            ELOG_WARN("Could not allocate converted input samples ", get_error_text(error));
            av_freep(&(*converted_input_samples)[0]);
            free(*converted_input_samples);
            return error;
        }
        return 0;
    }

    /**
     * Convert the input audio samples into the output sample format.
     * The conversion happens on a per-frame basis, the size of which is specified
     * by frame_size.
     */
    int AudioDecoder::convert_samples(const uint8_t **input_data,
            uint8_t **converted_data, const int frame_size,
            SwrContext *resample_context)
    {
        int error;

        /** Convert the samples using the resampler. */
        if ((error = swr_convert(resample_context,
                        converted_data, frame_size,
                        input_data    , frame_size)) < 0) {
            ELOG_DEBUG( "Could not convert input samples ", get_error_text(error));
            return error;
        }

        return 0;
    }

    ////////////////////////////////////////////////
    AudioEncoder::AudioEncoder(){
        codec_ = NULL;
        input_codec_context=NULL;
        aFrame_ = NULL;
    }

    AudioEncoder::~AudioEncoder(){
        ELOG_DEBUG("AudioEncoder Destructor");
        this->closeEncoder();
    }

    int AudioEncoder::initEncoder (const AudioCodecInfo& mediaInfo){

        ELOG_DEBUG("Init audioEncoder begin");
        codec_ = avcodec_find_encoder(mediaInfo.codec);
        if (!codec_) {
            ELOG_DEBUG("Audio Codec not found");
            return false;
        }

        input_codec_context = avcodec_alloc_context3(codec_);
        if (!input_codec_context) {
            ELOG_DEBUG("Memory error allocating audio coder context");
            return false;
        }

        input_codec_context->sample_fmt = OUTPUT_SAMPLE_FORMAT;
        input_codec_context->bit_rate = mediaInfo.bitRate;
        input_codec_context->sample_rate = mediaInfo.sampleRate;
        input_codec_context->channels = OUTPUT_CHANNELS;
        char errbuff[500];
        int res = avcodec_open2(input_codec_context, codec_, NULL);
        if(res != 0){
            av_strerror(res, (char*)(&errbuff), 500);
            ELOG_DEBUG("fail when opening input %s", errbuff);
            return -1;
        }
        ELOG_DEBUG("Init audioEncoder end");
        return true;
    }

    int AudioEncoder::encodeAudio (unsigned char* inBuffer, int nSamples, AVPacket* pkt) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            ELOG_ERROR("could not allocate audio frame");
            return 0;
        }
        int ret, got_output, buffer_size;

        frame->nb_samples = input_codec_context->frame_size;
        frame->format = input_codec_context->sample_fmt;
        //	frame->channel_layout = input_codec_context->channel_layout;

        /* the codec gives us the frame size, in samples,
         * we calculate the size of the samples buffer in bytes */
        ELOG_DEBUG("channels %d, frame_size %d, sample_fmt %d",
                input_codec_context->channels, input_codec_context->frame_size,
                input_codec_context->sample_fmt);
        buffer_size = av_samples_get_buffer_size(NULL, input_codec_context->channels,
                input_codec_context->frame_size, input_codec_context->sample_fmt, 0);
        uint16_t* samples = (uint16_t*) malloc(buffer_size);
        if (!samples) {
            ELOG_ERROR("could not allocate %d bytes for samples buffer",buffer_size);
            return 0;
        }
        /* setup the data pointers in the AVFrame */
        ret = avcodec_fill_audio_frame(frame, input_codec_context->channels,
                input_codec_context->sample_fmt, (const uint8_t*) samples, buffer_size,
                0);
        if (ret < 0) {
            free(samples);
            ELOG_ERROR("could not setup audio frame");
            return 0;
        }

        ret = avcodec_encode_audio2(input_codec_context, pkt, frame, &got_output);
        if (ret < 0) {
            ELOG_ERROR("error encoding audio frame");
            free(samples);
            return 0;
        }
        if (got_output) {
            //fwrite(pkt.data, 1, pkt.size, f);
            ELOG_DEBUG("Got OUTPUT");
        }

        return ret;
    }

    int AudioEncoder::closeEncoder (){
        if (input_codec_context!=NULL){
            avcodec_close(input_codec_context);
        }
        if (aFrame_!=NULL){
            av_frame_free(&aFrame_);
        }
        return 0;
    }


    AudioDecoder::AudioDecoder(){
        codec_ = NULL;
        output_codec_context = NULL;
        dFrame_ = NULL;
    }

    AudioDecoder::~AudioDecoder(){
        ELOG_DEBUG("AudioDecoder Destructor");
        this->closeDecoder();
    }

    int AudioDecoder::initDecoder(AVCodecContext* context, AVCodec* dec_codec){
        if (dec_codec != NULL)
        {
            codec_ = dec_codec;
            ELOG_DEBUG("codec_ assigned");
        }

        //input_codec_context = avcodec_alloc_context3(codec_);
        input_codec_context = context;  // ok?
        if (!input_codec_context) {
            ELOG_DEBUG("AudioDecoder Error allocating audio decoder context");
            return 0;
        }

        if (avcodec_open2(input_codec_context, codec_, NULL) < 0) {
            ELOG_DEBUG("AudioDecoder initDecoder Error open2 audio decoder");
            return 0;
        }

        // Init output encoder as well.
        AVCodecContext *avctx          = NULL;
        AVCodec *output_codec          = NULL;
        int error;

        if (!(output_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_U8))) {
            ELOG_DEBUG( "Could not find an AAC encoder.");

            return 0;
        }
        avctx = avcodec_alloc_context3(output_codec);
        if (!avctx) {
            ELOG_DEBUG( "Could not allocate an encoding context");
            return 0;
        }

        memcpy(outBuff, decBuff, outSize);
        outBuff += outSize;
        decSize += outSize;
        /**
         * Set the basic encoder parameters.
         * The input file's sample rate is used to avoid a sample rate conversion.
         */
        avctx->channels       = OUTPUT_CHANNELS;
        avctx->channel_layout = av_get_default_channel_layout(OUTPUT_CHANNELS);
        avctx->sample_rate    = context->sample_rate;
        avctx->sample_fmt     = output_codec->sample_fmts[0];
        avctx->bit_rate       = OUTPUT_BIT_RATE;
        /** Allow the use of the experimental AAC encoder */
        avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        ELOG_DEBUG(" avcodec_alloc_context3(output_codec) done");

        /** Open the encoder for the audio stream to use it later. */
        if ((error = avcodec_open2(avctx, output_codec, NULL)) < 0) {
            ELOG_DEBUG( "Could not open output codec ", get_error_text(error));

            return 0;
        }

        free(decBuff);
        ELOG_DEBUG(" avcodec_open2(avctx, output_codec done");


        /** Save the encoder context for easier access later. */
        output_codec_context = avctx;
        /** Initialize the resampler to be able to convert audio sample formats. */
        if (init_resampler(input_codec_context, output_codec_context))
        {
            ELOG_DEBUG(" init resampleer failed !!");

            return 0;
        }
        ELOG_DEBUG(" init_resampler done");

        init_fifo(&fifo);

        ELOG_DEBUG(" init_fifo done");

        return 1;
    }
    int AudioDecoder::decodeAudio(AVPacket& input_packet, unsigned char* outbuf)    {
        AVFrame* input_frame;
        init_input_frame(&input_frame);

        ELOG_DEBUG("input packet size "<<input_packet.size);
        int* data_present = 0;
        int error = avcodec_decode_audio4(input_codec_context, input_frame, data_present,&input_packet);

        if (error < 0)
        {
            ELOG_DEBUG( get_error_text(error));

            return error;
        }

        if (*data_present <= 0)
        {
            ELOG_DEBUG(" data not present ");
            return 0;
        }

        // resample

        /** Initialize the temporary storage for the converted input samples. */
        uint8_t **converted_input_samples = NULL;
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                    input_frame->nb_samples))
        {
            ELOG_DEBUG(" init_converted_samples fails");

            return 0;
        }

        /**
         * Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples
         */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,input_frame->nb_samples, resample_context))
        {
            ELOG_WARN(" convert_samples failed!!");

            return 0;
        }

        /** Add converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                    input_frame->nb_samples))
        {
            ELOG_WARN(" add_samples to fifo failed !!");
        }

        // meanwhile, encode; package
        return load_encode_and_write(outbuf);
    }

    int AudioDecoder::decodeAudio(unsigned char* inBuff, int inBuffLen,
            unsigned char* outBuff, int outBuffLen, int* gotFrame){

        AVPacket avpkt;
        int outSize;
        int decSize = 0;
        int len = -1;
        uint8_t *decBuff = (uint8_t*) malloc(16000);

        av_init_packet(&avpkt);
        avpkt.data = (unsigned char*) inBuff;
        avpkt.size = inBuffLen;

        while (avpkt.size > 0) {

            outSize = 16000;

            //Puede fallar. Cogido de libavcodec/utils.c del paso de avcodec_decode_audio3 a avcodec_decode_audio4
            //avcodec_decode_audio3(aDecoderContext, (short*)decBuff, &outSize, &avpkt);

            AVFrame frame;
            int got_frame = 0;

            //      aDecoderContext->get_buffer = avcodec_default_get_buffer;
            //      aDecoderContext->release_buffer = avcodec_default_release_buffer;

            len = avcodec_decode_audio4(output_codec_context, &frame, &got_frame, &avpkt);
            if (len >= 0 && got_frame) {
                int plane_size;
                //int planar = av_sample_fmt_is_planar(aDecoderContext->sample_fmt);
                int data_size = av_samples_get_buffer_size(&plane_size,
                        output_codec_context->channels, frame.nb_samples,
                        output_codec_context->sample_fmt, 1);
                if (outSize < data_size) {
                    ELOG_DEBUG("output buffer size is too small for the current frame");
                    free(decBuff);
                    return AVERROR(EINVAL);
                }

                memcpy(decBuff, frame.extended_data[0], plane_size);

                /* Si hay más de un canal
                   if (planar && aDecoderContext->channels > 1) {
                   uint8_t *out = ((uint8_t *)decBuff) + plane_size;
                   for (int ch = 1; ch < aDecoderContext->channels; ch++) {
                   memcpy(out, frame.extended_data[ch], plane_size);
                   out += plane_size;
                   }
                   }
                   */
                outSize = data_size;
            } else {
                outSize = 0;
            }

            if (len < 0) {
                ELOG_DEBUG("Error al decodificar audio");
                free(decBuff);
                return -1;
            }

            avpkt.size -= len;
            avpkt.data += len;

            if (outSize <= 0) {
                continue;
            }

            memcpy(outBuff, decBuff, outSize);
            outBuff += outSize;
            decSize += outSize;
        }

        free(decBuff);

        if (outSize <= 0) {
            ELOG_DEBUG("Error de decodificación de audio debido a tamaño incorrecto");
            return -1;
        }

        return decSize;
    }
    int AudioDecoder::closeDecoder(){
        if (output_codec_context!=NULL){
            avcodec_close(output_codec_context);
        }
        if (dFrame_!=NULL){
            av_frame_free(&dFrame_);
        }
        return 0;
    }
}
