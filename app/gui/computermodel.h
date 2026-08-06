#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    // Per-host token for the host's web API, used to submit the pairing PIN for the
    // user instead of making them type it on the host.
    Q_INVOKABLE QString getApiToken(int computerIndex);
    Q_INVOKABLE void setApiToken(int computerIndex, QString token);

    // Number of screens attached to this PC, used to size the multi-display launcher.
    Q_INVOKABLE int getClientScreenCount();

    // Streams the first screenCount host displays, one per screen on this PC.
    //
    // moonlight-common-c keeps its connection state in file-scope statics, so a
    // process can only ever hold one stream. Each display therefore gets its own
    // Umbra process, launched with --host-display/--client-screen.
    Q_INVOKABLE bool launchMultiDisplay(int computerIndex, int screenCount);

signals:
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
