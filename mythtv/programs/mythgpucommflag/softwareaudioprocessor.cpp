#include <math.h>

#include "mythlogging.h"
#include "resultslist.h"
#include "audioprocessor.h"

// Prototypes
FlagResults *SoftwareVolumeLevel(OpenCLDevice *dev, int16_t *samples, int size,
                                 int count, int64_t pts, int rate);

AudioProcessorList *softwareAudioProcessorList;

AudioProcessorInit softwareAudioProcessorInit[] = {
    { "Volume Level", SoftwareVolumeLevel },
    { "", NULL }
};

void InitSoftwareAudioProcessors(void)
{
    softwareAudioProcessorList =
        new AudioProcessorList(softwareAudioProcessorInit);
}

#define MAX_ACCUM (((uint64_t)(~1)) - (0xFFFFLL * 0xFFFFLL))
FlagResults *SoftwareVolumeLevel(OpenCLDevice *dev, int16_t *samples, int size,
                                 int count, int64_t pts, int rate)
{
    static uint64_t accumSRMS = 0;
    static int accumSRMSShift = 0;
    static uint64_t accumSample = 0;

    uint64_t accum = 0;
    int accumShift = 0;
    int channels = size / count / sizeof(int16_t);
    int sampleCount = count * channels;

#if 0
    LOG(VB_GENERAL, LOG_INFO, "Software Volume Level");
#endif

    // Accumulate this frame's partial squared RMS
    for (int i = 0; i < sampleCount; i++)
    {
        // Check accumulator for potential overflow (shouldn't happen)
        if (accum >= MAX_ACCUM)
        {
            accum >>= 2;
            accumShift += 2;
        }
        accum += (uint64_t)(samples[i]*samples[i]) >> accumShift;
    }

#if 0
    LOG(VB_GENERAL, LOG_DEBUG, 
        QString("accum: %1, accumShift: %2, sampleCount: %3")
        .arg(accum) .arg(accumShift) .arg(sampleCount));
#endif

    // Calculate RMS level of this window
    uint16_t windowRMS = sqrt((double)accum * pow(2.0, (double)accumShift) /
                              (double)sampleCount);
    if (!windowRMS)
        windowRMS = 1;

    float windowRMSdB = 20.0 * log10((double)windowRMS / 32767.0);

    // Check overall SRMS for potential overflow
    if (accumSRMS >= MAX_ACCUM)
    {
        accumSRMS >>= 2;
        accumSRMSShift += 2;
    }

    // Normalize the window and overall SRMS to have the same number of shifts
    if (accumShift > accumSRMSShift)
    {
        accumSRMS >>= (accumShift - accumSRMSShift);
        accumSRMSShift = accumShift;
    }
    else if (accumShift < accumSRMSShift)
    {
        accum >>= (accumSRMSShift - accumShift);
        accumShift = accumSRMSShift;
    }

    // Accumulate the overall SRMS
    accumSRMS += accum;
    accumSample += sampleCount;

#if 0
    LOG(VB_GENERAL, LOG_DEBUG,
        QString("accumSRMS: %1, accumSRMSShift: %2, accumSample: %3")
        .arg(accumSRMS) .arg(accumSRMSShift) .arg(accumSample));
#endif

    // Calculate RMS level of the recording so far
    uint16_t overallRMS = sqrt((double)accumSRMS *
                               pow(2.0, (double)accumSRMSShift) /
                               (double)accumSample);
    if (!overallRMS)
        overallRMS = 1;

    float overallRMSdB = 20.0 * log10(double(overallRMS) / 32767.0);
    float deltaRMSdB = windowRMSdB - overallRMSdB;

#if 0
    LOG(VB_GENERAL, LOG_INFO,
        QString("Window RMS: %1 (%2 dB), Overall RMS: %3 (%4 dB), Delta: %5 dB")
        .arg(windowRMS) .arg(windowRMSdB) .arg(overallRMS) .arg(overallRMSdB)
        .arg(deltaRMSdB));
#endif

    FlagFindings *finding = NULL;

    if (deltaRMSdB >= 6.0)
        finding = new FlagFindings(kFindingAudioHigh, true);
    else if (deltaRMSdB <= -12.0)
        finding = new FlagFindings(kFindingAudioLow, true);

    if (!finding)
        return NULL;

    FlagFindingsList *findings = new FlagFindingsList();
    findings->append(finding);
    FlagResults *results = new FlagResults(findings);

    return results;
}

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
