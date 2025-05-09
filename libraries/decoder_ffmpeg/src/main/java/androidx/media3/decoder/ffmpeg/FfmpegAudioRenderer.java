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
package androidx.media3.decoder.ffmpeg;

import static androidx.media3.exoplayer.audio.AudioSink.SINK_FORMAT_SUPPORTED_DIRECTLY;
import static androidx.media3.exoplayer.audio.AudioSink.SINK_FORMAT_SUPPORTED_WITH_TRANSCODING;
import static androidx.media3.exoplayer.audio.AudioSink.SINK_FORMAT_UNSUPPORTED;

import android.os.Handler;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.audio.AudioProcessor;
import androidx.media3.common.util.Assertions;
import androidx.media3.common.util.TraceUtil;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.util.Util;
import androidx.media3.decoder.CryptoConfig;
import androidx.media3.exoplayer.audio.AudioRendererEventListener;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.AudioSink.SinkFormatSupport;
import androidx.media3.exoplayer.audio.DecoderAudioRenderer;
import androidx.media3.exoplayer.audio.DefaultAudioSink;

/** Decodes and renders audio using FFmpeg. */
@UnstableApi
public final class FfmpegAudioRenderer extends DecoderAudioRenderer<FfmpegAudioDecoder> {

  private static final String TAG = "FfmpegAudioRenderer";

  /** The number of input and output buffers. */
  private static final int NUM_BUFFERS = 16;

  /** The default input buffer size. */
  private static final int DEFAULT_INPUT_BUFFER_SIZE = 960 * 6;
  
  /** A larger buffer size for DSD content. */
  private static final int DSD_INPUT_BUFFER_SIZE = DEFAULT_INPUT_BUFFER_SIZE * 8;

  public FfmpegAudioRenderer() {
    this(/* eventHandler= */ null, /* eventListener= */ null);
  }

  /**
   * Creates a new instance.
   *
   * @param eventHandler A handler to use when delivering events to {@code eventListener}. May be
   *     null if delivery of events is not required.
   * @param eventListener A listener of events. May be null if delivery of events is not required.
   * @param audioProcessors Optional {@link AudioProcessor}s that will process audio before output.
   */
  public FfmpegAudioRenderer(
      @Nullable Handler eventHandler,
      @Nullable AudioRendererEventListener eventListener,
      AudioProcessor... audioProcessors) {
    this(
        eventHandler,
        eventListener,
        new DefaultAudioSink.Builder().setAudioProcessors(audioProcessors).build());
  }

  /**
   * Creates a new instance.
   *
   * @param eventHandler A handler to use when delivering events to {@code eventListener}. May be
   *     null if delivery of events is not required.
   * @param eventListener A listener of events. May be null if delivery of events is not required.
   * @param audioSink The sink to which audio will be output.
   */
  public FfmpegAudioRenderer(
      @Nullable Handler eventHandler,
      @Nullable AudioRendererEventListener eventListener,
      AudioSink audioSink) {
    super(eventHandler, eventListener, audioSink);
  }

  @Override
  public String getName() {
    return TAG;
  }

  @Override
  protected @C.FormatSupport int supportsFormatInternal(Format format) {
    String mimeType = Assertions.checkNotNull(format.sampleMimeType);
    if (!FfmpegLibrary.isAvailable() || !MimeTypes.isAudio(mimeType)) {
      return C.FORMAT_UNSUPPORTED_TYPE;
    } else if (!FfmpegLibrary.supportsFormat(mimeType)
        || (!sinkSupportsFormat(format, C.ENCODING_PCM_16BIT)
            && !sinkSupportsFormat(format, C.ENCODING_PCM_FLOAT))) {
      return C.FORMAT_UNSUPPORTED_SUBTYPE;
    } else if (format.cryptoType != C.CRYPTO_TYPE_NONE) {
      return C.FORMAT_UNSUPPORTED_DRM;
    } else {
      return C.FORMAT_HANDLED;
    }
  }

  @Override
  public @AdaptiveSupport int supportsMixedMimeTypeAdaptation() {
    return ADAPTIVE_NOT_SEAMLESS;
  }

  /**
   * Checks if the format is a DSD audio format.
   */
  private boolean isDsdFormat(String mimeType) {
    return "audio/dsf".equals(mimeType) 
        || "audio/x-dsf".equals(mimeType)
        || "audio/dsd_lsbf".equals(mimeType)
        || "audio/dsdiff".equals(mimeType)
        || "audio/x-dsdiff".equals(mimeType)
        || "audio/x-dff".equals(mimeType)
        || "audio/dsd_msbf".equals(mimeType)
        || "audio/dsd".equals(mimeType);
  }

  /**
   * {@inheritDoc}
   *
   * @hide
   */
  @Override
  protected FfmpegAudioDecoder createDecoder(Format format, @Nullable CryptoConfig cryptoConfig)
      throws FfmpegDecoderException {
    TraceUtil.beginSection("createFfmpegAudioDecoder");
    
    // Check if this is a DSD format
    String mimeType = Assertions.checkNotNull(format.sampleMimeType);
    boolean isDsd = isDsdFormat(mimeType);
    
    // Use a larger buffer for DSD content
    int initialInputBufferSize =
        format.maxInputSize != Format.NO_VALUE 
            ? format.maxInputSize 
            : (isDsd ? DSD_INPUT_BUFFER_SIZE : DEFAULT_INPUT_BUFFER_SIZE);
    
    // For DSD, we should always use float output
    boolean outputFloat = isDsd || shouldOutputFloat(format);
    
    FfmpegAudioDecoder decoder =
        new FfmpegAudioDecoder(
            format, NUM_BUFFERS, NUM_BUFFERS, initialInputBufferSize, outputFloat, isDsd);
    
    TraceUtil.endSection();
    return decoder;
  }

  /**
   * {@inheritDoc}
   *
   * @hide
   */
  @Override
  protected Format getOutputFormat(FfmpegAudioDecoder decoder) {
  Assertions.checkNotNull(decoder);
  int sampleRate = decoder.getSampleRate();
  
  // Additional safety check for valid sample rates
  if (sampleRate < 8000 || sampleRate > 192000) {
    // Force a compatible PCM rate if we get an out-of-range value
    sampleRate = 44100;
  }
  
  return new Format.Builder()
      .setSampleMimeType(MimeTypes.AUDIO_RAW)
      .setChannelCount(decoder.getChannelCount())
      .setSampleRate(sampleRate)
      .setPcmEncoding(decoder.getEncoding())
      .build();
  }

  /**
   * Returns whether the renderer's {@link AudioSink} supports the PCM format that will be output
   * from the decoder for the given input format and requested output encoding.
   */
  private boolean sinkSupportsFormat(Format inputFormat, @C.PcmEncoding int pcmEncoding) {
    return sinkSupportsFormat(
        Util.getPcmFormat(pcmEncoding, inputFormat.channelCount, inputFormat.sampleRate));
  }

  private boolean shouldOutputFloat(Format inputFormat) {
    String mimeType = Assertions.checkNotNull(inputFormat.sampleMimeType);
    
    // Always use floating point for DSD formats
    if (isDsdFormat(mimeType)) {
      return true;
    }
    
    if (!sinkSupportsFormat(inputFormat, C.ENCODING_PCM_16BIT)) {
      // We have no choice because the sink doesn't support 16-bit integer PCM.
      return true;
    }

    @SinkFormatSupport
    int formatSupport =
        getSinkFormatSupport(
            Util.getPcmFormat(
                C.ENCODING_PCM_FLOAT, inputFormat.channelCount, inputFormat.sampleRate));
    switch (formatSupport) {
      case SINK_FORMAT_SUPPORTED_DIRECTLY:
        // AC-3 is always 16-bit, so there's no point using floating point. Assume that it's worth
        // using for all other formats.
        return !MimeTypes.AUDIO_AC3.equals(inputFormat.sampleMimeType);
      case SINK_FORMAT_UNSUPPORTED:
      case SINK_FORMAT_SUPPORTED_WITH_TRANSCODING:
      default:
        // Always prefer 16-bit PCM if the sink does not provide direct support for floating point.
        return false;
    }
  }
}
