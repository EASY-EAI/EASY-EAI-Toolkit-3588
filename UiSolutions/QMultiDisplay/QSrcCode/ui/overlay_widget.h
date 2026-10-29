/**
 * overlay_widget — 半透明演示界面（版式沿用原 QMultiDisplay 设计）
 *
 * 布局、文字、颜色与设计尺寸都在 overlay_widget.ui（1080p 设计稿），代码只做
 * 按屏幕高度缩放、填充动态数据（时钟/IP/SN/统计）与按钮信号。根背景全透明
 * （露出下层摄像头画面），所有控件由 QWidget::render() 离屏画进 UI 图层显存。
 */

#ifndef OVERLAY_WIDGET_H
#define OVERLAY_WIDGET_H

#include <QWidget>

class QTimer;

namespace Ui {
class OverlayWidget;
}

class OverlayWidget : public QWidget
{
    Q_OBJECT

public:
    OverlayWidget(int screenW, int screenH, int planeId, int alpha8,
                  QWidget *parent = nullptr);
    ~OverlayWidget() override;

    void setFps(int uiFps, double videoFps);
    void setPointer(int x, int y);

private slots:
    void updateClock();
    void toggleWelcome();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void applyScale();

    Ui::OverlayWidget *ui;
    int m_screenW, m_screenH;
    int m_planeId, m_alpha8;
    qreal m_scale = 1.0;
    QTimer *m_clockTimer = nullptr;
    int m_pointerX = -1;
    int m_pointerY = -1;
};

#endif /* OVERLAY_WIDGET_H */
