#ifndef __MAINWIDGET_H__
#define __MAINWIDGET_H__

#include <QWidget>
#include <QLabel>
#include <QTimer>

class QHBoxLayout;

namespace Ui {
class Widget;
}

class mainWidget : public QWidget
{
    Q_OBJECT

public:
    explicit mainWidget(QWidget *parent = 0);
    ~mainWidget();

private slots:
    void on_mpStandbyBtn_clicked();

private:
    void init();

    void getSysDateTime();
    void updateSysDateTime();
    void updateSysIpParam();
    void updateCPUSerial();

    QLabel *mpTextLab;
    QHBoxLayout *layout;

    int m_dateYear;
    int m_dateMonth;
    int m_dateDay;
    int m_dateWeek;
    int m_timeHour;
    int m_timeMin;
    int m_timeSecond;
    Ui::Widget *ui;
};

#endif // __MAINWIDGET_H__
