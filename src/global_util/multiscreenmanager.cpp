#include "multiscreenmanager.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QDebug>
#include <QDesktopWidget>
#include <QHash>
#include <QRect>

#include <com_deepin_daemon_display.h>

#include "src/session-widgets/sessionbasemodel.h"

using DisplayInter=com::deepin::daemon::Display;

MultiScreenManager::MultiScreenManager(QObject *parent)
    : QObject(parent)
    , m_raiseContentFrameTimer(new QTimer(this))
    , m_displayInter("com.deepin.daemon.Display", "/com/deepin/daemon/Display", QDBusConnection::sessionBus(), this)
    , m_waitQscreenflag(false)
{
    connect(&m_displayInter, &DisplayInter::MonitorsChanged, this, &MultiScreenManager::onMonitorsChanged);

    connect(qApp, &QApplication::screenAdded, this, &MultiScreenManager::ScreenAdd);
    // 在sw平台存在复制模式显示问题，使用延迟来置顶一个Frame
    m_raiseContentFrameTimer->setInterval(80);
    m_raiseContentFrameTimer->setSingleShot(true);

    connect(m_raiseContentFrameTimer, &QTimer::timeout, this, &MultiScreenManager::raiseContentFrame);
}

void MultiScreenManager::startRaiseContentFrame()
{
    m_raiseContentFrameTimer->start();
}

void MultiScreenManager::register_for_mutil_monitor(std::function<QWidget* (Monitor *)> function)
{
    QList<QDBusObjectPath> monitor = m_displayInter.monitors();
    m_registerMonitorFun = function;
    initMonitors(monitor);
}


inline uint qHash(const QRect& rect)
{
    QString s = QString("%1:%2:%3:%4").arg(rect.x()).arg(rect.y()).arg(rect.right()).arg(rect.bottom());
    return qHash(s);
}

inline bool screenGeometryValid(const QRect &rect)
{
    //防止坐标为4个0,此时QRect.isValid返回true
    return rect.width() > 1 && rect.height() > 1;
}

void MultiScreenManager::setFrameVisible() {
    raiseContentFrame();
}

void MultiScreenManager::ScreenAdd()
{
    qDebug() << "screen add " << m_waitQscreenflag;
    if (m_monPathList.isEmpty()) {
        m_waitQscreenflag = true;
    } else {
        for (auto path : m_monPathList) {
            Monitor *monitor = monitorAdded(path);
            m_frameMoniter[monitor] = m_registerMonitorFun(monitor);
        }
        m_monPathList.clear();
        m_waitQscreenflag = false;
        startRaiseContentFrame();
    }

}

void MultiScreenManager::raiseContentFrame()
{
    if (m_model == nullptr || !m_model->isShow()) {
        return;
    }
    qDebug() << "MultiScreenManager::raiseContentFrame, monitor num=" << m_frameMoniter.size() << m_frameMoniter;

    //统计screen的信息
    QHash<QRect, Monitor*> rect2Screen;
    QHash<QRect, int> rect2Count;
    Monitor *contentVisibleScreen = nullptr;
    for (auto it = m_frameMoniter.constBegin(); it != m_frameMoniter.constEnd(); ++it) {
        Monitor *monitor = it.key();
        const auto& geometry = monitor->rect();
        rect2Screen.insertMulti(geometry, monitor);
        rect2Count[geometry]++; //这里key不存在时，会静默插入key且value为0
        if (it.value()->property("contentVisible").toBool()) {
            if (screenGeometryValid(geometry) && monitor->enable()) {
                contentVisibleScreen = monitor;
            }
        }
    }

    //挑选一个内容可见的screen，优先主屏幕
    //优先cursor所在的frame的逻辑更好，但是wl中取不到cursor位置
    while (contentVisibleScreen == nullptr) {
        for (auto it = m_frameMoniter.constBegin(); it != m_frameMoniter.constEnd(); ++it) {
            Monitor *itMonitor = it.key();
            if (!itMonitor->isPrimary()) {
                continue;
            }
            if (screenGeometryValid(itMonitor->rect()) && itMonitor->enable()) {
                contentVisibleScreen = itMonitor;
            }
            break;
        }
        if (contentVisibleScreen != nullptr) {
            qDebug() << "MultiScreenManager::raiseContentFrame, primaryMonitor content visible";
            break;
        }
        for (auto it = m_frameMoniter.constBegin(); it != m_frameMoniter.constEnd(); ++it) {
            Monitor *monitor = it.key();
            const auto& geometry = monitor->rect();
            if (screenGeometryValid(geometry) && monitor->enable()) {
                contentVisibleScreen = monitor;
                break;
            }
        }
        break;
    }

    if (contentVisibleScreen == nullptr) {
        qWarning() << "MultiScreenManager::raiseContentFrame, contentVisibleScreen is null";
        return ;
    }

    //坐标不重复的显示，重复的只留一个显示，在所有显示的screen中必须有一个content可见
    for (auto it = m_frameMoniter.constBegin(); it != m_frameMoniter.constEnd(); ++it) {
        Monitor *monitor = it.key();
        const auto& geometry = monitor->rect();
        if (!monitor->enable()) {
            it.value()->hide();
            continue;
        }
        if (!screenGeometryValid(geometry)) {
            it.value()->hide();
            continue;
        }

        if (rect2Count[geometry] == 1) {
            it.value()->show();
            continue;
        }

        //复制模式, 如果conttent visible命中这里，就留visible这个显示，否则就留第一个显示。
        if (contentVisibleScreen->rect() == geometry) {
            if (it.key() == contentVisibleScreen) {
                it.value()->show();
            } else {
                it.value()->hide();
            }
        } else {
            //只显示第一个
            bool bShow = true;
            QHash<QRect, Monitor*>::iterator itFind = rect2Screen.find(geometry);
            while (itFind != rect2Screen.end() && itFind.key() == geometry) {
                Monitor* monSavePos = itFind.value();
                if (!monSavePos->enable() || !bShow) {
                    m_frameMoniter[itFind.value()]->hide();
                    ++itFind;
                    continue;
                }
                if (bShow) {
                    bShow = false;
                    m_frameMoniter[itFind.value()]->show();
                }
                ++itFind;
            }
            rect2Screen.remove(geometry);
        }
    }
}

void MultiScreenManager::onMonitorsChanged(const QList<QDBusObjectPath> & mons)
{
    qDebug() << "MultiScreenManager::onMonitorsChanged size:" << mons.size();

    QList<QString> ops;
    for (const auto *mon : m_frameMoniter.keys())
        ops << mon->path();

    QList<QString> pathList;
    QList<Monitor *> monitors;
    for (const auto op : mons) {
        const QString path = op.path();
        pathList << path;
        qDebug() << path;
        if (!ops.contains(path)) {
            if (m_waitQscreenflag) {
                Monitor *monitor = monitorAdded(path);
                monitors << monitor;
                m_waitQscreenflag = false;
            } else {
                m_monPathList.push_back(path);
            }
        }
    }

    // 主屏最后new，保证多屏复制模式下显示在最上面，wayland环境下，widget->raise()函数不生效
    for (Monitor *mon : monitors) {
        m_frameMoniter[mon] = m_registerMonitorFun(mon);
    }

    for (const auto op : ops)
        if (!pathList.contains(op)) {
            monitorRemoved(op);
        }

    startRaiseContentFrame();
}

Monitor *MultiScreenManager::monitorAdded(const QString &path)
{
    MonitorInter *inter = new MonitorInter("com.deepin.daemon.Display", path, QDBusConnection::sessionBus(), this);
    Monitor *mon = new Monitor(this);
    connect(inter, &MonitorInter::XChanged, mon, &Monitor::setX);
    connect(inter, &MonitorInter::YChanged, mon, &Monitor::setY);
    connect(inter, &MonitorInter::WidthChanged, mon, &Monitor::setW);
    connect(inter, &MonitorInter::HeightChanged, mon, &Monitor::setH);
    connect(inter, &MonitorInter::EnabledChanged, mon, &Monitor::setMonitorEnable);
    connect(inter, &MonitorInter::ModesChanged, mon, &Monitor::setMonitorModes);
    // NOTE: DO NOT using async dbus call. because we need to have a unique name to distinguish each monitor
    Q_ASSERT(inter->isValid());
    mon->setName(inter->name());
    mon->setMonitorEnable(inter->enabled());
    mon->setPath(path);
    mon->setX(inter->x());
    mon->setY(inter->y());
    mon->setW(inter->width());
    mon->setH(inter->height());
    mon->setMonitorModes(inter->modes());
    mon->setPrimary(m_displayInter.primary());

    qDebug() << "MultiScreenManager::monitorAdded:" << mon << mon->name() << mon->rect() << mon->enable();

    return mon;
}

void MultiScreenManager::monitorRemoved(const QString &path)
{
    Monitor *monitor = nullptr;
    for (auto it(m_frameMoniter.cbegin()); it != m_frameMoniter.cend(); ++it) {
        if (it.key()->path() == path) {
            monitor = it.key();
            break;
        }
    }
    if (!monitor)
        return;

    qDebug() << "MultiScreenManager::monitorRemoved:" << monitor << monitor->name() << monitor->rect() << monitor->enable();

    m_frameMoniter[monitor]->deleteLater();
    m_frameMoniter.remove(monitor);
    monitor->deleteLater();
}

void MultiScreenManager::initMonitors(const QList<QDBusObjectPath> &mons)
{
    for (const auto op : mons) {
        const QString path = op.path();
        Monitor *monitor = monitorAdded(path);
        m_frameMoniter[monitor] = m_registerMonitorFun(monitor);
    }
    startRaiseContentFrame();
}
