#include "ExternalInput.h"
#include "../WebRtcConnection.h"
#include <cstdio>

#include "libavutil/opt.h"
#include <boost/cstdint.hpp>
#include <sys/time.h>
#include <arpa/inet.h>
#include "libavutil/time.h"
#include "codecs/AudioCodec.h"

namespace erizo {

    AudioDecoder audioDecoder;
    extern const char* get_error_text(const int error);
    DEFINE_LOGGER(ExternalInput, "media.ExternalInput");
    ExternalInput::ExternalInput(const std::string& inputUrl):url_(inputUrl){
        sourcefbSink_=NULL;
        context_ = NULL;
        running_ = false;
        needTranscoding_ = false;
        lastPts_ = 0;
        lastAudioPts_=0;
    }

    extern MediaSink* audioSink;

    ExternalInput::~ExternalInput(){
        ELOG_DEBUG("Destructor ExternalInput %s" , url_.c_str());
        running_ = false;
        thread_.join();
        if (needTranscoding_)
            encodeThread_.join();
        av_free_packet(&avpacket_);
        if (context_!=NULL)
            avformat_free_context(context_);
        ELOG_DEBUG("ExternalInput closed");
    }

    int ExternalInput::init(){

        // for OutputProcessor->packageAudio()
        audioSink = audioSink_;

        context_ = avformat_alloc_context();
        av_register_all();
        avcodec_register_all();
        avformat_network_init();
        //open rtsp
        av_init_packet(&avpacket_);
        avpacket_.data = NULL;
        ELOG_INFO("Trying to open input from url %s", url_.c_str());
        int res = avformat_open_input(&context_, url_.c_str(),NULL,NULL);
        if(res != 0){
            ELOG_ERROR("Error opening input %s", get_error_text(res));
            return res;
        }
        res = avformat_find_stream_info(context_,NULL);
        if(res<0){
            ELOG_ERROR("Error finding stream info %s", get_error_text(res));
            return res;
        }

        MediaInfo om;
        AVStream *st, *audio_st;
        AVCodec* audioCodec = NULL;

        int streamNo = av_find_best_stream(context_, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (streamNo < 0){
            ELOG_WARN("No Video stream found");
        }else{
            om.hasVideo = true;
            video_stream_index_ = streamNo;
            st = context_->streams[streamNo]; 

            av_dump_format(context_, streamNo, url_.c_str(), 0);
        }

        int audioStreamNo = av_find_best_stream(context_, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
        if (audioStreamNo < 0){
            ELOG_WARN("No Audio stream found");
        }else{
            av_dump_format(context_, audioStreamNo, url_.c_str(), 0);

            om.hasAudio = true;
            audio_stream_index_ = audioStreamNo;
            audio_st = context_->streams[audio_stream_index_];
            ELOG_DEBUG("Audio, audio stream number %d. time base = %d / %d ", audio_stream_index_, audio_st->time_base.num, audio_st->time_base.den);
            audio_time_base_ = audio_st->time_base.den;

            audioDecoder.initDecoder(audio_st->codec, audioCodec);
            if (audio_st->codec->codec_id==AV_CODEC_ID_PCM_MULAW){
                ELOG_DEBUG("PCM U8");
                om.audioCodec.sampleRate=8000;
                om.audioCodec.codec = AUDIO_CODEC_PCM_U8;
                om.rtpAudioInfo.PT = PCMU_8000_PT; 
            }else if (audio_st->codec->codec_id == AV_CODEC_ID_OPUS){
                ELOG_DEBUG("OPUS");
                om.audioCodec.sampleRate=48000;
                om.audioCodec.codec = AUDIO_CODEC_OPUS;
                om.rtpAudioInfo.PT = OPUS_48000_PT; 
            }
            if (!om.hasVideo)
                st = audio_st;
        }


        if (st->codec->codec_id==AV_CODEC_ID_VP8 || !om.hasVideo){
            ELOG_DEBUG("No need for video transcoding, already VP8");      
            video_time_base_ = st->time_base.den;
            needTranscoding_=false;
            decodedBuffer_.reset((unsigned char*) malloc(100000));
            MediaInfo om;
            om.processorType = PACKAGE_ONLY;
            if (audio_st->codec->codec_id==AV_CODEC_ID_PCM_MULAW){
                ELOG_DEBUG("PCM U8");
                om.audioCodec.sampleRate=8000;
                om.audioCodec.codec = AUDIO_CODEC_PCM_U8;
                om.rtpAudioInfo.PT = PCMU_8000_PT; 
            }else if (audio_st->codec->codec_id == AV_CODEC_ID_OPUS){
                ELOG_DEBUG("OPUS");
                om.audioCodec.sampleRate=48000;
                om.audioCodec.codec = AUDIO_CODEC_OPUS;
                om.rtpAudioInfo.PT = OPUS_48000_PT; 
            }
            op_.reset(new OutputProcessor());
            op_->init(om,this);
        }else{
            needTranscoding_=true;
            inCodec_.initDecoder(st->codec);

            bufflen_ = st->codec->width*st->codec->height*3/2;
            decodedBuffer_.reset((unsigned char*) malloc(bufflen_));

            om.processorType = RTP_ONLY;
            om.videoCodec.codec = VIDEO_CODEC_VP8;
            om.videoCodec.bitRate = 1000000;
            om.videoCodec.width = 854;
            om.videoCodec.height = 480;
            om.videoCodec.frameRate = 20;
            om.hasVideo = true;

            om.hasAudio = true;
            if (om.hasAudio) {
                om.audioCodec.sampleRate = 8000;
                om.audioCodec.bitRate = 64000;
            }

            op_.reset(new OutputProcessor());
            op_->init(om, this);
        }

        av_init_packet(&avpacket_);

        thread_ = boost::thread(&ExternalInput::receiveLoop, this);
        running_ = true;
        if (needTranscoding_)
            encodeThread_ = boost::thread(&ExternalInput::encodeLoop, this);

        return true;
    }

    int ExternalInput::sendPLI() {
        return 0;
    }

    void ExternalInput::receiveRtpData(unsigned char*rtpdata, int len) {
        if (audioSink_!=NULL){
            memcpy(sendVideoBuffer_, rtpdata, len);
            audioSink_->deliverVideoData(sendVideoBuffer_, len);
        }
    }

    void ExternalInput::receiveLoop(){

        av_read_play(context_);//play RTSP
        int gotDecodedFrame = 0;
        startTime_ = av_gettime();

        ELOG_INFO("Start playing external input %s", url_.c_str() );
        while(av_read_frame(context_,&avpacket_)>=0&& running_==true){
            AVPacket orig_pkt = avpacket_;
            if (needTranscoding_){
                if(avpacket_.stream_index == video_stream_index_){

                    // Speed control.
                    int64_t pts = av_rescale(lastPts_, 1000000, (long int)video_time_base_);
                    int64_t now = av_gettime() - startTime_;         
                    if (pts > now){
                        av_usleep(pts - now);
                    }
                    lastPts_ = avpacket_.pts;

                    inCodec_.decodeVideo(avpacket_.data, avpacket_.size, decodedBuffer_.get(), bufflen_, &gotDecodedFrame);
                    RawDataPacket packetR;
                    if (gotDecodedFrame){
                        packetR.data = decodedBuffer_.get();
                        packetR.length = bufflen_;
                        packetR.type = VIDEO;
                        queueMutex_.lock();
                        packetQueue_.push(packetR);
                        queueMutex_.unlock();
                        gotDecodedFrame=0;
                    }
                }
            }else{
                if(avpacket_.stream_index == video_stream_index_){//packet is video               
                    int64_t pts = av_rescale(lastPts_, 1000000, (long int)video_time_base_);
                    int64_t now = av_gettime() - startTime_;         
                    if (pts > now){
                        av_usleep(pts - now);
                    }
                    lastPts_ = avpacket_.pts;

                    ELOG_DEBUG("Video and package %d, dts=%ld,duration=%d,pos=%ld, pts=%ld", avpacket_.size, avpacket_.dts, avpacket_.duration, avpacket_.pos, avpacket_.pts);
                    op_->packageVideo(avpacket_.data, avpacket_.size, decodedBuffer_.get(), avpacket_.pts);
                }else if(avpacket_.stream_index == audio_stream_index_){//packet is audio
                    int64_t pts = av_rescale(lastAudioPts_, 1000000, (long int)audio_time_base_);
                    int64_t now = av_gettime() - startTime_;
                    if (pts > now){
                        av_usleep(pts - now);
                    }
                    lastAudioPts_ = avpacket_.pts;

                    ELOG_DEBUG("Audio and package %d ==> length, dts=%ld,duration=%d,pos=%ld, pts=%ld", avpacket_.size, avpacket_.dts, avpacket_.duration, avpacket_.pos, avpacket_.pts);

                    op_->packageAudio(avpacket_.data, avpacket_.size, decodedBuffer_.get(), avpacket_.pts);
                }
            }
            av_free_packet(&orig_pkt);
        }
        running_=false;
        av_read_pause(context_);
    }

    void ExternalInput::encodeLoop() {
        while (running_ == true) {
            queueMutex_.lock();
            if (packetQueue_.size() > 0) {
                op_->receiveRawData(packetQueue_.front());
                packetQueue_.pop();
                queueMutex_.unlock();
            } else {
                queueMutex_.unlock();
                usleep(10000);
            }
        }
    }

}

