/*
Consider this code public domain.

This is research into a new resampler for mini_al. Not yet complete.

Requirements:
- Selection of different algorithms. The following at a minimum:
  - Linear with optional filtering
  - Sinc
- Floating point pipeline for f32 and fixed point integer pipeline for s16
  - Specify a mal_format enum as a config at initialization time, but fail if it's anything other than f32 or s16
- Need ability to move time forward without processing any samples
  - Needs an option to handle the cache as if silent samples of 0 have been passed as input
  - Needs option to move time forward by output sample rate _or_ input sample rate
- Need to be able to do the equivalent to a seek by passing in NULL to the read API()
  - mal_resampler_read(pResampler, frameCount, NULL) = mal_resampler_seek(pResampler, frameCount, 0)
- Need to be able to query the number of output PCM frames that can be generated from the currently cached input. The
  returned value must be fractional. Likewise, must be able to query the number of cached input PCM frames and must
  also be fractional.
- Need to be able to query exactly how many output PCM frames the user would get if they requested a certain number
  input frames. Likewise, need to be able to query how many input PCM frames are required for a certain number of
  output frames.
- Must support dynamic changing of the sample rate, both by input/output rate and by ratio
  - Each read and seek function for each algorithm must handle a ratio of 1 in a fast path
- Must have different modes on how to handle the last of the input samples. Certain situations (streaming) requires
  the last input samples to be cached in the internal structure for the windowing algorithm. Other situations require
  all of the input samples to be consumed in order to output the correct total sample count.
- Pointers passed into the onRead() callback must be guaranteed to be aligned to MAL_SIMD_ALIGNMENT.


Other Notes:
- I've had a bug in the past where a single call to read() returns too many samples. It essentially computes more
  samples than the input data would allow. The input data would get consumed, but output samples would continue to
  get computed up to the requested frame count, filling in the end with zeroes. This is completely wrong because
  the return value needs to be used to know whether or not the end of the input has been reached.


Random Notes:
- You cannot change the algorithm after initialization.
- It is recommended to keep the mal_resampler object aligned to MAL_SIMD_ALIGNMENT, though it is not necessary.
- Ratios need to be in the range of MAL_RESAMPLER_MIN_RATIO and MAL_RESAMPLER_MAX_RATIO. If you need extreme ratios
  then you will need to chain resamplers together.
*/
#ifndef mal_resampler_h
#define mal_resampler_h

#define MAL_RESAMPLER_SEEK_NO_CLIENT_READ   (1 << 0)    /* When set, does not read anything from the client when seeking. This does _not_ call onRead(). */
#define MAL_RESAMPLER_SEEK_INPUT_RATE       (1 << 1)    /* When set, treats the specified frame count based on the input sample rate rather than the output sample rate. */

#define MAL_RESAMPLER_CACHE_SIZE_IN_BYTES   4096

typedef struct mal_resampler mal_resampler;

/* Client callbacks. */
typedef mal_uint32 (* mal_resampler_read_from_client_proc)(mal_resampler* pResampler, mal_uint32 frameCount, void** ppFrames);

/* Backend functions. */
typedef mal_result (* mal_resampler_init_proc)(mal_resampler* pResampler);
typedef mal_uint64 (* mal_resampler_read_proc)(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames);
typedef mal_uint64 (* mal_resampler_seek_proc)(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options);

typedef enum
{
    mal_resampler_algorithm_sinc = 0,   /* Default. */
    mal_resampler_algorithm_linear,     /* Fastest. */
} mal_resampler_algorithm;

typedef enum
{
    mal_resampler_end_of_input_mode_consume = 0,    /* When the end of the input stream is reached, consume the last input PCM frames (do not leave them in the internal cache). Default. */
    mal_resampler_end_of_input_mode_no_consume      /* When the end of the input stream is reached, do _not_ consume the last input PCM frames (leave them in the internal cache). Use this in streaming situations. */
} mal_resampler_end_of_input_mode;

typedef struct
{
    mal_format format;
    mal_uint32 channels;
    mal_uint32 sampleRateIn;
    mal_uint32 sampleRateOut;
    double ratio;    /* ratio = in/out */
    mal_resampler_algorithm algorithm;
    mal_resampler_end_of_input_mode endOfInputMode;
    mal_resampler_read_from_client_proc onRead;
    void* pUserData;
} mal_resampler_config;

struct mal_resampler
{
    union
    {
        float     f32[MAL_RESAMPLER_CACHE_SIZE_IN_BYTES/sizeof(float)];
        mal_int16 s16[MAL_RESAMPLER_CACHE_SIZE_IN_BYTES/sizeof(mal_int16)];
    } cache;   /* Do not use directly. Keep this as the first member of this structure for SIMD alignment purposes. */
    mal_uint16 firstCachedFrameOffset;
    mal_uint16 cacheLengthInFrames; /* The number of valid frames sitting in the cache. May be less than the cache's capacity. */
    mal_uint16 windowLength;
    double windowTime;  /* By input rate. Relative to the start of the cache. */
    mal_resampler_config config;
    mal_resampler_init_proc init;
    mal_resampler_read_proc read;
    mal_resampler_seek_proc seek;
};

/*
Initializes a new resampler object from a config.
*/
mal_result mal_resampler_init(const mal_resampler_config* pConfig, mal_resampler* pResampler);

/*
Uninitializes the given resampler.
*/
void mal_resampler_uninit(mal_resampler* pResampler);

/*
Dynamically adjusts the sample rate.
*/
mal_result mal_resampler_set_rate(mal_resampler* pResampler, mal_uint32 sampleRateIn, mal_uint32 sampleRateOut);

/*
Dynamically adjusts the sample rate by a ratio.
*/
mal_result mal_resampler_set_rate_ratio(mal_resampler* pResampler, double ratio);

/*
Reads a number of PCM frames from the resampler.

Passing in NULL for ppFrames is equivalent to calling mal_resampler_seek(pResampler, frameCount, 0).
*/
mal_uint64 mal_resampler_read(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames);

/*
Seeks forward by the specified number of PCM frames.

"options" can be a cobination of the following:
    MAL_RESAMPLER_SEEK_NO_CLIENT_READ
        Reads in silence instead of reading in data from the onRead callback.
    MAL_RESAMPLER_SEEK_INPUT_RATE
        Treats "frameCount" as input samples instead of output samples.
*/
mal_uint64 mal_resampler_seek(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options);

/*
Retrieves the number of cached input frames.

This is equivalent to: (mal_uint64)ceil(mal_resampler_get_cached_input_time(pResampler));
*/
mal_uint64 mal_resampler_get_cached_input_frame_count(mal_resampler* pResampler);

/*
Retrieves the number of whole output frames that can be calculated from the currently cached input frames.

This is equivalent to: (mal_uint64)floor(mal_resampler_get_cached_output_time(pResampler));
*/
mal_uint64 mal_resampler_get_cached_output_frame_count(mal_resampler* pResampler);

/*
The same as mal_resampler_get_cached_input_frame_count(), except returns a fractional value representing the exact amount
of time in input rate making up the cached input.

When the end of input mode is set to mal_resampler_end_of_input_mode_no_consume, the input frames currently sitting in the
window are not included in the calculation.
*/
double mal_resampler_get_cached_input_time(mal_resampler* pResampler);

/*
The same as mal_resampler_get_cached_output_frame_count(), except returns a fractional value representing the exact amount
of time in output rate making up the cached output.

When the end of input mode is set to mal_resampler_end_of_input_mode_no_consume, the input frames currently sitting in the
window are not included in the calculation.
*/
double mal_resampler_get_cached_output_time(mal_resampler* pResampler);

/*
Calculates the number of whole input frames that would need to be read from the client in order to output the specified
number of output frames.

The returned value does not include cached input frames. It only returns the number of extra frames that would need to be
read from the client in order to output the specified number of output frames.

When the end of input mode is set to mal_resampler_end_of_input_mode_no_consume, the input frames sitting in the filter
window are not included in the calculation.
*/
mal_uint64 mal_resampler_get_required_input_frame_count(mal_resampler* pResampler, mal_uint64 outputFrameCount);

/*
Calculates the number of whole output frames that would be output after fully reading and consuming the specified number of
input frames from the client.

A detail to keep in mind is how cached input frames are handled. This function calculates the output frame count based on
inputFrameCount + mal_resampler_get_cached_input_time(). It essentially calcualtes how many output frames will be returned
if an additional inputFrameCount frames were read from the client and consumed by the resampler. You can adjust the return
value by mal_resampler_get_cached_output_frame_count() which calculates the number of output frames that can be output from
the currently cached input.

When the end of input mode is set to mal_resampler_end_of_input_mode_no_consume, the input frames sitting in the filter
window are not included in the calculation.
*/
mal_uint64 mal_resampler_get_expected_output_frame_count(mal_resampler* pResampler, mal_uint64 inputFrameCount);
#endif

#ifdef MINI_AL_IMPLEMENTATION

#ifndef MAL_RESAMPLER_MIN_RATIO
#define MAL_RESAMPLER_MIN_RATIO 0.001
#endif
#ifndef MAL_RESAMPLER_MAX_RATIO
#define MAL_RESAMPLER_MAX_RATIO 100.0
#endif

mal_uint64 mal_resampler_read__linear(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames);
mal_uint64 mal_resampler_seek__linear(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options);

mal_result mal_resampler_init__sinc(mal_resampler* pResampler);
mal_uint64 mal_resampler_read__sinc(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames);
mal_uint64 mal_resampler_seek__sinc(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options);

/* TODO: Add this to mini_al.h */
#define MAL_ALIGN_INT(val, alignment) (((val) + ((alignment)-1)) & ~((alignment)-1))
#define MAL_ALIGN_PTR(ptr, alignment) (void*)MAL_ALIGN_INT(((mal_uintptr)(ptr)), (alignment))

/*
This macro declares a set of variables on the stack of a given size in bytes. The variables it creates are:
  - mal_uint8 <name>Unaligned[size + MAL_SIMD_ALIGNMENT];
  - <type>* <name>[MAL_MAX_CHANNELS];
  - size_t <name>FrameCount;    <-- This is the number of samples contained within each sub-buffer of <name>

This does not work for formats that do not have a clean mapping to a primitive C type. s24 will not work here.
*/
#define MAL_DECLARE_ALIGNED_STACK_BUFFER(type, name, size, channels) \
    mal_uint8 name##Unaligned[(size) + MAL_SIMD_ALIGNMENT]; \
    type* name[MAL_MAX_CHANNELS]; \
    size_t name##FrameCount = ((size) & ~((MAL_SIMD_ALIGNMENT)-1)) / sizeof(type); \
    do { \
        mal_uint32 iChannel; \
        for (iChannel = 0; iChannel < channels; ++iChannel) { \
            name[iChannel] = (type*)((mal_uint8*)MAL_ALIGN_PTR(name##Unaligned, MAL_SIMD_ALIGNMENT) + (iChannel*((size) & ~((MAL_SIMD_ALIGNMENT)-1)))); \
        } \
    } while (0)
    

mal_result mal_resampler_init(const mal_resampler_config* pConfig, mal_resampler* pResampler)
{
    if (pResampler == NULL) {
        return MAL_INVALID_ARGS;
    }
    mal_zero_object(pResampler);

    if (pConfig == NULL) {
        return MAL_INVALID_ARGS;
    }

    pResampler->config = *pConfig;
    if (pResampler->config.format != mal_format_f32 && pResampler->config.format != mal_format_s16) {
        return MAL_INVALID_ARGS;        /* Unsupported format. */
    }
    if (pResampler->config.channels == 0) {
        return MAL_INVALID_ARGS;        /* Unsupported channel count. */
    }
    if (pResampler->config.ratio == 0) {
        if (pResampler->config.sampleRateIn == 0 || pResampler->config.sampleRateOut == 0) {
            return MAL_INVALID_ARGS;    /* Unsupported sample rate. */
        }
        pResampler->config.ratio = (double)pResampler->config.sampleRateIn / (double)pResampler->config.sampleRateOut;
    }
    if (pResampler->config.onRead == NULL) {
        return MAL_INVALID_ARGS;        /* No input callback specified. */
    }

    switch (pResampler->config.algorithm) {
        case mal_resampler_algorithm_linear:
        {
            pResampler->init = NULL;
            pResampler->read = mal_resampler_read__linear;
            pResampler->seek = mal_resampler_seek__linear;
        } break;

        case mal_resampler_algorithm_sinc:
        {
            pResampler->init = mal_resampler_init__sinc;
            pResampler->read = mal_resampler_read__sinc;
            pResampler->seek = mal_resampler_seek__sinc;
        } break;
    }

    if (pResampler->init != NULL) {
        mal_result result = pResampler->init(pResampler);
        if (result != MAL_SUCCESS) {
            return result;
        }
    }

    return MAL_SUCCESS;
}

void mal_resampler_uninit(mal_resampler* pResampler)
{
    (void)pResampler;
}

mal_result mal_resampler_set_rate(mal_resampler* pResampler, mal_uint32 sampleRateIn, mal_uint32 sampleRateOut)
{
    if (pResampler == NULL) {
        return MAL_INVALID_ARGS;
    }

    if (sampleRateIn == 0 || sampleRateOut == 0) {
        return MAL_INVALID_ARGS;
    }

    double ratio = (double)pResampler->config.sampleRateIn / (double)pResampler->config.sampleRateOut;
    if (ratio < MAL_RESAMPLER_MIN_RATIO || ratio > MAL_RESAMPLER_MAX_RATIO) {
        return MAL_INVALID_ARGS;    /* Ratio is too extreme. */
    }

    pResampler->config.sampleRateIn  = sampleRateIn;
    pResampler->config.sampleRateOut = sampleRateOut;
    pResampler->config.ratio         = ratio;

    return MAL_SUCCESS;
}

mal_result mal_resampler_set_rate_ratio(mal_resampler* pResampler, double ratio)
{
    if (pResampler == NULL) {
        return MAL_INVALID_ARGS;
    }

    if (ratio < MAL_RESAMPLER_MIN_RATIO || ratio > MAL_RESAMPLER_MAX_RATIO) {
        return MAL_INVALID_ARGS;    /* Ratio is too extreme. */
    }

    pResampler->config.ratio = ratio;

    return MAL_SUCCESS;
}

mal_uint64 mal_resampler_read(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames)
{
    if (pResampler == NULL || pResampler->read == NULL) {
        return 0;   /* Invalid arguments. */
    }

    if (frameCount == 0) {
        return 0;   /* Nothing to do, so return early. */
    }

    /* When ppFrames is NULL, reading is equivalent to seeking with default options. */
    if (ppFrames == NULL) {
        return mal_resampler_seek(pResampler, frameCount, 0);
    }


    return pResampler->read(pResampler, frameCount, ppFrames);
}

mal_uint64 mal_resampler_seek(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options)
{
    if (pResampler == NULL || pResampler->seek == NULL) {
        return 0;
    }

    if (frameCount == 0) {
        return 0;   /* Nothing to do, so return early. */
    }


    return pResampler->seek(pResampler, frameCount, options);
}


mal_uint64 mal_resampler_get_cached_input_frame_count(mal_resampler* pResampler)
{
    return (mal_uint64)ceil(mal_resampler_get_cached_input_time(pResampler));
}

mal_uint64 mal_resampler_get_cached_output_frame_count(mal_resampler* pResampler)
{
    return (mal_uint64)floor(mal_resampler_get_cached_output_time(pResampler));
}

double mal_resampler__calculate_cached_input_time(mal_resampler* pResampler)
{
    /*
    The cached input time depends on whether or not the end of the input is being consumed. If so, it's the difference between the
    last cached frame and the halfway point of the window, rounded down. Otherwise it's between the last cached frame and the end
    of the window.
    */
    double cachedInputTime = pResampler->cacheLengthInFrames;
    if (pResampler->config.endOfInputMode == mal_resampler_end_of_input_mode_consume) {
        cachedInputTime -= (pResampler->windowTime + (pResampler->windowLength >> 1));
    } else {
        cachedInputTime -= (pResampler->windowTime + pResampler->windowLength);
    } 

    return cachedInputTime;
}

double mal_resampler_get_cached_input_time(mal_resampler* pResampler)
{
    if (pResampler == NULL) {
        return 0;   /* Invalid args. */
    }

    return mal_resampler__calculate_cached_input_time(pResampler);
}

double mal_resampler__calculate_cached_output_time(mal_resampler* pResampler)
{
    return mal_resampler__calculate_cached_input_time(pResampler) / pResampler->config.ratio;
}

double mal_resampler_get_cached_output_time(mal_resampler* pResampler)
{
    if (pResampler == NULL) {
        return 0;   /* Invalid args. */
    }

    return mal_resampler__calculate_cached_output_time(pResampler);
}


mal_uint64 mal_resampler_get_required_input_frame_count(mal_resampler* pResampler, mal_uint64 outputFrameCount)
{
    if (pResampler == NULL) {
        return 0;   /* Invalid args. */
    }

    if (outputFrameCount == 0) {
        return 0;
    }

    /* First grab the amount of output time sitting in the cache. */
    double cachedOutputTime = mal_resampler__calculate_cached_output_time(pResampler);
    if (cachedOutputTime >= outputFrameCount) {
        return 0;   /* All of the necessary input data is cached. No additional data is required from the client. */
    }

    /*
    Getting here means more input data will be required. A detail to consider here is that we are accepting an unsigned 64-bit integer
    for the output frame count, however we need to consider sub-frame timing which we're doing by using a double. There will not be
    enough precision in the double to represent the whole 64-bit range of the input variable. For now I'm not handling this explicitly
    because I think it's unlikely outputFrameCount will be set to something so huge anyway, but it will be something to think about in
    order to get this working properly for the whole 64-bit range.

    The return value must always be larger than 0 after this point. If it's not we have an error.
    */
    double nonCachedOutputTime = outputFrameCount - cachedOutputTime;
    mal_assert(nonCachedOutputTime > 0);

    mal_uint64 requiredInputFrames = (mal_uint64)ceil(nonCachedOutputTime * pResampler->config.ratio);
    mal_assert(requiredInputFrames > 0);

    return requiredInputFrames;
}

mal_uint64 mal_resampler_get_expected_output_frame_count(mal_resampler* pResampler, mal_uint64 inputFrameCount)
{
    if (pResampler == NULL) {
        return 0; /* Invalid args. */
    }

    if (inputFrameCount == 0) {
        return 0;
    }

    /* What we're actually calculating here is how many whole output frames will be calculated after consuming inputFrameCount + mal_resampler_get_cached_input_time(). */
    return (mal_uint64)floor((mal_resampler__calculate_cached_input_time(pResampler) + inputFrameCount) / pResampler->config.ratio);
}


/*
Linear
*/
mal_uint64 mal_resampler_read__linear(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames)
{
    mal_assert(pResampler != NULL);
    mal_assert(pResampler->config.onRead != NULL);
    mal_assert(frameCount > 0);
    mal_assert(ppFrames != NULL);

    /* TODO: Implement me. */
    (void)pResampler;
    (void)frameCount;
    (void)ppFrames;
    return 0;
}

mal_uint64 mal_resampler_seek__linear(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options)
{
    mal_assert(pResampler != NULL);
    mal_assert(pResampler->config.onRead != NULL);
    mal_assert(frameCount > 0);

    /* TODO: Implement me. */
    (void)pResampler;
    (void)frameCount;
    (void)options;
    return 0;
}


/*
Sinc
*/
mal_result mal_resampler_init__sinc(mal_resampler* pResampler)
{
    mal_assert(pResampler != NULL);

    /* TODO: Implement me. Need to initialize the sinc table. */
    return MAL_SUCCESS;
}

mal_uint64 mal_resampler_read__sinc(mal_resampler* pResampler, mal_uint64 frameCount, void** ppFrames)
{
    mal_assert(pResampler != NULL);
    mal_assert(pResampler->config.onRead != NULL);
    mal_assert(frameCount > 0);
    mal_assert(ppFrames != NULL);

    /* TODO: Implement me. */
    (void)pResampler;
    (void)frameCount;
    (void)ppFrames;
    return 0;
}

mal_uint64 mal_resampler_seek__sinc(mal_resampler* pResampler, mal_uint64 frameCount, mal_uint32 options)
{
    mal_assert(pResampler != NULL);
    mal_assert(pResampler->config.onRead != NULL);
    mal_assert(frameCount > 0);

    /* TODO: Implement me. */
    (void)pResampler;
    (void)frameCount;
    (void)options;
    return 0;
}

#endif
