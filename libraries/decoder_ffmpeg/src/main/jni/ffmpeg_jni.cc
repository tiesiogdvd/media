/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG "ffmpeg_jni"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define LOGD(...) \
  ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))

#define LIBRARY_FUNC(RETURN_TYPE, NAME, ...)                                   \
  extern "C" {                                                                 \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(JNIEnv *env,    \
                                                               jobject thiz,   \
                                                               ##__VA_ARGS__); \
  }                                                                            \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(                \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define AUDIO_DECODER_FUNC(RETURN_TYPE, NAME, ...)                   \
  extern "C" {                                                       \
  JNIEXPORT RETURN_TYPE                                              \
      Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__);                 \
  }                                                                  \
  JNIEXPORT RETURN_TYPE                                              \
      Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define ERROR_STRING_BUFFER_LENGTH 256

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;
// For DSD, use special handling
static const AVSampleFormat OUTPUT_FORMAT_DSD = AV_SAMPLE_FMT_FLT; // We'll convert DSD to float

// LINT.IfChange
static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;
// LINT.ThenChange(../java/androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder.java)

static const int DSD_OUTPUT_SAMPLE_RATE = 44100; // or 48000

// DSD-specific parameters
struct DsdContext {
  bool isDsd;
  int dsdSampleRate;
  // Additional parameters needed for DSD processing
};

static jmethodID growOutputBufferMethod;

/**
 * Returns the AVCodec with the specified name, or NULL if it is not available.
 */
const AVCodec *getCodecByName(JNIEnv *env, jstring codecName);

/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * Returns the created context.
 */
AVCodecContext *createContext(JNIEnv *env, const AVCodec *codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jboolean isDsd);

struct GrowOutputBufferCallback {
  uint8_t *operator()(int requiredSize) const;

  JNIEnv *env;
  jobject thiz;
  jobject decoderOutputBuffer;
};

/**
 * Decodes the packet into the output buffer, returning the number of bytes
 * written, or a negative AUDIO_DECODER_ERROR constant value in the case of an
 * error.
 */
int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize,
                 GrowOutputBufferCallback growBuffer);

/**
 * Specialized handling for DSD decoding.
 */
int decodeDsdPacket(AVCodecContext *context, AVPacket *packet,
                   uint8_t *outputBuffer, int outputSize,
                   GrowOutputBufferCallback growBuffer,
                   DsdContext *dsdContext);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

/**
 * Outputs a log message describing the avcodec error number.
 */
void logError(const char *functionName, int errorNumber);

/**
 * Releases the specified context.
 */
void releaseContext(AVCodecContext *context);

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JNI_OnLoad: GetEnv failed");
    return -1;
  }
  jclass clazz =
      env->FindClass("androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder");
  if (!clazz) {
    LOGE("JNI_OnLoad: FindClass failed");
    return -1;
  }
  growOutputBufferMethod =
      env->GetMethodID(clazz, "growOutputBuffer",
                       "(Landroidx/media3/decoder/"
                       "SimpleDecoderOutputBuffer;I)Ljava/nio/ByteBuffer;");
  if (!growOutputBufferMethod) {
    LOGE("JNI_OnLoad: GetMethodID failed");
    return -1;
  }
  return JNI_VERSION_1_6;
}

LIBRARY_FUNC(jstring, ffmpegGetVersion) {
  return env->NewStringUTF(LIBAVCODEC_IDENT);
}

LIBRARY_FUNC(jint, ffmpegGetInputBufferPaddingSize) {
  return (jint)AV_INPUT_BUFFER_PADDING_SIZE;
}

LIBRARY_FUNC(jboolean, ffmpegHasDecoder, jstring codecName) {
  return getCodecByName(env, codecName) != NULL;
}

AUDIO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jboolean outputFloat,
                   jint rawSampleRate, jint rawChannelCount,
                   jboolean isDsd) {
  const AVCodec *codec = getCodecByName(env, codecName);
  if (!codec) {
    LOGE("Codec not found.");
    return 0L;
  }
  return (jlong)createContext(env, codec, extraData, outputFloat, rawSampleRate,
                              rawChannelCount, isDsd);
}

AUDIO_DECODER_FUNC(jint, ffmpegDecode, jlong context, jobject inputData,
                   jint inputSize, jobject decoderOutputBuffer,
                   jobject outputData, jint outputSize) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  if (!inputData || !decoderOutputBuffer || !outputData) {
    LOGE("Input and output buffers must be non-NULL.");
    return -1;
  }
  if (inputSize < 0) {
    LOGE("Invalid input buffer size: %d.", inputSize);
    return -1;
  }
  if (outputSize < 0) {
    LOGE("Invalid output buffer length: %d", outputSize);
    return -1;
  }
  uint8_t *inputBuffer = (uint8_t *)env->GetDirectBufferAddress(inputData);
  uint8_t *outputBuffer = (uint8_t *)env->GetDirectBufferAddress(outputData);
  AVPacket *packet = av_packet_alloc();
  if (!packet) {
    LOGE("Failed to allocate packet.");
    return -1;
  }
  packet->data = inputBuffer;
  packet->size = inputSize;
  
  AVCodecContext *codecContext = (AVCodecContext *)context;
  DsdContext *dsdContext = (DsdContext *)codecContext->opaque;
  
  int ret;
  if (dsdContext && dsdContext->isDsd) {
    // Use specialized DSD handling
    ret = decodeDsdPacket(codecContext, packet, outputBuffer, outputSize,
                         GrowOutputBufferCallback{env, thiz, decoderOutputBuffer},
                         dsdContext);
  } else {
    // Use standard decoding path
    ret = decodePacket(codecContext, packet, outputBuffer, outputSize,
                      GrowOutputBufferCallback{env, thiz, decoderOutputBuffer});
  }
  
  av_packet_free(&packet);
  return ret;
}

uint8_t *GrowOutputBufferCallback::operator()(int requiredSize) const {
  jobject newOutputData = env->CallObjectMethod(
      thiz, growOutputBufferMethod, decoderOutputBuffer, requiredSize);
  if (env->ExceptionCheck()) {
    LOGE("growOutputBuffer() failed");
    env->ExceptionDescribe();
    return nullptr;
  }
  return static_cast<uint8_t *>(env->GetDirectBufferAddress(newOutputData));
}

AUDIO_DECODER_FUNC(jint, ffmpegGetChannelCount, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  return ((AVCodecContext *)context)->ch_layout.nb_channels;
}

AUDIO_DECODER_FUNC(jint, ffmpegGetSampleRate, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  
  AVCodecContext *codecContext = (AVCodecContext *)context;
  DsdContext *dsdContext = (DsdContext *)codecContext->opaque;
  
  // For DSD, always return a standard PCM rate
  if (dsdContext && dsdContext->isDsd) {
    // Return the downsampled PCM rate, never the original DSD rate
    return 44100; // or 48000, must match the rate used in resampling
  }
  
  return codecContext->sample_rate;
}

AUDIO_DECODER_FUNC(jlong, ffmpegReset, jlong jContext, jbyteArray extraData) {
  AVCodecContext *context = (AVCodecContext *)jContext;
  if (!context) {
    LOGE("Tried to reset without a context.");
    return 0L;
  }

  AVCodecID codecId = context->codec_id;
  // Store DSD context info if present since we'll need to recreate it
  DsdContext *dsdContext = (DsdContext *)context->opaque;
  bool isDsd = false;
  int dsdSampleRate = 0;
  
  if (dsdContext && dsdContext->isDsd) {
    isDsd = true;
    dsdSampleRate = dsdContext->dsdSampleRate;
  }
  
  if (codecId == AV_CODEC_ID_TRUEHD || isDsd) {
    // Release and recreate the context if the codec is TrueHD or DSD.
    // Some codecs need full reinitialization
    releaseContext(context);
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec) {
      LOGE("Unexpected error finding codec %d.", codecId);
      return 0L;
    }
    jboolean outputFloat =
        (jboolean)(context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
    return (jlong)createContext(env, codec, extraData, outputFloat,
                               /* rawSampleRate= */ isDsd ? dsdSampleRate : -1,
                               /* rawChannelCount= */ -1,
                               /* isDsd= */ isDsd);
  }

  avcodec_flush_buffers(context);
  return (jlong)context;
}

AUDIO_DECODER_FUNC(void, ffmpegRelease, jlong context) {
  if (context) {
    releaseContext((AVCodecContext *)context);
  }
}

const AVCodec *getCodecByName(JNIEnv *env, jstring codecName) {
  if (!codecName) {
    return NULL;
  }
  const char *codecNameChars = env->GetStringUTFChars(codecName, NULL);
  const AVCodec *codec = avcodec_find_decoder_by_name(codecNameChars);
  env->ReleaseStringUTFChars(codecName, codecNameChars);
  return codec;
}

AVCodecContext *createContext(JNIEnv *env, const AVCodec *codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jboolean isDsd) {
  AVCodecContext *context = avcodec_alloc_context3(codec);
  if (!context) {
    LOGE("Failed to allocate context.");
    return NULL;
  }
  
  // Setup DSD context if needed
  if (isDsd) {
    DsdContext *dsdContext = (DsdContext *)malloc(sizeof(DsdContext));
    if (!dsdContext) {
      LOGE("Failed to allocate DSD context");
      releaseContext(context);
      return NULL;
    }
    dsdContext->isDsd = true;
    dsdContext->dsdSampleRate = DSD_OUTPUT_SAMPLE_RATE;
    context->opaque = dsdContext;
    
    // For DSD, we'll handle output format specially
    context->request_sample_fmt = OUTPUT_FORMAT_DSD;
  } else {
    context->request_sample_fmt =
        outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
  }
  
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    context->extradata_size = size;
    context->extradata =
        (uint8_t *)av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      LOGE("Failed to allocate extradata.");
      releaseContext(context);
      return NULL;
    }
    env->GetByteArrayRegion(extraData, 0, size, (jbyte *)context->extradata);
  }
  
  // Special codec handling for sample rates and channel layouts
  if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
      context->codec_id == AV_CODEC_ID_PCM_ALAW ||
      isDsd) {
    // Use specific sample rate for these codecs
    context->sample_rate = rawSampleRate > 0 ? rawSampleRate : 
                          (isDsd ? 2822400 : 44100); // Default rates
    
    // Use specified channel count or default to stereo
    av_channel_layout_default(&context->ch_layout, 
                            rawChannelCount > 0 ? rawChannelCount : 2);
  }
  
  context->err_recognition = AV_EF_IGNORE_ERR;
  int result = avcodec_open2(context, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseContext(context);
    return NULL;
  }
  return context;
}

int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize,
                 GrowOutputBufferCallback growBuffer) {
  int result = 0;
  // Queue input data.
  result = avcodec_send_packet(context, packet);
  if (result) {
    logError("avcodec_send_packet", result);
    return transformError(result);
  }

  // Dequeue output data until it runs out.
  int outSize = 0;
  while (true) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
      LOGE("Failed to allocate output frame.");
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    result = avcodec_receive_frame(context, frame);
    if (result) {
      av_frame_free(&frame);
      if (result == AVERROR(EAGAIN)) {
        break;
      }
      logError("avcodec_receive_frame", result);
      return transformError(result);
    }

    // Resample output.
    AVSampleFormat sampleFormat = context->sample_fmt;
    int channelCount = context->ch_layout.nb_channels;
    int sampleRate = context->sample_rate;
    int sampleCount = frame->nb_samples;
    int dataSize = av_samples_get_buffer_size(NULL, channelCount, sampleCount,
                                              sampleFormat, 1);
    SwrContext *resampleContext = static_cast<SwrContext *>(context->opaque);
    if (!resampleContext) {
      result =
          swr_alloc_set_opts2(&resampleContext,             // ps
                              &context->ch_layout,          // out_ch_layout
                              context->request_sample_fmt,   // out_sample_fmt
                              sampleRate,                   // out_sample_rate
                              &context->ch_layout,          // in_ch_layout
                              sampleFormat,                 // in_sample_fmt
                              sampleRate,                   // in_sample_rate
                              0,                            // log_offset
                              NULL                          // log_ctx
          );
      if (result < 0) {
        logError("swr_alloc_set_opts2", result);
        av_frame_free(&frame);
        return transformError(result);
      }
      result = swr_init(resampleContext);
      if (result < 0) {
        logError("swr_init", result);
        av_frame_free(&frame);
        return transformError(result);
      }
      context->opaque = resampleContext;
    }

    int outSampleSize = av_get_bytes_per_sample(context->request_sample_fmt);
    int outSamples = swr_get_out_samples(resampleContext, sampleCount);
    int bufferOutSize = outSampleSize * channelCount * outSamples;
    if (outSize + bufferOutSize > outputSize) {
      LOGD(
          "Output buffer size (%d) too small for output data (%d), "
          "reallocating buffer.",
          outputSize, outSize + bufferOutSize);
      outputSize = outSize + bufferOutSize;
      outputBuffer = growBuffer(outputSize);
      if (!outputBuffer) {
        LOGE("Failed to reallocate output buffer.");
        av_frame_free(&frame);
        return AUDIO_DECODER_ERROR_OTHER;
      }
    }
    result = swr_convert(resampleContext, &outputBuffer, bufferOutSize,
                         (const uint8_t **)frame->data, frame->nb_samples);
    av_frame_free(&frame);
    if (result < 0) {
      logError("swr_convert", result);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    int available = swr_get_out_samples(resampleContext, 0);
    if (available != 0) {
      LOGE("Expected no samples remaining after resampling, but found %d.",
           available);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    outputBuffer += bufferOutSize;
    outSize += bufferOutSize;
  }
  return outSize;
}

// Special handling for DSD data
int decodeDsdPacket(AVCodecContext *context, AVPacket *packet,
                   uint8_t *outputBuffer, int outputSize,
                   GrowOutputBufferCallback growBuffer,
                   DsdContext *dsdContext) {
  int result = 0;
  // Queue input data
  result = avcodec_send_packet(context, packet);
  if (result) {
    logError("avcodec_send_packet", result);
    return transformError(result);
  }

  // Dequeue output data until it runs out
  int outSize = 0;
  while (true) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
      LOGE("Failed to allocate output frame.");
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    result = avcodec_receive_frame(context, frame);
    if (result) {
      av_frame_free(&frame);
      if (result == AVERROR(EAGAIN)) {
        break;
      }
      logError("avcodec_receive_frame", result);
      return transformError(result);
    }

    // For DSD we need special handling
    AVSampleFormat sampleFormat = context->sample_fmt;
    int channelCount = context->ch_layout.nb_channels;
    int sampleRate = context->sample_rate;
    int sampleCount = frame->nb_samples;
    
    // DSD data can be very dense, ensure we have enough buffer space
    int dsdBytesPerSample = av_get_bytes_per_sample(sampleFormat);
    int outSampleSize = av_get_bytes_per_sample(context->request_sample_fmt);
    
    // For DSD we might need to downsample from extremely high rates
    // This ratio controls how many DSD samples become one PCM sample
    int conversionRatio = sampleRate / 44100; // Example: convert to 44.1kHz
    if (conversionRatio < 1) conversionRatio = 1;
    
    // For DSD->PCM we need specialized resampling
    SwrContext *resampleContext = static_cast<SwrContext *>(context->opaque);
    if (!resampleContext) {
      // Create a custom resampler for DSD
      // We need to convert the high sample rate DSD data to a more reasonable PCM rate
      // First, prepare a suitable output channel layout
      result = swr_alloc_set_opts2(&resampleContext,
                          &context->ch_layout,        // out_ch_layout
                          context->request_sample_fmt, // out_sample_fmt
                          DSD_OUTPUT_SAMPLE_RATE,     // Use constant PCM rate
                          &context->ch_layout,        // in_ch_layout
                          sampleFormat,               // in_sample_fmt
                          sampleRate,                 // in_sample_rate (original high DSD rate)
                          0,                         
                          NULL);
      
      if (result < 0) {
        logError("swr_alloc_set_opts2", result);
        av_frame_free(&frame);
        return transformError(result);
      }
      
      // Initialize the resampler
      result = swr_init(resampleContext);
      if (result < 0) {
        logError("swr_init", result);
        av_frame_free(&frame);
        return transformError(result);
      }
      
      // Store the resampler in the context for reuse
      context->opaque = resampleContext;
      
      // Update the DSD context with the output sample rate
      dsdContext->dsdSampleRate = 44100; // The rate we're converting to
    }
    
    // Calculate output samples after conversion
    int outSamples = swr_get_out_samples(resampleContext, sampleCount);
    int bufferOutSize = outSampleSize * channelCount * outSamples;
    
    // Ensure our output buffer is large enough
    if (outSize + bufferOutSize > outputSize) {
      LOGD(
          "Output buffer size (%d) too small for DSD output data (%d), "
          "reallocating buffer.",
          outputSize, outSize + bufferOutSize);
      outputSize = outSize + bufferOutSize;
      outputBuffer = growBuffer(outputSize);
      if (!outputBuffer) {
        LOGE("Failed to reallocate output buffer for DSD.");
        av_frame_free(&frame);
        return AUDIO_DECODER_ERROR_OTHER;
      }
    }
    
    // Perform the actual conversion from DSD to PCM
    result = swr_convert(resampleContext, 
                        &outputBuffer, outSamples,
                        (const uint8_t **)frame->data, frame->nb_samples);
    
    av_frame_free(&frame);
    if (result < 0) {
      logError("swr_convert for DSD", result);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    
    // Check if any samples are remaining in the resampler
    int available = swr_get_out_samples(resampleContext, 0);
    if (available > 0) {
      // We have leftover samples, convert them too
      int additionalSize = outSampleSize * channelCount * available;
      
      // Make sure our buffer is still large enough
      if (outSize + bufferOutSize + additionalSize > outputSize) {
        outputSize = outSize + bufferOutSize + additionalSize;
        uint8_t *newBuffer = growBuffer(outputSize);
        if (!newBuffer) {
          LOGE("Failed to reallocate output buffer for DSD leftovers.");
          return AUDIO_DECODER_ERROR_OTHER;
        }
        // Update buffer pointer to the new location
        outputBuffer = newBuffer + outSize + bufferOutSize;
      } else {
        // Advance the buffer pointer past the data we've already written
        outputBuffer += bufferOutSize;
      }
      
      // Convert the remaining samples
      result = swr_convert(resampleContext, 
                          &outputBuffer, available,
                          NULL, 0);  // NULL for input means "flush"
      
      if (result < 0) {
        logError("swr_convert flush for DSD", result);
        return AUDIO_DECODER_ERROR_INVALID_DATA;
      }
      
      // Add these bytes to our output size
      bufferOutSize += additionalSize;
    }
    
    // Update pointers and sizes
    outputBuffer += bufferOutSize;
    outSize += bufferOutSize;
  }
  
  return outSize;
}

int transformError(int errorNumber) {
  return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                            : AUDIO_DECODER_ERROR_OTHER;
}

void logError(const char *functionName, int errorNumber) {
  char *buffer = (char *)malloc(ERROR_STRING_BUFFER_LENGTH * sizeof(char));
  av_strerror(errorNumber, buffer, ERROR_STRING_BUFFER_LENGTH);
  LOGE("Error in %s: %s", functionName, buffer);
  free(buffer);
}

void releaseContext(AVCodecContext *context) {
  if (!context) {
    return;
  }
  
  // Check if it's a DSD context or a regular context
  DsdContext *dsdContext = NULL;
  SwrContext *swrContext = NULL;
  
  void *opaquePtr = context->opaque;
  if (opaquePtr) {
    // First try to interpret as DsdContext 
    dsdContext = static_cast<DsdContext*>(opaquePtr);
    if (dsdContext && dsdContext->isDsd) {
      // It's a DSD context, free it
      free(dsdContext);
      context->opaque = NULL;
    } else {
      // Not a DSD context, try as SwrContext
      swrContext = static_cast<SwrContext*>(opaquePtr);
      if (swrContext) {
        swr_free(&swrContext);
        context->opaque = NULL;
      }
    }
  }
  
  avcodec_free_context(&context);
}
