#if defined(BEDLAM_USE_SDL_AUDIO_MIXER)

#    include "bedlam2_audio_mixer.h"

#    include <algorithm>
#    include <cstdlib>
#    include <cstring>
#    include <vector>

namespace
{
    constexpr int kDefaultChannels = 8;
    constexpr int kMixBufferSamples = 8192;

    struct Channel
    {
        Mix_Chunk *chunk = nullptr;
        Uint32 position = 0;
        int loops_remaining = 0;
        int volume = MIX_MAX_VOLUME;
        Uint8 left = 255;
        Uint8 right = 255;
        Uint64 fade_total = 0;
        Uint64 fade_remaining = 0;
    };

    struct Mixer
    {
        SDL_AudioDeviceID device = 0;
        int frequency = 0;
        std::vector<Channel> channels;
        int reserved_channels = 0;
        std::vector<int32_t> accumulation;
    };

    Mixer g_mixer;

    void audio_callback(void *userdata, Uint8 *stream, int length)
    {
        Mixer *mixer = static_cast<Mixer *>(userdata);
        std::memset(stream, 0, static_cast<size_t>(length));
        if (!mixer || mixer->frequency <= 0 || length < 4)
        {
            return;
        }

        int16_t *output = reinterpret_cast<int16_t *>(stream);
        const int frame_count = length / 4;
        int frame_base = 0;
        while (frame_base < frame_count)
        {
            const int frames = std::min(frame_count - frame_base, kMixBufferSamples / 2);
            const int sample_count = frames * 2;
            std::fill(mixer->accumulation.begin(), mixer->accumulation.begin() + sample_count, 0);

            for (Channel &channel : mixer->channels)
            {
                Mix_Chunk *chunk = channel.chunk;
                if (!chunk || !chunk->abuf || chunk->alen < 4)
                {
                    continue;
                }

                const int16_t *samples = reinterpret_cast<const int16_t *>(chunk->abuf);
                const Uint32 total_frames = chunk->alen / 4;
                for (int frame = 0; frame < frames; ++frame)
                {
                    Uint32 source_frame = channel.position / 4;
                    if (source_frame >= total_frames)
                    {
                        if (channel.loops_remaining == 0)
                        {
                            channel.chunk = nullptr;
                            break;
                        }
                        if (channel.loops_remaining > 0)
                        {
                            --channel.loops_remaining;
                        }
                        channel.position = 0;
                        source_frame = 0;
                    }

                    int volume = channel.volume;
                    if (channel.fade_total > 0)
                    {
                        volume = static_cast<int>(static_cast<Uint64>(volume) * channel.fade_remaining / channel.fade_total);
                        if (channel.fade_remaining > 0)
                        {
                            --channel.fade_remaining;
                        }
                        if (channel.fade_remaining == 0)
                        {
                            channel.chunk = nullptr;
                        }
                    }

                    const int left = samples[source_frame * 2];
                    const int right = samples[source_frame * 2 + 1];
                    const int chunk_volume = chunk->volume;
                    const int denominator = MIX_MAX_VOLUME * MIX_MAX_VOLUME * 255;
                    mixer->accumulation[frame * 2] += static_cast<int32_t>(
                        static_cast<int64_t>(left) * volume * chunk_volume * channel.left / denominator);
                    mixer->accumulation[frame * 2 + 1] += static_cast<int32_t>(
                        static_cast<int64_t>(right) * volume * chunk_volume * channel.right / denominator);
                    channel.position += 4;

                    if (!channel.chunk)
                    {
                        break;
                    }
                }
            }

            for (int sample = 0; sample < sample_count; ++sample)
            {
                const int32_t mixed = mixer->accumulation[sample];
                output[frame_base * 2 + sample] = static_cast<int16_t>(
                    std::max<int32_t>(-32768, std::min<int32_t>(32767, mixed)));
            }
            frame_base += frames;
        }
    }

    bool channel_in_range(int channel)
    {
        return channel >= 0 && static_cast<size_t>(channel) < g_mixer.channels.size();
    }

    void lock_audio()
    {
        if (g_mixer.device)
        {
            SDL_LockAudioDevice(g_mixer.device);
        }
    }

    void unlock_audio()
    {
        if (g_mixer.device)
        {
            SDL_UnlockAudioDevice(g_mixer.device);
        }
    }

    void stop_channel(Channel &channel)
    {
        channel.chunk = nullptr;
        channel.position = 0;
        channel.loops_remaining = 0;
        channel.fade_total = 0;
        channel.fade_remaining = 0;
    }
} // namespace

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunk_size)
{
    if (g_mixer.device || frequency <= 0 || channels != 2 || format != AUDIO_S16SYS)
    {
        SDL_SetError("Bedlam audio requires signed 16-bit stereo output");
        return -1;
    }

    SDL_AudioSpec desired{};
    desired.freq = frequency;
    desired.format = format;
    desired.channels = static_cast<Uint8>(channels);
    desired.samples = static_cast<Uint16>(std::max(64, std::min(chunk_size, 8192)));
    desired.callback = audio_callback;
    desired.userdata = &g_mixer;

    g_mixer.channels.assign(kDefaultChannels, Channel{});
    g_mixer.accumulation.assign(kMixBufferSamples, 0);
    g_mixer.frequency = frequency;
    g_mixer.reserved_channels = 0;
    g_mixer.device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (!g_mixer.device)
    {
        g_mixer.channels.clear();
        g_mixer.accumulation.clear();
        g_mixer.frequency = 0;
        return -1;
    }
    SDL_PauseAudioDevice(g_mixer.device, 0);
    return 0;
}

void Mix_CloseAudio()
{
    if (g_mixer.device)
    {
        SDL_PauseAudioDevice(g_mixer.device, 1);
        SDL_CloseAudioDevice(g_mixer.device);
        g_mixer.device = 0;
    }
    g_mixer.channels.clear();
    g_mixer.accumulation.clear();
    g_mixer.frequency = 0;
    g_mixer.reserved_channels = 0;
}

int Mix_AllocateChannels(int channels)
{
    lock_audio();
    if (channels < 0)
    {
        channels = static_cast<int>(g_mixer.channels.size());
    }
    if (static_cast<size_t>(channels) < g_mixer.channels.size())
    {
        for (size_t i = static_cast<size_t>(channels); i < g_mixer.channels.size(); ++i)
        {
            stop_channel(g_mixer.channels[i]);
        }
    }
    g_mixer.channels.resize(static_cast<size_t>(channels));
    g_mixer.reserved_channels = std::min(g_mixer.reserved_channels, channels);
    unlock_audio();
    return channels;
}

int Mix_ReserveChannels(int channels)
{
    lock_audio();
    g_mixer.reserved_channels = std::max(0, std::min(channels, static_cast<int>(g_mixer.channels.size())));
    const int result = g_mixer.reserved_channels;
    unlock_audio();
    return result;
}

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *source, int free_source)
{
    if (!source)
    {
        SDL_SetError("No WAV source supplied");
        return nullptr;
    }

    SDL_AudioSpec source_spec{};
    Uint8 *source_buffer = nullptr;
    Uint32 source_length = 0;
    if (!SDL_LoadWAV_RW(source, free_source, &source_spec, &source_buffer, &source_length))
    {
        return nullptr;
    }

    const int output_frequency = g_mixer.frequency > 0 ? g_mixer.frequency : 44100;
    SDL_AudioStream *converter = SDL_NewAudioStream(
        source_spec.format, source_spec.channels, source_spec.freq, AUDIO_S16SYS, 2, output_frequency);
    if (!converter || SDL_AudioStreamPut(converter, source_buffer, static_cast<int>(source_length)) < 0
        || SDL_AudioStreamFlush(converter) < 0)
    {
        SDL_FreeWAV(source_buffer);
        if (converter)
        {
            SDL_FreeAudioStream(converter);
        }
        return nullptr;
    }

    const int available = SDL_AudioStreamAvailable(converter);
    if (available < 4)
    {
        SDL_FreeWAV(source_buffer);
        SDL_FreeAudioStream(converter);
        SDL_SetError("WAV contains no converted audio samples");
        return nullptr;
    }

    const Uint32 length = static_cast<Uint32>(available - (available % 4));
    Mix_Chunk *chunk = static_cast<Mix_Chunk *>(SDL_malloc(sizeof(Mix_Chunk)));
    Uint8 *buffer = static_cast<Uint8 *>(SDL_malloc(length));
    if (!chunk || !buffer)
    {
        SDL_free(chunk);
        SDL_free(buffer);
        SDL_FreeWAV(source_buffer);
        SDL_FreeAudioStream(converter);
        SDL_SetError("Out of memory loading WAV chunk");
        return nullptr;
    }

    const int received = SDL_AudioStreamGet(converter, buffer, static_cast<int>(length));
    SDL_FreeWAV(source_buffer);
    SDL_FreeAudioStream(converter);
    if (received != static_cast<int>(length))
    {
        SDL_free(buffer);
        SDL_free(chunk);
        SDL_SetError("Unable to convert complete WAV chunk");
        return nullptr;
    }

    chunk->allocated = 1;
    chunk->abuf = buffer;
    chunk->alen = length;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

void Mix_FreeChunk(Mix_Chunk *chunk)
{
    if (!chunk)
    {
        return;
    }
    lock_audio();
    for (Channel &channel : g_mixer.channels)
    {
        if (channel.chunk == chunk)
        {
            stop_channel(channel);
        }
    }
    SDL_free(chunk->abuf);
    chunk->abuf = nullptr;
    SDL_free(chunk);
    unlock_audio();
}

int Mix_Volume(int channel, int volume)
{
    lock_audio();
    if (channel == -1)
    {
        const int old_volume = g_mixer.channels.empty() ? MIX_MAX_VOLUME : g_mixer.channels.front().volume;
        if (volume >= 0)
        {
            for (Channel &item : g_mixer.channels)
            {
                item.volume = std::max(0, std::min(volume, MIX_MAX_VOLUME));
            }
        }
        unlock_audio();
        return old_volume;
    }
    if (!channel_in_range(channel))
    {
        unlock_audio();
        return -1;
    }
    const int old_volume = g_mixer.channels[static_cast<size_t>(channel)].volume;
    if (volume >= 0)
    {
        g_mixer.channels[static_cast<size_t>(channel)].volume = std::max(0, std::min(volume, MIX_MAX_VOLUME));
    }
    unlock_audio();
    return old_volume;
}

int Mix_VolumeChunk(Mix_Chunk *chunk, int volume)
{
    if (!chunk)
    {
        return -1;
    }
    lock_audio();
    const int previous = chunk->volume;
    if (volume >= 0)
    {
        chunk->volume = static_cast<Uint8>(std::max(0, std::min(volume, MIX_MAX_VOLUME)));
    }
    unlock_audio();
    return previous;
}

int Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
    lock_audio();
    if (!channel_in_range(channel))
    {
        unlock_audio();
        return 0;
    }
    Channel &item = g_mixer.channels[static_cast<size_t>(channel)];
    item.left = left;
    item.right = right;
    unlock_audio();
    return 1;
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    if (!chunk || !chunk->abuf || chunk->alen < 4)
    {
        return -1;
    }
    lock_audio();
    if (channel < 0)
    {
        channel = g_mixer.reserved_channels;
        while (channel < static_cast<int>(g_mixer.channels.size()) && g_mixer.channels[static_cast<size_t>(channel)].chunk)
        {
            ++channel;
        }
    }
    if (!channel_in_range(channel))
    {
        unlock_audio();
        return -1;
    }
    Channel &item = g_mixer.channels[static_cast<size_t>(channel)];
    item.chunk = chunk;
    item.position = 0;
    item.loops_remaining = loops;
    item.fade_total = 0;
    item.fade_remaining = 0;
    unlock_audio();
    return channel;
}

int Mix_HaltChannel(int channel)
{
    lock_audio();
    int stopped = 0;
    if (channel == -1)
    {
        for (Channel &item : g_mixer.channels)
        {
            if (item.chunk)
            {
                stop_channel(item);
                ++stopped;
            }
        }
    }
    else if (channel_in_range(channel) && g_mixer.channels[static_cast<size_t>(channel)].chunk)
    {
        stop_channel(g_mixer.channels[static_cast<size_t>(channel)]);
        stopped = 1;
    }
    unlock_audio();
    return stopped;
}

int Mix_FadeOutChannel(int channel, int milliseconds)
{
    lock_audio();
    int fading = 0;
    const Uint64 fade_frames = static_cast<Uint64>(std::max(0, milliseconds)) * static_cast<Uint64>(g_mixer.frequency) / 1000;
    for (size_t i = 0; i < g_mixer.channels.size(); ++i)
    {
        if (channel != -1 && static_cast<int>(i) != channel)
        {
            continue;
        }
        Channel &item = g_mixer.channels[i];
        if (!item.chunk)
        {
            continue;
        }
        if (fade_frames == 0)
        {
            stop_channel(item);
        }
        else
        {
            item.fade_total = fade_frames;
            item.fade_remaining = fade_frames;
        }
        ++fading;
    }
    unlock_audio();
    return fading;
}

int Mix_Playing(int channel)
{
    if (!channel_in_range(channel))
    {
        return 0;
    }
    lock_audio();
    const int playing = g_mixer.channels[static_cast<size_t>(channel)].chunk != nullptr;
    unlock_audio();
    return playing;
}

const char *Mix_GetError()
{
    return SDL_GetError();
}

#endif // BEDLAM_USE_SDL_AUDIO_MIXER
