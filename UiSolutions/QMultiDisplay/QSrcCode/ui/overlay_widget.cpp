#include "overlay_widget.h"
#include "ui_overlay_widget.h"

#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QPainter>
#include <QPushButton>
#include <QTimer>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace {

/* .ui 中的尺寸与字号均按 1080p 设计稿给出，运行时按屏幕高度等比缩放。 */
const int kDesignHeight = 1080;

/*
 * 板端有线口不一定是 eth0（本机为 eth1），无线口为 wlan0，
 * 因此遍历所有接口：优先 eth*，其次 wlan*，最后取任意非回环地址。
 */
QString localIp()
{
    struct ifaddrs *ifs = nullptr;
    QString best;
    int bestRank = 3;

    if (getifaddrs(&ifs) != 0)
        return QString();
    for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
        char buf[INET_ADDRSTRLEN] = { 0 };
        int rank;
        QString name;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                  buf, sizeof(buf));
        if (!buf[0])
            continue;
        name = QString::fromLatin1(ifa->ifa_name);
        if (name.startsWith(QStringLiteral("eth")))
            rank = 0;
        else if (name.startsWith(QStringLiteral("wlan")))
            rank = 1;
        else
            rank = 2;
        if (rank < bestRank) {
            best = QString::fromLatin1(buf);
            bestRank = rank;
        }
    }
    freeifaddrs(ifs);
    return best;
}

QString cpuSerial()
{
    QFile f(QStringLiteral("/proc/cpuinfo"));

    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    for (const QByteArray &line : f.readAll().split('\n')) {
        int colon;
        if (!line.startsWith("Serial"))
            continue;
        colon = line.indexOf(':');
        if (colon >= 0)
            return QString::fromLatin1(line.mid(colon + 1).trimmed());
    }
    return QString();
}

} /* namespace */

OverlayWidget::OverlayWidget(int screenW, int screenH, int planeId, int alpha8,
                             QWidget *parent)
    : QWidget(parent), ui(new Ui::OverlayWidget), m_screenW(screenW),
      m_screenH(screenH), m_planeId(planeId), m_alpha8(alpha8)
{
    ui->setupUi(this);
    setFixedSize(screenW, screenH);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);

    m_scale = screenH > 0 ? (qreal)screenH / kDesignHeight : 1.0;
    applyScale();

    const QString ip = localIp();
    ui->ipLabel->setText(ip.isEmpty() ? QStringLiteral("IP: --")
                                      : QStringLiteral("IP: %1").arg(ip));
    const QString sn = cpuSerial();
    ui->snLabel->setText(sn.isEmpty() ? QStringLiteral("SN: --")
                                      : QStringLiteral("SN: %1").arg(sn));
    ui->statusLabel->setText(QStringLiteral("UI Plane#%1 · α %2/255")
                                 .arg(m_planeId).arg(m_alpha8));

    connect(ui->welcomeBtn, &QPushButton::clicked, this,
            &OverlayWidget::toggleWelcome);

    m_clockTimer = new QTimer(this);
    m_clockTimer->start(500);
    connect(m_clockTimer, &QTimer::timeout, this, &OverlayWidget::updateClock);
    updateClock();
}

OverlayWidget::~OverlayWidget()
{
    delete ui;
}

void OverlayWidget::applyScale()
{
    const QList<QWidget *> scaled = {
        ui->welcomeLabel, ui->dateLabel, ui->weekLabel, ui->productLabel,
        ui->timeLabel,    ui->ipLabel,   ui->snLabel,   ui->statusLabel,
    };

    if (qFuzzyCompare(m_scale, qreal(1.0)))
        return; /* 1080p 下 .ui 内的设计值即最终值 */

    for (QWidget *w : scaled) {
        QFont f = w->font();
        if (f.pixelSize() > 0)
            f.setPixelSize(qRound(f.pixelSize() * m_scale));
        else if (f.pointSizeF() > 0)
            f.setPointSizeF(f.pointSizeF() * m_scale);
        w->setFont(f);
    }
    ui->bottomBar->setFixedHeight(qRound(ui->bottomBar->minimumHeight() * m_scale));
    ui->welcomeBtn->setFixedSize(qRound(ui->welcomeBtn->minimumWidth() * m_scale),
                                 qRound(ui->welcomeBtn->minimumHeight() * m_scale));
    ui->ipLabel->setFixedHeight(qRound(ui->ipLabel->maximumHeight() * m_scale));
    const QMargins m = ui->barLay->contentsMargins();
    ui->barLay->setContentsMargins(qRound(m.left() * m_scale), m.top(),
                                   qRound(m.right() * m_scale), m.bottom());
    ui->infoRow->setSpacing(qRound(ui->infoRow->spacing() * m_scale));
}

void OverlayWidget::updateClock()
{
    static const char *const kWeek[] = { "星期一", "星期二", "星期三", "星期四",
                                         "星期五", "星期六", "星期日" };
    const QDateTime now = QDateTime::currentDateTime();
    const QDate date = now.date();

    ui->dateLabel->setText(QStringLiteral("%1年%2月%3日")
                               .arg(date.year())
                               .arg(date.month(), 2, 10, QLatin1Char('0'))
                               .arg(date.day(), 2, 10, QLatin1Char('0')));
    ui->weekLabel->setText(QString::fromUtf8(kWeek[date.dayOfWeek() - 1]));
    ui->timeLabel->setText(now.time().toString(QStringLiteral("HH:mm")));
}

void OverlayWidget::toggleWelcome()
{
    ui->welcomeLabel->setVisible(!ui->welcomeLabel->isVisible());
}

void OverlayWidget::setFps(int uiFps, double videoFps)
{
    const QString video = videoFps < 0
        ? QStringLiteral("--")
        : QStringLiteral("%1").arg(videoFps, 0, 'f', 1);
    ui->statusLabel->setText(QStringLiteral(
        "UI Plane#%1 · α %2/255 · UI %3 fps · 视频 %4 fps")
        .arg(m_planeId)
        .arg(m_alpha8)
        .arg(uiFps)
        .arg(video));
}

void OverlayWidget::setPointer(int x, int y)
{
    if (m_pointerX == x && m_pointerY == y)
        return;
    m_pointerX = x;
    m_pointerY = y;
    update();
}

void OverlayWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    /* 自绘输入位置标记（offscreen 平台没有系统鼠标光标） */
    if (m_pointerX >= 0) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(255, 255, 255, 210), 2));
        p.setBrush(QColor(0, 150, 255, 150));
        p.drawEllipse(QPointF(m_pointerX, m_pointerY), 9, 9);
    }
}
