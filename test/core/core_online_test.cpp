/*
    Copyright © 2019 by The qTox Project Contributors

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "src/core/core.h"
#include "src/core/icoresettings.h"
#include "src/core/toxoptions.h"
#include "src/model/ibootstraplistgenerator.h"
#include "src/net/bootstrapnodeupdater.h"
#include "src/persistence/settings.h"

#include <QSignalSpy>
#include <QtGlobal>
#include <QtTest/QtTest>
#include <limits>

#include <QTest>
#include <iostream>
#include <memory>

Q_DECLARE_METATYPE(QList<DhtServer>)
Q_DECLARE_METATYPE(ToxPk)
Q_DECLARE_METATYPE(uint32_t)
Q_DECLARE_METATYPE(Status::Status)

class MockSettings : public QObject, public ICoreSettings
{
Q_OBJECT
public:
    MockSettings() {
        Q_INIT_RESOURCE(res);
        qRegisterMetaType<QList<DhtServer>>("QList<DhtServer>");
        qRegisterMetaType<ToxPk>("ToxPk");
        qRegisterMetaType<uint32_t>("uint32_t");
        qRegisterMetaType<Status::Status>("Status::Status");
    }

    bool getEnableIPv6() const override { return false; }
    void setEnableIPv6(bool) override { }

    bool getForceTCP() const override { return false; }
    void setForceTCP(bool) override { }

    bool getEnableLanDiscovery() const override { return false; }
    void setEnableLanDiscovery(bool) override { }

    QString getProxyAddr() const override { return Addr; }
    void setProxyAddr(const QString &Addr) override { this->Addr = Addr; }

    ProxyType getProxyType() const override { return type; }
    void setProxyType(ProxyType type) override { this->type = type; }

    quint16 getProxyPort() const override { return port; }
    void setProxyPort(quint16 port) override { this->port = port; }

    QNetworkProxy getProxy() const override { return QNetworkProxy(QNetworkProxy::ProxyType::NoProxy); }

    SIGNAL_IMPL(MockSettings, enableIPv6Changed, bool enabled)
    SIGNAL_IMPL(MockSettings, forceTCPChanged, bool enabled)
    SIGNAL_IMPL(MockSettings, enableLanDiscoveryChanged, bool enabled)
    SIGNAL_IMPL(MockSettings, proxyTypeChanged, ICoreSettings::ProxyType type)
    SIGNAL_IMPL(MockSettings, proxyAddressChanged, const QString& address)
    SIGNAL_IMPL(MockSettings, proxyPortChanged, quint16 port)

private:
    QList<DhtServer> dhtServerList;
    QString Addr;
    ProxyType type;
    quint16 port;
};

class MockNodeListGenerator : public IBootstrapListGenerator
{
    QList<DhtServer> getBootstrapnodes() const override;
};

QList<DhtServer> MockNodeListGenerator::getBootstrapnodes() const {
    return BootstrapNodeUpdater::loadDefaultBootstrapNodes();
}

namespace {
    const int network_timeout = 60000; // timeout for operations happening online
    const int local_timeout = 500; // timeout for operations happening offline
}

class TestCoreOnline : public QObject
{
Q_OBJECT
private slots:
    void init();
    void deinit();

    // actual test cases
    void change_name();
    void change_status_message();
    void change_status();

private:
    /* Test Variables */
    ToxCorePtr alice;
    ToxCorePtr bob;
};

void TestCoreOnline::init()
{
    auto settings = std::unique_ptr<MockSettings>(new MockSettings());

    MockNodeListGenerator nodesGenerator{};

    alice = Core::makeToxCore(QByteArray{}, *settings, nodesGenerator, nullptr);
    QVERIFY2(alice != nullptr, "alice initialization failed");
    bob = Core::makeToxCore(QByteArray{}, *settings, nodesGenerator, nullptr);
    QVERIFY2(bob != nullptr, "bob initialization failed");

    QSignalSpy spyAliceOnline(alice.get(), &Core::connected);
    QSignalSpy spyBobOnline(bob.get(), &Core::connected);

    alice->start();
    bob->start();

    // Wait for both instances coming online
    QTRY_VERIFY_WITH_TIMEOUT(spyAliceOnline.count() >= 1 && spyBobOnline.count() >= 1, network_timeout);

    // Make a friend request from alice to bob
    const QLatin1String friendMsg{"Test Invite Message"};

    QSignalSpy spyBobFriendMsg(bob.get(), &Core::friendRequestReceived);
    QSignalSpy spyAliceFriendMsg(alice.get(), &Core::requestSent);
    alice->requestFriendship(bob->getSelfId(), friendMsg);

    // Wait for both instances coming online
    QTRY_VERIFY_WITH_TIMEOUT(spyBobFriendMsg.count() == 1 && spyAliceFriendMsg.count() == 1, network_timeout);

    // Check for expected signal content
    QVERIFY(qvariant_cast<ToxPk>(spyBobFriendMsg[0][0]) == alice->getSelfPublicKey());
    QVERIFY(spyBobFriendMsg[0][1].toString() == friendMsg);

    QVERIFY(qvariant_cast<ToxPk>(spyAliceFriendMsg[0][0]) == bob->getSelfPublicKey());
    QVERIFY(spyAliceFriendMsg[0][1].toString() == friendMsg);

    // Let Bob accept the friend request from Alice
    bob->acceptFriendRequest(alice->getSelfPublicKey());

    // Wait until both see each other
    QSignalSpy spyAliceFriendOnline(alice.get(), &Core::friendStatusChanged);
    QSignalSpy spyBobFriendOnline(bob.get(), &Core::friendStatusChanged);

    // FIXME: Check if this is reliable even with CoreExt
    // Wait for both instances being online
    QTRY_VERIFY_WITH_TIMEOUT(spyAliceFriendOnline.count() >= 1 && spyBobFriendOnline.count() >= 1, network_timeout);

    // Check for expected signal content
    QVERIFY(spyAliceFriendOnline[0][0].toInt() == static_cast<int>(Status::Status::Online));
    QVERIFY(spyBobFriendOnline[0][0].toInt() == static_cast<int>(Status::Status::Online));
}

void TestCoreOnline::deinit()
{
    alice.reset();
    bob.reset();
}

void TestCoreOnline::change_name()
{
    // Change the name of Alice to "Alice"
    const QLatin1String aliceName{"Alice"};

    QSignalSpy aliceSaveRequest(alice.get(), &Core::saveRequest);
    QSignalSpy aliceUsernameChanged(alice.get(), &Core::usernameSet);
    QSignalSpy bobUsernameChangeReceived(bob.get(), &Core::friendUsernameChanged);

    alice->setUsername(aliceName);

    QTRY_VERIFY_WITH_TIMEOUT(aliceSaveRequest.count() == 1, local_timeout);
    QTRY_VERIFY_WITH_TIMEOUT(aliceUsernameChanged.count() == 1 &&
                             aliceUsernameChanged[0][0].toString() == aliceName,
                             local_timeout);

    QTRY_VERIFY_WITH_TIMEOUT(bobUsernameChangeReceived.count() == 1 &&
                             bobUsernameChangeReceived[0][1].toString() == aliceName, network_timeout);

    // Setting the username again to the same value shoud NOT trigger any signals
    alice->setUsername(aliceName);

    // Need to sleep here, because QTRY_VERIFY_WITH_TIMEOUT would immideatly trigger
    QTest::qSleep(1000);

    QVERIFY(aliceSaveRequest.count() == 1);
    QVERIFY(aliceUsernameChanged.count() == 1);
    QVERIFY(bobUsernameChangeReceived.count() == 1);
}

void TestCoreOnline::change_status_message()
{
    // Change the status message of Alice
    const QLatin1String aliceStatusMsg{"Testing a lot"};

    QSignalSpy aliceSaveRequest(alice.get(), &Core::saveRequest);
    QSignalSpy aliceStatusMsgChanged(alice.get(), &Core::usernameSet);
    QSignalSpy bobStatusMsgChangeReceived(bob.get(), &Core::friendUsernameChanged);

    alice->setStatusMessage(aliceStatusMsg);

    QTRY_VERIFY_WITH_TIMEOUT(aliceSaveRequest.count() == 1, local_timeout);
    QTRY_VERIFY_WITH_TIMEOUT(aliceStatusMsgChanged.count() == 1 &&
                             aliceStatusMsgChanged[0][0].toString() == aliceStatusMsg,
                             local_timeout);

    QTRY_VERIFY_WITH_TIMEOUT(bobStatusMsgChangeReceived.count() == 1 &&
                             bobStatusMsgChangeReceived[0][1].toString() == aliceStatusMsg, network_timeout);

    // Setting the status message again to the same value shoud NOT trigger any signals
    alice->setStatusMessage(aliceStatusMsg);

    // Need to sleep here, because QTRY_VERIFY_WITH_TIMEOUT would immideatly trigger
    QTest::qSleep(1000);

    QVERIFY(aliceSaveRequest.count() == 1);
    QVERIFY(aliceStatusMsgChanged.count() == 1);
    QVERIFY(bobStatusMsgChangeReceived.count() == 1);
}

void TestCoreOnline::change_status()
{
    QSignalSpy aliceSaveRequest(alice.get(), &Core::saveRequest);
    QSignalSpy aliceStatusChanged(alice.get(), &Core::statusSet);
    QSignalSpy bobStatusChangeReceived(bob.get(), &Core::friendStatusChanged);

    alice->setStatus(Status::Status::Away);

    QTRY_VERIFY_WITH_TIMEOUT(aliceSaveRequest.count() == 1, local_timeout);
    QTRY_VERIFY_WITH_TIMEOUT(aliceStatusChanged.count() == 1 &&
                             qvariant_cast<Status::Status>(aliceStatusChanged[0][0]) == Status::Status::Away,
                             local_timeout);

    QTRY_VERIFY_WITH_TIMEOUT(bobStatusChangeReceived.count() == 1 &&
                             qvariant_cast<Status::Status>(bobStatusChangeReceived[0][1]) == Status::Status::Away,
                             network_timeout);

    // Setting the status message again to the same value again triggers all signals
    alice->setStatus(Status::Status::Away);

    // TODO(sudden6): Emitting these again odd and should probably be changed, lets codify it for now though
    QTRY_VERIFY_WITH_TIMEOUT(aliceSaveRequest.count() == 2, local_timeout);
    QTRY_VERIFY_WITH_TIMEOUT(aliceStatusChanged.count() == 2 &&
                             qvariant_cast<Status::Status>(aliceStatusChanged[1][0]) == Status::Status::Away,
                             local_timeout);

    // Toxcore will filter these, even though we set it
    QTest::qSleep(1000);
    QVERIFY(bobStatusChangeReceived.count() == 1);
}

QTEST_GUILESS_MAIN(TestCoreOnline)
#include "core_online_test.moc"
