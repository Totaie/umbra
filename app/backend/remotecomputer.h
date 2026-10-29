#pragma once

class RemoteStreamConfig {
public:
    bool remoteResolution;
    int remoteResolutionWidth;
    int remoteResolutionHeight;
    bool remoteFps;
    int remoteFpsRate;
    int originalStreamWidth;
    int originalStreamHeight;
    float maxBrightness;
    float minBrightness;
    float maxAverageBrightness;
    // Umbra Host: the display this session shows whatever other sessions show, by its
    // index in /actions/displays; -1 leaves it to the host. See SpanScreens.
    int pinnedHostDisplay = -1;
    // ...and its name, which the host prefers when its list has changed since.
    QString pinnedHostDisplayName;

    RemoteStreamConfig(
        bool remoteResolution, int remoteResolutionWidth, int remoteResolutionHeight,
        bool remoteFps, int remoteFpsRate,
        int originalStreamWidth, int originalStreamHeight,
        float maxBrightness = 0, float minBrightness = 0, float maxAverageBrightness = 0)
    {
        this->remoteResolution = remoteResolution;
        this->remoteResolutionWidth = remoteResolutionWidth;
        this->remoteResolutionHeight = remoteResolutionHeight;
        this->remoteFps = remoteFps;
        this->remoteFpsRate = remoteFpsRate;
        this->originalStreamWidth = originalStreamWidth;
        this->originalStreamHeight = originalStreamHeight;
        this->maxBrightness = maxBrightness;
        this->minBrightness = minBrightness;
        this->maxAverageBrightness = maxAverageBrightness;
    }
};
