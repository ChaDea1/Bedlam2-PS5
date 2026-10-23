#pragma once

#include <SDL.h>

#include <cstdint>

#define MIX_MAX_VOLUME 128

struct Mix_Chunk
{
    int allocated;
    Uint8 *abuf;
    Uint32 alen;
    Uint8 volume;
};

#ifdef __cplusplus
extern "C" {
#endif

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunk_size);
void Mix_CloseAudio();
int Mix_AllocateChannels(int channels);
int Mix_ReserveChannels(int channels);
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *source, int free_source);
void Mix_FreeChunk(Mix_Chunk *chunk);
int Mix_Volume(int channel, int volume);
int Mix_VolumeChunk(Mix_Chunk *chunk, int volume);
int Mix_SetPanning(int channel, Uint8 left, Uint8 right);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_HaltChannel(int channel);
int Mix_FadeOutChannel(int channel, int milliseconds);
int Mix_Playing(int channel);
const char *Mix_GetError();

#ifdef __cplusplus
}
#endif
