#include <QApplication>
#include <QTextCodec>

#include <opencv2/opencv.hpp>

#include "system.h"

#include "mainWidget.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QTextCodec *codec = QTextCodec::codecForName("GBK");
    QTextCodec::setCodecForLocale(codec);

    mainWidget w;
    w.show();

    return a.exec();
}
