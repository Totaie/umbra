#pragma once

#include "nvhttp.h"
#include "nvaddress.h"

#include <QThread>
#include <QReadWriteLock>
#include <QSettings>
#include <QRunnable>

class CopySafeReadWriteLock : public QReadWriteLock
{
public:
    CopySafeReadWriteLock() = default;

    // Don't actually copy the QReadWriteLock
    CopySafeReadWriteLock(const CopySafeReadWriteLock&) : QReadWriteLock() {}
    CopySafeReadWriteLock& operator=(const CopySafeReadWriteLock &) { return *this; }
};

class NvComputer
{
    friend class PcMonitorThread;
    friend class ComputerManager;
    friend class PendingQuitTask;

private:
    void sortAppList();

    bool updateAppList(QVector<NvApp> newAppList);

    bool pendingQuit;

public:
    NvComputer() = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer(const NvComputer&) = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer& operator=(const NvComputer &) = default;

    explicit NvComputer(NvHTTP& http, QString serverInfo);

    explicit NvComputer(QSettings& settings);

    void
    setRemoteAddress(QHostAddress);

    bool
    update(const NvComputer& that);

    bool
    wake() const;

    enum ReachabilityType
    {
        RI_UNKNOWN,
        RI_LAN,
        RI_VPN,
    };

    ReachabilityType
    getActiveAddressReachability() const;

    QVector<NvAddress>
    uniqueAddresses() const;

    void
    serialize(QSettings& settings, bool serializeApps) const;

    // Caller is responsible for synchronizing read access to both hosts
    bool
    isEqualSerialized(const NvComputer& that) const;

    enum PairState
    {
        PS_UNKNOWN,
        PS_PAIRED,
        PS_NOT_PAIRED
    };

    enum ComputerState
    {
        CS_UNKNOWN,
        CS_ONLINE,
        CS_OFFLINE
    };

    // Ephemeral traits
    ComputerState state;
    PairState pairState;
    NvAddress activeAddress;
    uint16_t activeHttpsPort;
    int currentGameId;
    QString gfeVersion;
    QString appVersion;
    // Product version of an Umbra host, empty for any other host software. The
    // GameStream appVersion above is identical across the whole Sunshine lineage, so
    // this is the only way to tell what is actually answering.
    QString umbraHostVersion;
    QVector<NvDisplayMode> displayModes;
    int maxLumaPixelsHEVC;
    int serverCodecModeSupport;
    QString gpuModel;
    bool isSupportedServerVersion;

    // Persisted traits
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress ipv6Address;
    NvAddress manualAddress;
    QByteArray macAddress;
    QString name;
    bool hasCustomName;
    QString uuid;
    QSslCertificate serverCert;
    QVector<NvApp> appList;
    bool isNvidiaServerSoftware;

    // Optional API token for the host's web interface, used to submit the pairing PIN
    // on the user's behalf so they don't have to type it on the host. Generated in the
    // host's web UI (Apollo/Vibepollo/Sunshine) and scoped to POST /api/pin.
    QString apiToken;

    // Persistent pairing passphrase configured on this host. When set, Umbra pairs
    // with it directly and nobody has to enter a PIN on the host.
    QString pairingPassphrase;
    // Remember to update isEqualSerialized() when adding fields here!

    // Synchronization
    mutable CopySafeReadWriteLock lock;

private:
    uint16_t externalPort;
};
